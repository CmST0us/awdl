use crate::{Operation, configuration::Configuration};
use anyhow::{Context, Result, bail, ensure};
use aware_common::bus;
use futures_util::{Stream, StreamExt};
use owly_airdrop::{
    driver::{
        mdns::Mdns,
        receiver::{Options, Receiver, ReceiverEvent},
        sender::{Event, Payload, Sender},
        tls::Identity,
    },
    proto::{
        mdns::{Burst, Service},
        sender::Outcome,
    },
};
use serde_json::{Value, json};
use std::{
    collections::{HashMap, HashSet},
    ffi::CString,
    net::{Ipv6Addr, SocketAddrV6},
    path::PathBuf,
    time::{Duration, Instant},
};
use tokio::sync::{mpsc, watch};

pub type Events = mpsc::UnboundedSender<Value>;
pub fn emit(events: &Events, event: Value) {
    let _ = events.send(event);
}
fn scope(name: &str) -> Result<u32> {
    let name = CString::new(name)?;
    let index = unsafe { libc::if_nametoindex(name.as_ptr()) };
    ensure!(index != 0, "AWDL 接口不存在");
    Ok(index)
}
async fn next<S: Stream<Item = zbus::Message> + Unpin>(signals: &mut S) -> Result<Value> {
    let message = signals.next().await.context("AWDL 事件通道关闭")?;
    let (data,): (String,) = message.body().deserialize()?;
    Ok(serde_json::from_str(&data)?)
}
async fn link<S: Stream<Item = zbus::Message> + Unpin>(
    signals: &mut S,
) -> Result<(u32, Ipv6Addr, Ipv6Addr)> {
    loop {
        let event = next(signals).await?;
        match event["event"].as_str() {
            Some("netdev-ready") => {
                return Ok((
                    scope(event["interface"].as_str().context("缺少接口")?)?,
                    event["local"]
                        .as_str()
                        .context("缺少本地地址")?
                        .split('/')
                        .next()
                        .unwrap()
                        .parse()?,
                    event["peer"].as_str().context("缺少对端地址")?.parse()?,
                ));
            }
            Some("released" | "stopped") => bail!("AWDL 链路已停止"),
            _ => {}
        }
    }
}
async fn resolve(
    index: u32,
    addr: Ipv6Addr,
    events: &Events,
) -> Result<owly_airdrop::proto::mdns::Found> {
    use owly_airdrop::proto::mdns::{BrowseEvent, Browser, Message};
    use socket2::{Domain, Protocol, Socket, Type};
    use std::net::SocketAddr;
    let socket = Socket::new(Domain::IPV6, Type::DGRAM, Some(Protocol::UDP))?;
    socket.set_only_v6(true)?;
    socket.set_reuse_address(true)?;
    socket.set_reuse_port(true)?;
    socket.bind(&SocketAddr::from((Ipv6Addr::UNSPECIFIED, 5353)).into())?;
    socket.set_unicast_hops_v6(255)?;
    socket.set_nonblocking(true)?;
    let socket = tokio::net::UdpSocket::from_std(socket.into())?;
    let peer = SocketAddrV6::new(addr, 5353, 0, index);
    let mut browser = Browser::new(Instant::now());
    let mut bytes = [0u8; 9000];
    loop {
        tokio::select! {
            _ = tokio::time::sleep_until(browser.next_timer().into()) => {
                if let Some(query) = browser.on_timer(Instant::now()) {
                    let mut message = Message::decode(&query)?;
                    for question in &mut message.questions { question.unicast = true; }
                    socket.send_to(&message.encode(), peer).await?;
                    emit(events, json!({"event":"service-query","address":peer.to_string()}));
                }
            }
            received = socket.recv_from(&mut bytes) => {
                let (len, source) = received?;
                if source.ip() != std::net::IpAddr::V6(addr) || source.port() != 5353 { continue; }
                if let Ok(message) = Message::decode(&bytes[..len]) { browser.handle(&message, Instant::now()); }
            }
        }
        while let Some(event) = browser.poll_event() {
            if let BrowseEvent::Found(found) = event {
                if found.addr == addr {
                    return Ok(found);
                }
            }
        }
    }
}
fn identifier() -> Result<String> {
    Ok(aware_common::unix::random::<6>()?
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect())
}
async fn send(
    cfg: &Configuration,
    connection: &zbus::Connection,
    name: &str,
    path: PathBuf,
    events: &Events,
) -> Result<()> {
    let file = std::fs::metadata(&path)?;
    ensure!(file.is_file(), "发送路径必须为普通文件");
    let bytes = file.len();
    let mut config = owly_airdrop::proto::sender::Config::new(&cfg.name, identifier()?);
    config.model_name = cfg.model.clone();
    config.ask_timeout = Duration::from_secs(cfg.accept_seconds);
    let sender = Sender::new(config)?;
    let radio = bus::proxy(connection, bus::NAME, bus::PATH, "org.wirelessd1.Awdl").await?;
    let discovery = async {
        let mut tried = HashSet::new();
        let mut candidates = radio.receive_signal("Event").await?;
        loop {
            let mut signals = radio.receive_signal("Event").await?;
            let _: () = radio.call("Acquire", &("",)).await?;
            let mac = loop {
                let event = next(&mut candidates).await?;
                if event["event"] == "peer" {
                    let mac = event["mac"]
                        .as_str()
                        .context("AWDL 对端缺少地址")?
                        .to_string();
                    if tried.insert(mac.clone()) {
                        break mac;
                    }
                }
            };
            emit(events, json!({"event":"candidate","mac":mac}));
            let attempt = async {
                let _: () = radio.call("SelectPeer", &(&mac,)).await?;
                let (index, _, address) = tokio::time::timeout(Duration::from_secs(cfg.service_seconds), link(&mut signals)).await??;
                let service = tokio::time::timeout(Duration::from_secs(cfg.service_seconds), resolve(index, address, events)).await??;
                let peer = SocketAddrV6::new(address, service.port, 0, index);
                let found = tokio::time::timeout(Duration::from_secs(cfg.service_seconds), sender.discover(peer)).await??.context("对端不可发现")?;
                emit(events, json!({"event":"discovered","name":found.computer_name,"model":found.model_name,"address":peer.to_string()}));
                ensure!(found.computer_name == name, "系统设备名不匹配");
                Ok::<_, anyhow::Error>(peer)
            }.await;
            match attempt {
                Ok(peer) => return Ok::<_, anyhow::Error>(peer),
                Err(error) => emit(
                    events,
                    json!({"event":"candidate-unavailable","mac":mac,"error":error.to_string()}),
                ),
            }
            let _: () = radio.call("Release", &()).await?;
        }
    };
    let peer = tokio::time::timeout(Duration::from_secs(cfg.discovery_seconds), discovery)
        .await
        .context("设备发现超时")??;
    emit(
        events,
        json!({"event":"target","name":name,"file_bytes":bytes}),
    );
    let (tx, mut rx) = mpsc::unbounded_channel();
    let payload = Payload::Files(vec![path]);
    let transfer = sender.send(peer, &payload, 0, Some(tx));
    tokio::pin!(transfer);
    let mut accepted: Option<Instant> = None;
    let mut compressed = 0;
    let mut reporting = Instant::now();
    let mut channel_open = true;
    loop {
        let expires = async {
            if let Some(start) = accepted {
                tokio::time::sleep_until((start + Duration::from_secs(cfg.upload_seconds)).into())
                    .await;
            } else {
                std::future::pending::<()>().await;
            }
        };
        tokio::select! {
            biased;
            event = rx.recv(), if channel_open => match event {
                Some(Event::Accepted) => { accepted=Some(Instant::now()); emit(events, json!({"event":"accepted"})); },
                Some(Event::Progress(progress)) => {
                    compressed = progress.total;
                    if reporting.elapsed() >= Duration::from_millis(500) || progress.sent == progress.total {
                        emit(events, json!({"event":"progress","sent":progress.sent,"total":progress.total,"seconds":accepted.map(|t|t.elapsed().as_secs_f64())}));
                        reporting = Instant::now();
                    }
                },
                None => channel_open=false,
            },
            result = &mut transfer => {
                ensure!(result? == Outcome::Delivered, "AirDrop 未交付文件");
                let seconds = accepted.context("缺少接受事件")?.elapsed().as_secs_f64();
                emit(events, json!({"event":"delivered","file_bytes":bytes,"compressed_bytes":compressed,"upload_seconds":seconds,
                    "file_MBps":bytes as f64/seconds/1e6,"compressed_MBps":compressed as f64/seconds/1e6}));
                return Ok(());
            },
            _ = expires => bail!("文件上传超过配置的失败期限"),
        }
    }
}
async fn receive(
    cfg: &Configuration,
    connection: &zbus::Connection,
    events: &Events,
    decisions: &mut mpsc::UnboundedReceiver<(u64, bool)>,
) -> Result<()> {
    std::fs::create_dir_all(&cfg.directory)?;
    let radio = bus::proxy(connection, bus::NAME, bus::PATH, "org.wirelessd1.Awdl").await?;
    {
        let mut signals = radio.receive_signal("Event").await?;
        let _: () = radio.call("Acquire", &("*",)).await?;
        emit(events, json!({"event":"listening"}));
        let (index, address, _) = link(&mut signals).await?;
        let identity = Identity::generate()?;
        let mut config = owly_airdrop::proto::receiver::Config::new(&cfg.name);
        config.model_name = cfg.model.clone();
        config.ask_timeout = Duration::from_secs(cfg.accept_seconds);
        let (receiver, mut incoming) = Receiver::bind(
            SocketAddrV6::new(address, cfg.port, 0, index),
            &identity,
            Options::new(config, cfg.directory.clone()),
        )
        .await?;
        let task = tokio::spawn(receiver.run());
        let _registration = Mdns::new(index)
            .with_burst(Some(Burst {
                count: cfg.announcement_count,
                interval: Duration::from_micros(cfg.announcement_interval_us),
                pause: Duration::from_millis(cfg.announcement_pause_ms),
            }))
            .register(Service {
                instance: identifier()?,
                host: cfg.hostname.clone(),
                port: cfg.port,
                addr: address,
                flags: 0x089,
            })
            .await?;
        emit(
            events,
            json!({"event":"ready","address":address.to_string(),"port":cfg.port,"name":cfg.name}),
        );
        let mut pending = HashMap::new();
        loop {
            tokio::select! {
                decision = decisions.recv() => {
                    if let Some((id, accept)) = decision { if let Some(request) = pending.remove(&id) {
                        let request: owly_airdrop::driver::receiver::Request = request;
                        if accept { request.accept(); } else { request.decline(); }
                    } }
                },
                event = incoming.recv() => match event {
                    Some(ReceiverEvent::Request(request)) => {
                        emit(events, json!({"event":"request","id":request.id,"peer":request.peer.to_string(),"offer":format!("{:?}",request.ask.offer)}));
                        if cfg.auto_accept {
                            emit(events, json!({"event":"answered","id":request.id,"accept":true}));
                            request.accept();
                        } else { pending.insert(request.id,request); }
                    },
                    Some(ReceiverEvent::Withdrawn{id}) => { pending.remove(&id); emit(events,json!({"event":"withdrawn","id":id})); },
                    Some(ReceiverEvent::Received{id,paths}) => { emit(events,json!({"event":"received","id":id,"paths":paths})); },
                    Some(ReceiverEvent::Failed{id,reason}) => { pending.remove(&id); emit(events,json!({"event":"receive-failed","id":id,"error":reason})); },
                    Some(event) => emit(events,json!({"event":"receiver","detail":format!("{event:?}")})),
                    None => bail!("AirDrop 接收器退出"),
                },
                event = next(&mut signals) => {
                    if event?["event"] == "released" { break; }
                },
            }
        }
        incoming.close();
        for id in pending.keys() {
            emit(events, json!({"event":"withdrawn","id":id}));
        }
        drop(pending);
        task.abort();
        let _ = task.await;
        Ok(())
    }
}
pub async fn run(
    operation: Operation,
    cfg: Configuration,
    connection: zbus::Connection,
    events: Events,
    decisions: &mut mpsc::UnboundedReceiver<(u64, bool)>,
    mut cancel: watch::Receiver<bool>,
) -> Result<()> {
    if *cancel.borrow() {
        return Ok(());
    }
    let work = async {
        match operation {
            Operation::Send(name, path) => send(&cfg, &connection, &name, path, &events).await,
            Operation::Receive => receive(&cfg, &connection, &events, decisions).await,
        }
    };
    tokio::select! {
        result = work => result,
        _ = cancel.changed() => {
            emit(&events, json!({"event":"cancelled"}));
            Ok(())
        },
    }
}

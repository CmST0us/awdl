use crate::{Operation, configuration::Configuration};
use anyhow::{Context, Result, bail, ensure};
use aware_common::bus;
use futures_util::{FutureExt, Stream, StreamExt};
use owly_airdrop::{
    driver::{
        mdns::Mdns,
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
        let mut peers: Vec<(String, u32)> = Vec::new();
        let mut excluded = HashSet::new();
        let mut candidates = radio.receive_signal("Event").await?;
        loop {
            let mut signals = radio.receive_signal("Event").await?;
            let _: () = radio.call("Acquire", &("",)).await?;
            // Keep transient failures eligible and give untried peers priority.
            // A verified different system name is excluded for this request.
            loop {
                let event = match next(&mut candidates).now_or_never() {
                    Some(result) => result?,
                    None if peers.iter().any(|(mac, _)| !excluded.contains(mac)) => break,
                    None => next(&mut candidates).await?,
                };
                if event["event"] == "peer" {
                    let mac = event["mac"]
                        .as_str()
                        .context("AWDL 对端缺少地址")?
                        .to_string();
                    if !peers.iter().any(|(known, _)| *known == mac) {
                        peers.push((mac, 0));
                    }
                }
            }
            let (mac, attempts) = peers
                .iter_mut()
                .filter(|(mac, _)| !excluded.contains(mac))
                .min_by_key(|(_, attempts)| *attempts)
                .context("没有可用的 AWDL 对端")?;
            *attempts += 1;
            let mac = mac.clone();
            emit(
                events,
                json!({"event":"candidate","mac":mac,"attempt":attempts}),
            );
            let attempt = async {
                let _: () = radio.call("SelectPeer", &(&mac,)).await?;
                let (index, _, address) = tokio::time::timeout(Duration::from_secs(cfg.service_seconds), link(&mut signals)).await??;
                let service = tokio::time::timeout(Duration::from_secs(cfg.service_seconds), resolve(index, address, events)).await??;
                let peer = SocketAddrV6::new(address, service.port, 0, index);
                let found = tokio::time::timeout(Duration::from_secs(cfg.service_seconds), sender.discover(peer)).await??.context("对端不可发现")?;
                emit(events, json!({"event":"discovered","name":found.computer_name,"model":found.model_name,"address":peer.to_string(),"flags":service.flags}));
                if found.computer_name != name {
                    excluded.insert(mac.clone());
                    bail!("系统设备名不匹配");
                }
                Ok::<_, anyhow::Error>((peer, service.flags))
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
    let (peer, flags) = tokio::time::timeout(Duration::from_secs(cfg.discovery_seconds), discovery)
        .await
        .context("设备发现超时")??;
    emit(
        events,
        json!({"event":"target","name":name,"file_bytes":bytes}),
    );
    let (tx, mut rx) = mpsc::unbounded_channel();
    let payload = Payload::Files(vec![path]);
    let transfer = sender.send(peer, &payload, flags, Some(tx));
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
    use crate::receiver::{Notice, Receiver};
    use owly_airdrop::proto::receiver::Answer;
    use std::sync::{Arc, atomic::AtomicU64};
    std::fs::create_dir_all(&cfg.directory)?;
    let radio = bus::proxy(connection, bus::NAME, bus::PATH, "org.wirelessd1.Awdl").await?;
    let identity = Identity::generate()?;
    let instance = identifier()?;
    let ids = Arc::new(AtomicU64::new(1));
    let mut target = "*".to_string();
    loop {
        let mut signals = radio.receive_signal("Event").await?;
        let _: () = radio.call("Acquire", &(&target,)).await?;
        emit(events, json!({"event":"listening"}));
        let (index, address, _) = link(&mut signals).await?;
        let (mut receiver, mut incoming) = Receiver::bind(
            SocketAddrV6::new(address, cfg.port, 0, index),
            &identity,
            cfg,
            ids.clone(),
            events.clone(),
        )
        .await?;
        let registration = Mdns::new(index)
            .with_burst(Some(Burst {
                count: cfg.announcement_count,
                interval: Duration::from_micros(cfg.announcement_interval_us),
                pause: Duration::from_millis(cfg.announcement_pause_ms),
            }))
            .register(Service {
                instance: instance.clone(),
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
        let mut pending = HashMap::<u64, tokio::sync::oneshot::Sender<Answer>>::new();
        let mut candidate = None;
        let mut released = false;
        loop {
            tokio::select! {
                biased;
                notice = incoming.recv() => match notice.context("AirDrop 接收器退出")? {
                    Notice::Request { id, peer, ask, reply } => {
                        emit(events,json!({"event":"request","id":id,"peer":peer.to_string(),"offer":format!("{:?}",ask.offer)}));
                        if cfg.auto_accept {
                            emit(events,json!({"event":"answered","id":id,"accept":true}));
                            let _ = reply.send(Answer::Accept);
                        } else { pending.insert(id, reply); }
                    }
                    Notice::Idle => { pending.retain(|_, reply| !reply.is_closed()); }
                },
                decision = decisions.recv() => {
                    if let Some((id, accept)) = decision {
                        if let Some(reply) = pending.remove(&id) {
                            let _ = reply.send(if accept { Answer::Accept } else { Answer::Decline });
                        }
                    }
                },
                result = receiver.step() => { result?; },
                event = next(&mut signals) => {
                    let event = event?;
                    if event["event"] == "sender-requested" {
                        candidate = Some(event["mac"].as_str().context("发送者缺少 MAC 地址")?.to_string());
                    }
                    if event["event"] == "released" { released = true; target = "*".into(); break; }
                },
            }
            if !receiver.busy() {
                if let Some(mac) = candidate.take() {
                    emit(events, json!({"event":"sender-selecting","mac":mac}));
                    target = mac;
                    break;
                }
            }
        }
        // Close all connections before releasing their native data station.
        incoming.close();
        for id in pending.keys() {
            emit(events, json!({"event":"withdrawn","id":id}));
        }
        drop(pending);
        receiver.close().await;
        drop(registration);
        if !released {
            let _: () = radio.call("Release", &()).await?;
        }
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

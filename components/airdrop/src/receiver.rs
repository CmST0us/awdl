use crate::{configuration::Configuration, transfer::Events};
use anyhow::{Context, Result, bail};
use owly_airdrop::{
    driver::{
        staging::Staging,
        tls::{self, Identity},
    },
    proto::{
        messages::AskRequest,
        receiver::{Answer, Config, Connection, Event},
    },
};
use serde_json::{Value, json};
use std::{
    net::SocketAddrV6,
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicU64, AtomicUsize, Ordering},
    },
    time::Duration,
};
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt},
    net::{TcpListener, TcpStream},
    sync::{mpsc, oneshot},
    task::JoinSet,
    time::{Instant, sleep_until, timeout},
};
use tokio_rustls::TlsAcceptor;

pub enum Notice {
    Request {
        id: u64,
        peer: SocketAddrV6,
        ask: AskRequest,
        reply: oneshot::Sender<Answer>,
    },
    Idle,
}

/// Owns every TLS connection so a radio handover can close them before STOP.
pub struct Receiver {
    listener: TcpListener,
    acceptor: TlsAcceptor,
    connections: JoinSet<()>,
    busy: Arc<AtomicUsize>,
    ids: Arc<AtomicU64>,
    notices: mpsc::UnboundedSender<Notice>,
    events: Events,
    cfg: Configuration,
    protocol: Arc<Config>,
}

impl Receiver {
    pub async fn bind(
        address: SocketAddrV6,
        identity: &Identity,
        cfg: &Configuration,
        ids: Arc<AtomicU64>,
        events: Events,
    ) -> Result<(Self, mpsc::UnboundedReceiver<Notice>)> {
        let mut protocol = Config::new(&cfg.name);
        protocol.model_name = cfg.model.clone();
        protocol.ask_timeout = Duration::from_secs(cfg.accept_seconds);
        let (notices, incoming) = mpsc::unbounded_channel();
        Ok((
            Self {
                listener: TcpListener::bind(address).await?,
                acceptor: TlsAcceptor::from(tls::server_config(identity)?),
                connections: JoinSet::new(),
                busy: Arc::default(),
                ids,
                notices,
                events,
                cfg: cfg.clone(),
                protocol: Arc::new(protocol),
            },
            incoming,
        ))
    }

    pub fn busy(&self) -> bool {
        self.busy.load(Ordering::Relaxed) != 0
    }

    pub async fn step(&mut self) -> Result<()> {
        tokio::select! {
            accepted = self.listener.accept() => {
                let (stream, address) = accepted?;
                let std::net::SocketAddr::V6(peer) = address else { bail!("AirDrop 要求 IPv6"); };
                let mut task = Session {
                    cfg: self.cfg.clone(), protocol: self.protocol.clone(),
                    acceptor: self.acceptor.clone(), peer, ids: self.ids.clone(),
                    busy: self.busy.clone(), notices: self.notices.clone(), events: self.events.clone(),
                    id: None, accepted: None, committed: None, bytes: 0, terminal: false,
                };
                self.connections.spawn(async move {
                    let result = task.run(stream).await;
                    if let Some(paths) = task.committed.take() {
                        let seconds = task.accepted.map(|t|t.elapsed().as_secs_f64());
                        task.report(json!({"event":"received","id":task.id,"paths":paths,"response_sent":result.is_ok(),
                            "file_bytes":task.bytes,"receive_seconds":seconds,"file_MBps":seconds.map(|t|task.bytes as f64/t/1e6)}));
                    } else if result.is_ok() && task.accepted.is_some() && !task.terminal {
                        task.report(json!({"event":"receive-failed","id":task.id,"error":"发送者在上传完成前关闭连接"}));
                    }
                    if let Err(error) = result {
                        task.report(json!({"event":if task.id.is_some(){"receive-failed"}else{"receiver-connection-failed"},
                            "id":task.id,"peer":peer.to_string(),"error":format!("{error:#}")}));
                    }
                });
            }
            result = self.connections.join_next(), if !self.connections.is_empty() => {
                result.context("接收连接任务缺失")??;
            }
        }
        Ok(())
    }

    pub async fn close(mut self) {
        self.connections.abort_all();
        while self.connections.join_next().await.is_some() {}
    }
}

struct Session {
    cfg: Configuration,
    protocol: Arc<Config>,
    acceptor: TlsAcceptor,
    peer: SocketAddrV6,
    ids: Arc<AtomicU64>,
    busy: Arc<AtomicUsize>,
    notices: mpsc::UnboundedSender<Notice>,
    events: Events,
    id: Option<u64>,
    accepted: Option<Instant>,
    committed: Option<Vec<PathBuf>>,
    bytes: u64,
    terminal: bool,
}

impl Drop for Session {
    fn drop(&mut self) {
        if self.id.is_some() {
            self.busy.fetch_sub(1, Ordering::Relaxed);
            let _ = self.notices.send(Notice::Idle);
        }
    }
}

impl Session {
    fn report(&self, event: Value) {
        let _ = self.events.send(event);
    }

    async fn run(&mut self, stream: TcpStream) -> Result<()> {
        stream.set_nodelay(true)?;
        let service_timeout = Duration::from_secs(self.cfg.service_seconds);
        let mut tls = timeout(service_timeout, self.acceptor.accept(stream))
            .await
            .context("TLS 握手超时")??;
        let mut protocol = Connection::new(self.protocol.clone());
        let mut buffer = vec![0; 64 << 10];
        let mut timer = None;
        let mut idle = Instant::now() + Duration::from_secs(self.cfg.receive_idle_seconds);
        let mut answer: Option<oneshot::Receiver<Answer>> = None;
        let mut staging: Option<Staging> = None;
        let mut reporting = Instant::now();
        let mut eof = false;
        loop {
            while let Some(event) = protocol.poll() {
                match event {
                    Event::Transmit(data) => {
                        timeout(service_timeout, async {
                            tls.write_all(&data).await?;
                            tls.flush().await
                        })
                        .await
                        .context("接收响应写入超时")??;
                    }
                    Event::SetTimer(after) => timer = after.map(|d| Instant::now() + d),
                    Event::Discovered(request) => self.report(
                        json!({"event":"receiver-discovered","peer":self.peer.to_string(),
                        "name":request.sender_computer_name,"model":request.sender_model_name}),
                    ),
                    Event::Ask(ask) => {
                        let id = self.ids.fetch_add(1, Ordering::Relaxed);
                        self.id = Some(id);
                        self.busy.fetch_add(1, Ordering::Relaxed);
                        let (reply, rx) = oneshot::channel();
                        answer = Some(rx);
                        let _ = self.notices.send(Notice::Request {
                            id,
                            peer: self.peer,
                            ask,
                            reply,
                        });
                    }
                    Event::AskWithdrawn => {
                        answer = None;
                        self.terminal = true;
                        self.report(json!({"event":"withdrawn","id":self.id}));
                    }
                    Event::Urls(urls) => {
                        self.terminal = true;
                        self.report(json!({"event":"urls","id":self.id,"urls":urls}))
                    }
                    Event::UploadStarted => {
                        staging = Some(Staging::new(&self.cfg.directory).await?)
                    }
                    Event::CreateDir(path) => {
                        staging
                            .as_mut()
                            .context("缺少接收暂存目录")?
                            .create_dir(&path)
                            .await?
                    }
                    Event::CreateFile(path) => {
                        staging
                            .as_mut()
                            .context("缺少接收暂存目录")?
                            .create_file(&path)
                            .await?
                    }
                    Event::Data(data) => {
                        staging
                            .as_mut()
                            .context("缺少接收暂存目录")?
                            .write(&data)
                            .await?;
                        self.bytes += data.len() as u64;
                        if reporting.elapsed() >= Duration::from_millis(500) {
                            self.report(
                                json!({"event":"receive-progress","id":self.id,"bytes":self.bytes,
                                "seconds":self.accepted.map(|t|t.elapsed().as_secs_f64())}),
                            );
                            reporting = Instant::now();
                        }
                    }
                    Event::EndFile => {
                        staging
                            .as_mut()
                            .context("缺少接收暂存目录")?
                            .end_file()
                            .await?
                    }
                    Event::UploadComplete => {
                        self.terminal = true;
                        let result = staging.take().context("缺少接收暂存目录")?.finish().await;
                        protocol
                            .finish_upload(result.as_ref().map(|_| ()).map_err(|e| e.to_string()));
                        match result {
                            Ok(paths) => self.committed = Some(paths),
                            Err(error) => self.report(json!({"event":"receive-failed","id":self.id,"error":error.to_string()})),
                        }
                    }
                    Event::UploadFailed(reason) => {
                        self.terminal = true;
                        staging = None;
                        self.report(json!({"event":"receive-failed","id":self.id,"error":reason}));
                    }
                    Event::Close => {
                        timeout(service_timeout, tls.shutdown()).await??;
                        return Ok(());
                    }
                }
            }
            if protocol.is_closed() {
                timeout(service_timeout, tls.shutdown()).await??;
                return Ok(());
            }
            let deadline = self
                .accepted
                .map(|t| t + Duration::from_secs(self.cfg.upload_seconds))
                .unwrap_or(idle);
            tokio::select! {
                read = tls.read(&mut buffer), if !eof => {
                    let count = read?;
                    if count == 0 { eof = true; protocol.receive_eof(); } else {
                        idle = Instant::now() + Duration::from_secs(self.cfg.receive_idle_seconds);
                        protocol.receive(&buffer[..count]);
                    }
                }
                reply = async { answer.as_mut().unwrap().await }, if answer.is_some() => {
                    answer = None;
                    let reply = reply.unwrap_or(Answer::Decline);
                    if reply == Answer::Accept { self.accepted = Some(Instant::now()); }
                    protocol.answer(reply);
                }
                _ = sleep_until(timer.unwrap_or(deadline)), if timer.is_some() => {
                    timer = None;
                    if answer.take().is_some() { self.report(json!({"event":"withdrawn","id":self.id})); }
                    protocol.timeout();
                }
                _ = sleep_until(deadline), if answer.is_none() => bail!("接收连接超过配置的失败期限"),
            }
        }
    }
}

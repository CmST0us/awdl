use crate::{
    api::{Error, Reply},
    radio::Radio,
};
use anyhow::{Context, Result, bail, ensure};
use aware_common::{bus, unix::interface_exists};
use futures_util::StreamExt;
use serde::Deserialize;
use serde_json::{Value, json};
use std::{
    process::Stdio,
    sync::{
        Arc, RwLock,
        atomic::{AtomicBool, Ordering},
    },
    time::Duration,
};
use tokio::{
    io::{AsyncBufReadExt, AsyncWriteExt, BufReader, Lines},
    process::{Child, ChildStdout},
    sync::mpsc,
};
use zbus::{message::Header, object_server::SignalEmitter};

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Configuration {
    name: String,
    start_seconds: u64,
    stop_seconds: u64,
}
enum Request {
    Acquire(String, String, Reply<()>),
    Select(String, String, Reply<()>),
    Release(String, Reply<()>),
}
struct Interface {
    sender: mpsc::Sender<Request>,
    state: Arc<RwLock<Value>>,
}
fn owner(header: &Header<'_>) -> Result<String, Error> {
    header
        .sender()
        .map(ToString::to_string)
        .ok_or_else(|| Error::AccessDenied("缺少发送者".into()))
}
impl Interface {
    async fn request(&self, request: impl FnOnce(Reply<()>) -> Request) -> Result<(), Error> {
        let (tx, rx) = tokio::sync::oneshot::channel();
        self.sender
            .send(request(tx))
            .await
            .map_err(|_| Error::NotReady("AWDL 已停止".into()))?;
        rx.await
            .map_err(|_| Error::NotReady("AWDL 已停止".into()))?
    }
}
#[zbus::interface(name = "org.wirelessd1.Awdl")]
impl Interface {
    async fn acquire(
        &self,
        target: String,
        #[zbus(header)] header: Header<'_>,
    ) -> Result<(), Error> {
        let sender = owner(&header)?;
        self.request(|reply| Request::Acquire(sender, target, reply))
            .await
    }
    async fn select_peer(
        &self,
        mac: String,
        #[zbus(header)] header: Header<'_>,
    ) -> Result<(), Error> {
        let sender = owner(&header)?;
        self.request(|reply| Request::Select(sender, mac, reply))
            .await
    }
    async fn release(&self, #[zbus(header)] header: Header<'_>) -> Result<(), Error> {
        let sender = owner(&header)?;
        self.request(|reply| Request::Release(sender, reply)).await
    }
    #[zbus(property)]
    fn status(&self) -> String {
        self.state.read().unwrap().to_string()
    }
    #[zbus(signal)]
    async fn event(emitter: &SignalEmitter<'_>, data: &str) -> zbus::Result<()>;
}
struct Session {
    child: Child,
    lines: Lines<BufReader<ChildStdout>>,
    cleanup: bool,
}
struct Manager {
    cfg: Configuration,
    program: String,
    interface: String,
    channel: u8,
    session: Option<Session>,
    owner: Option<String>,
    state: Arc<RwLock<Value>>,
    connection: zbus::Connection,
    owned: Arc<AtomicBool>,
    constraint: Option<String>,
}
fn valid_mac(mac: &str) -> bool {
    let parts: Vec<_> = mac.split(':').collect();
    parts.len() == 6
        && parts
            .iter()
            .all(|s| s.len() == 2 && s.bytes().all(|b| b.is_ascii_hexdigit()))
}
impl Manager {
    async fn emit(&self, event: Value) -> Result<()> {
        let emitter = SignalEmitter::new(&self.connection, bus::PATH)?;
        Interface::event(&emitter, &event.to_string()).await?;
        eprintln!("awdl: {event}");
        Ok(())
    }
    async fn line(&mut self, line: &str) -> Result<Option<String>> {
        let Ok(event) = serde_json::from_str::<Value>(line) else {
            eprintln!("awdl: {line}");
            return Ok(None);
        };
        let kind = event["event"].as_str().unwrap_or("").to_string();
        match kind.as_str() {
            "started" => *self.state.write().unwrap() = json!({"state":"discovering"}),
            "netdev-ready" => {
                *self.state.write().unwrap() = json!({"state":"connected","link":event})
            }
            "stopped" => {
                if let Some(session) = &mut self.session {
                    session.cleanup = event["cleanup"] == 0;
                }
            }
            _ => {}
        }
        self.emit(event).await?;
        Ok(Some(kind))
    }
    async fn acquire(&mut self, owner: String, target: String) -> Result<(), Error> {
        if let Some(reason) = &self.constraint {
            return Err(Error::NotSupported(reason.clone()));
        }
        if self.owner.is_some() {
            return Err(Error::Busy("AWDL 已被应用占用".into()));
        }
        if !(target.is_empty() || target == "*" || valid_mac(&target)) {
            return Err(Error::InvalidArgs("目标必须为空、* 或 MAC 地址".into()));
        }
        self.owned.store(true, Ordering::Release);
        let mut child = tokio::process::Command::new(&self.program)
            .args([
                &self.interface,
                &self.channel.to_string(),
                &target,
                &self.cfg.name,
            ])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()
            .map_err(|e| {
                self.owned.store(false, Ordering::Release);
                Error::Radio(e.to_string())
            })?;
        let lines = BufReader::new(child.stdout.take().unwrap()).lines();
        self.session = Some(Session {
            child,
            lines,
            cleanup: false,
        });
        self.owner = Some(owner);
        let start_duration = Duration::from_secs(self.cfg.start_seconds);
        let ready = async {
            loop {
                let line = self
                    .session
                    .as_mut()
                    .unwrap()
                    .lines
                    .next_line()
                    .await?
                    .context("AWDL 启动进程已退出")?;
                if self.line(&line).await?.as_deref() == Some("started") {
                    return Ok::<_, anyhow::Error>(());
                }
            }
        };
        let start = tokio::time::timeout(start_duration, ready).await;
        match start {
            Ok(Ok(())) => Ok(()),
            failure => {
                let error = format!("AWDL 启动失败: {failure:?}");
                if let Err(cleanup) = self.stop().await {
                    return Err(Error::Radio(format!("{error}; {cleanup:#}")));
                }
                Err(Error::Radio(error))
            }
        }
    }
    fn check_owner(&self, sender: &str) -> Result<(), Error> {
        if self.owner.as_deref() == Some(sender) {
            Ok(())
        } else {
            Err(Error::AccessDenied("应用未持有 AWDL".into()))
        }
    }
    async fn select(&mut self, sender: &str, mac: &str) -> Result<(), Error> {
        self.check_owner(sender)?;
        if !valid_mac(mac) {
            return Err(Error::InvalidArgs("无效 MAC 地址".into()));
        }
        if self.state.read().unwrap()["state"] == "connected" {
            return Err(Error::Busy("当前链路仍在使用".into()));
        }
        self.session
            .as_mut()
            .context("AWDL 进程未运行")?
            .child
            .stdin
            .as_mut()
            .context("AWDL 控制管道已关闭")?
            .write_all(format!("SELECT {mac}\n").as_bytes())
            .await
            .map_err(|e| Error::Radio(e.to_string()))
    }
    async fn stop(&mut self) -> Result<()> {
        if let Some(session) = &mut self.session {
            if session.child.try_wait()?.is_none() {
                if let Some(input) = session.child.stdin.as_mut() {
                    let _ = input.write_all(b"STOP\n").await;
                }
                let stop_duration = Duration::from_secs(self.cfg.stop_seconds);
                let drain = async {
                    while let Some(line) = self.session.as_mut().unwrap().lines.next_line().await? {
                        self.line(&line).await?;
                    }
                    self.session.as_mut().unwrap().child.wait().await?;
                    Ok::<_, anyhow::Error>(())
                };
                match tokio::time::timeout(stop_duration, drain).await {
                    Ok(result) => result?,
                    Err(_) => {
                        *self.state.write().unwrap() =
                            json!({"state":"failed","error":"AWDL 停止未完成"});
                        bail!("AWDL 停止未完成，资源所有权保留");
                    }
                }
            }
            if !self.session.as_ref().unwrap().cleanup {
                // A crashed control process may leave the native owner alive.
                let output = tokio::time::timeout(
                    Duration::from_secs(self.cfg.stop_seconds),
                    tokio::process::Command::new(&self.program)
                        .args([&self.interface, "stop"])
                        .output(),
                )
                .await??;
                ensure!(
                    output.status.success(),
                    "AWDL 原生 STOP 失败，资源所有权保留"
                );
            }
            ensure!(!interface_exists("awdl0")?, "AWDL 接口尚未释放");
        }
        self.session = None;
        self.owner = None;
        self.owned.store(false, Ordering::Release);
        *self.state.write().unwrap() = json!({"state":"idle"});
        self.emit(json!({"event":"released"})).await
    }
}
pub struct Service {
    shutdown: Option<tokio::sync::oneshot::Sender<()>>,
    task: Option<tokio::task::JoinHandle<Result<()>>>,
    finished: tokio::sync::watch::Receiver<Option<String>>,
    failure: Option<String>,
}
impl Service {
    pub async fn failed(&mut self) -> String {
        if self.finished.borrow().is_none() {
            let _ = self.finished.changed().await;
        }
        self.finished
            .borrow()
            .clone()
            .unwrap_or_else(|| "AWDL 管理器已退出".into())
    }
    pub async fn close(&mut self) -> Result<()> {
        if let Some(tx) = self.shutdown.take() {
            let _ = tx.send(());
        }
        if let Some(task) = self.task.take() {
            if let Err(error) = task.await.map_err(anyhow::Error::from).and_then(|r| r) {
                self.failure = Some(format!("{error:#}"));
            }
        }
        if let Some(error) = &self.failure {
            bail!("{error}");
        }
        Ok(())
    }
}
pub async fn attach(
    connection: &zbus::Connection,
    program: String,
    interface: String,
    channel: u8,
    owned: Arc<AtomicBool>,
    constraint: Option<String>,
) -> Result<Service> {
    let cfg: Configuration = aware_common::configuration::load(
        include_str!("../../../config/awdl.toml"),
        &match std::fs::read_to_string("/etc/wirelessd/awdl.toml") {
            Ok(text) => text,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => String::new(),
            Err(error) => return Err(error.into()),
        },
    )?;
    ensure!(
        !cfg.name.is_empty() && cfg.name.len() <= 63 && !cfg.name.contains(['\n', '\r']),
        "AWDL 名称必须为 1–63 字节单行文本"
    );
    ensure!(
        cfg.start_seconds > 0 && cfg.stop_seconds > 0,
        "AWDL 期限必须为正整数秒"
    );
    let (tx, rx) = mpsc::channel(16);
    let state = Arc::new(RwLock::new(json!({"state":"idle"})));
    connection
        .object_server()
        .at(
            bus::PATH,
            Interface {
                sender: tx,
                state: state.clone(),
            },
        )
        .await?;
    let manager = Manager {
        cfg,
        program,
        interface,
        channel,
        session: None,
        owner: None,
        state,
        connection: connection.clone(),
        owned,
        constraint,
    };
    // Subscribe before exposing the service so owner loss cannot precede the subscription.
    let dbus = zbus::fdo::DBusProxy::new(connection).await?;
    let owners = dbus.receive_name_owner_changed().await?;
    let (shutdown, stop) = tokio::sync::oneshot::channel();
    let (finished_tx, finished) = tokio::sync::watch::channel(None);
    let task = tokio::spawn(async move {
        let result = serve(manager, rx, owners, async {
            let _ = stop.await;
        })
        .await;
        let _ = finished_tx.send(Some(match &result {
            Ok(()) => "AWDL 管理器已退出".into(),
            Err(e) => format!("{e:#}"),
        }));
        result
    });
    Ok(Service {
        shutdown: Some(shutdown),
        task: Some(task),
        finished,
        failure: None,
    })
}
async fn serve(
    mut manager: Manager,
    mut rx: mpsc::Receiver<Request>,
    mut owners: zbus::fdo::NameOwnerChangedStream,
    shutdown: impl std::future::Future<Output = ()>,
) -> Result<()> {
    tokio::pin!(shutdown);
    let result = loop {
        let line = async {
            match manager.session.as_mut() {
                Some(session) => session.lines.next_line().await,
                None => std::future::pending().await,
            }
        };
        tokio::select! {
            _ = &mut shutdown => break Ok(()),
            request = rx.recv() => match request {
                Some(Request::Acquire(owner, target, reply)) => { let _ = reply.send(manager.acquire(owner, target).await); },
                Some(Request::Select(owner, mac, reply)) => { let _ = reply.send(manager.select(&owner, &mac).await); },
                Some(Request::Release(owner, reply)) => { let result = match manager.check_owner(&owner) { Ok(()) => manager.stop().await.map_err(Into::into), Err(error) => Err(error) }; let _ = reply.send(result); },
                None => break Ok(()),
            },
            line = line => match line {
                Ok(Some(line)) => { if let Err(error) = manager.line(&line).await { break Err(error); } },
                Ok(None) => { if let Err(error) = manager.stop().await { break Err(error); } },
                Err(error) => break Err(error.into()),
            },
            signal = owners.next() => {
                let Some(signal) = signal else { break Err(anyhow::anyhow!("系统总线断开")); };
                let args = signal.args()?;
                if args.new_owner().is_none() && manager.owner.as_deref() == Some(args.name().as_str()) {
                    if let Err(error) = manager.stop().await { break Err(error); }
                }
            }
        }
    };
    let cleanup = manager.stop().await;
    result.and(cleanup)
}
pub async fn run(mut radio: Radio, shutdown: impl std::future::Future<Output = ()>) -> Result<()> {
    let (program, interface, channel) = radio.prepare_awdl().await?;
    let connection = bus::connect().await?;
    let mut service = attach(
        &connection,
        program,
        interface,
        channel,
        radio.awdl_owned.clone(),
        None,
    )
    .await?;
    let result = async {
        connection.request_name(bus::NAME).await?;
        if std::env::var_os("NOTIFY_SOCKET").is_some() {
            let status = tokio::process::Command::new("systemd-notify").arg("--ready").status().await?;
            ensure!(status.success(), "AWDL 就绪通知失败");
        }
        tokio::pin!(shutdown);
        tokio::select! { _ = &mut shutdown => Ok(()), error = service.failed() => Err(anyhow::anyhow!(error)) }
    }.await;
    let cleanup = service.close().await;
    let radio_cleanup = radio.close().await;
    result.and(cleanup).and(radio_cleanup)
}

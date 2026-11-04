mod configuration;
mod receiver;
mod transfer;
use anyhow::{Context, Result, bail, ensure};
use aware_common::bus;
use configuration::Configuration;
use futures_util::StreamExt;
use serde_json::{Value, json};
use std::{
    path::PathBuf,
    process::Stdio,
    sync::{Arc, RwLock},
    time::Duration,
};
use tokio::{
    io::{AsyncBufReadExt, BufReader},
    process::Child,
    sync::{mpsc, oneshot, watch},
};
use zbus::object_server::SignalEmitter;
const NAME: &str = "org.airdrop1";
const PATH: &str = "/org/airdrop1";
const API: &str = "org.airdrop1.Manager";
#[derive(Clone)]
pub enum Operation {
    Send(String, PathBuf),
    Receive,
}
type Reply = oneshot::Sender<zbus::fdo::Result<u64>>;
enum Command {
    Start(Operation, Reply),
    Stop(Reply),
    Decide(u64, bool, Reply),
}
struct Interface {
    sender: mpsc::Sender<Command>,
    state: Arc<RwLock<Value>>,
}
impl Interface {
    async fn request(&self, make: impl FnOnce(Reply) -> Command) -> zbus::fdo::Result<u64> {
        let (tx, rx) = oneshot::channel();
        self.sender
            .send(make(tx))
            .await
            .map_err(|_| zbus::fdo::Error::Failed("AirDrop 已停止".into()))?;
        rx.await
            .map_err(|_| zbus::fdo::Error::Failed("AirDrop 已停止".into()))?
    }
}
#[zbus::interface(name = "org.airdrop1.Manager")]
impl Interface {
    async fn send(&self, name: String, path: String) -> zbus::fdo::Result<u64> {
        if name.is_empty() || !PathBuf::from(&path).is_absolute() {
            return Err(zbus::fdo::Error::InvalidArgs(
                "系统设备名不能为空，文件路径须为绝对路径".into(),
            ));
        }
        self.request(|reply| Command::Start(Operation::Send(name, path.into()), reply))
            .await
    }
    async fn receive(&self) -> zbus::fdo::Result<u64> {
        self.request(|reply| Command::Start(Operation::Receive, reply))
            .await
    }
    async fn stop(&self) -> zbus::fdo::Result<u64> {
        self.request(Command::Stop).await
    }
    async fn accept(&self, id: u64) -> zbus::fdo::Result<u64> {
        self.request(|reply| Command::Decide(id, true, reply)).await
    }
    async fn decline(&self, id: u64) -> zbus::fdo::Result<u64> {
        self.request(|reply| Command::Decide(id, false, reply))
            .await
    }
    #[zbus(property)]
    fn status(&self) -> String {
        self.state.read().unwrap().to_string()
    }
    #[zbus(signal)]
    async fn event(emitter: &SignalEmitter<'_>, data: &str) -> zbus::Result<()>;
}
struct Job {
    id: u64,
    cancel: watch::Sender<bool>,
    decisions: mpsc::UnboundedSender<(u64, bool)>,
    task: tokio::task::JoinHandle<Result<()>>,
    ble: Option<Child>,
}
async fn ble(index: u16) -> Result<Child> {
    let mut child = tokio::process::Command::new("python3")
        .args([
            "/usr/local/libexec/airdrop/ble.py",
            "--index",
            &index.to_string(),
        ])
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .spawn()?;
    let mut lines = BufReader::new(child.stdout.take().unwrap()).lines();
    let ready = tokio::time::timeout(Duration::from_secs(4), lines.next_line()).await;
    if !matches!(ready,Ok(Ok(Some(ref line))) if line=="AIRDROP_BLE_READY") {
        if let Some(pid) = child.id() {
            unsafe {
                libc::kill(pid as i32, libc::SIGTERM);
            }
        }
        let _ = tokio::time::timeout(Duration::from_secs(4), child.wait()).await;
        bail!("专用 HCI 的 AirDrop 广播未就绪");
    }
    Ok(child)
}
async fn cleanup(connection: &zbus::Connection, job: &mut Job) -> Result<()> {
    let released: zbus::Result<()> = async {
        let radio = bus::proxy(connection, bus::NAME, bus::PATH, "org.wirelessd1.Awdl").await?;
        radio.call("Release", &()).await
    }
    .await;
    let mut result = match released {
        Ok(()) => Ok(()),
        Err(zbus::Error::MethodError(name, _, _))
            if name.as_str() == "org.freedesktop.DBus.Error.AccessDenied" =>
        {
            Ok(())
        }
        Err(error) => Err(error.into()),
    };
    if let Err(error) = stop_ble(job).await {
        result = Err(error);
    }
    result
}
async fn stop_ble(job: &mut Job) -> Result<()> {
    if let Some(child) = &mut job.ble {
        if let Some(pid) = child.id() {
            unsafe {
                libc::kill(pid as i32, libc::SIGTERM);
            }
        }
        match tokio::time::timeout(Duration::from_secs(4), child.wait()).await {
            Ok(Ok(status)) if status.success() => {
                job.ble = None;
            }
            other => bail!("BLE 关闭未完成: {other:?}"),
        }
    }
    Ok(())
}

async fn emit(
    connection: &zbus::Connection,
    state: &Arc<RwLock<Value>>,
    mut event: Value,
    job: u64,
) -> Result<()> {
    event["job"] = json!(job);
    {
        let mut state = state.write().unwrap();
        state["last_event"] = event.clone();
        if event["event"] == "request" {
            if !state["offers"].is_object() {
                state["offers"] = json!({});
            }
            state["offers"][event["id"].to_string()] = event.clone();
        }
        if ["withdrawn", "received", "receive-failed", "answered"]
            .contains(&event["event"].as_str().unwrap_or(""))
        {
            if let Some(offers) = state["offers"].as_object_mut() {
                offers.remove(&event["id"].to_string());
            }
        }
    }
    println!("{event}");
    Interface::event(&SignalEmitter::new(connection, PATH)?, &event.to_string()).await?;
    Ok(())
}
async fn serve(cfg: Configuration) -> Result<()> {
    rustls::crypto::ring::default_provider()
        .install_default()
        .map_err(|_| anyhow::anyhow!("TLS provider"))?;
    let connection = bus::connect().await?;
    let radio_connection = bus::connect().await?;
    let state = Arc::new(RwLock::new(json!({"state":"idle","offers":{}})));
    let (tx, mut commands) = mpsc::channel(16);
    connection
        .object_server()
        .at(
            PATH,
            Interface {
                sender: tx,
                state: state.clone(),
            },
        )
        .await?;
    connection.request_name(NAME).await?;
    let (events, mut incoming) = mpsc::unbounded_channel();
    let mut job: Option<Job> = None;
    let mut held: Option<Job> = None;
    let mut next_id = 0;
    let shutdown = aware_common::unix::shutdown()?;
    tokio::pin!(shutdown);
    let mut stopping = false;
    let mut failure = None;
    loop {
        let finished = async {
            match job.as_mut() {
                Some(job) => (&mut job.task).await,
                None => std::future::pending().await,
            }
        };
        tokio::select! {
            _=&mut shutdown, if !stopping=>{stopping=true;if let Some(job)=&job{let _=job.cancel.send(true);}else{break;}},
            result=finished=>{
                let mut completed=job.take().unwrap();
                // The dedicated runtime has dropped all receiver connections before native STOP.
                let released=cleanup(&radio_connection,&mut completed).await;
                let cleanup_failed=released.is_err();
                let result=result.map_err(anyhow::Error::from).and_then(|r|r).and(released);
                while let Ok(event)=incoming.try_recv(){emit(&connection,&state,event,completed.id).await?;}
                *state.write().unwrap()=json!({"state":if result.is_ok(){"idle"}else{"failed"},"offers":{}});
                emit(&connection,&state,json!({"event":"finished","ok":result.is_ok(),"error":result.as_ref().err().map(|e|format!("{e:#}"))}),completed.id).await?;
                if cleanup_failed { held=Some(completed); }
                if let Err(error)=result{if stopping{failure=Some(error);}}
                if stopping{break;}
            },
            event=incoming.recv()=>{if let Some(event)=event{
                if event["event"] == "accepted" {
                    if let Some(active) = &mut job {
                        if let Err(error) = stop_ble(active).await {
                            emit(&connection,&state,json!({"event":"ble-stop-failed","error":error.to_string()}),active.id).await?;
                        }
                    }
                }
                emit(&connection,&state,event,job.as_ref().map_or(0,|j|j.id)).await?;
            }},
            command=commands.recv(), if !stopping=>match command {
                Some(Command::Start(operation,reply))=>{
                    if job.is_some()||held.is_some(){let _=reply.send(Err(zbus::fdo::Error::Failed("已有 AirDrop 操作，请先停止".into())));continue;}
                    let advertisement=if matches!(operation,Operation::Send(..)) {
                        match ble(cfg.hci).await{Ok(child)=>Some(child),Err(error)=>{let _=reply.send(Err(zbus::fdo::Error::Failed(error.to_string())));continue;}}
                    }else{None};
                    next_id+=1;
                    let(cancel,cancelled)=watch::channel(false);
                    let(decisions,mut decision_rx)=mpsc::unbounded_channel();
                    let config=cfg.clone();let connection=radio_connection.clone();let output=events.clone();
                    let receive=matches!(operation,Operation::Receive);
                    let task=tokio::task::spawn_blocking(move||{
                        loop {
                            let runtime=tokio::runtime::Builder::new_current_thread().enable_all().build()?;
                            let result=runtime.block_on(transfer::run(operation.clone(),config.clone(),connection.clone(),output.clone(),&mut decision_rx,cancelled.clone()));
                            drop(runtime);
                            if !receive||result.is_err()||*cancelled.borrow(){return result;}
                        }
                    });
                    job=Some(Job{id:next_id,cancel,decisions,task,ble:advertisement});
                    *state.write().unwrap()=json!({"state":if receive{"receiving"}else{"sending"},"job":next_id,"offers":{}});
                    let _=reply.send(Ok(next_id));
                },
                Some(Command::Stop(reply))=>{
                    if let Some(job)=&job{let _=job.cancel.send(true);let _=reply.send(Ok(job.id));}
                    else if let Some(mut resources)=held.take(){
                        match cleanup(&radio_connection,&mut resources).await{
                            Ok(())=>{*state.write().unwrap()=json!({"state":"idle","offers":{}});let _=reply.send(Ok(resources.id));emit(&connection,&state,json!({"event":"finished","ok":true}),resources.id).await?;},
                            Err(error)=>{held=Some(resources);let _=reply.send(Err(zbus::fdo::Error::Failed(error.to_string())));},
                        }
                    }else{let _=reply.send(Err(zbus::fdo::Error::Failed("没有活动操作".into())));}
                },
                Some(Command::Decide(id,accept,reply))=>{
                    let pending=state.read().unwrap()["offers"].get(id.to_string()).is_some();
                    if pending { if let Some(job)=&job{let _=job.decisions.send((id,accept));let _=reply.send(Ok(job.id));emit(&connection,&state,json!({"event":"answered","id":id,"accept":accept}),job.id).await?;} }
                    else{let _=reply.send(Err(zbus::fdo::Error::InvalidArgs("请求不存在或已撤回".into())));}
                },
                None=>break,
            },
        }
    }
    if let Some(mut resources) = held {
        cleanup(&radio_connection, &mut resources).await?;
    }
    if let Some(error) = failure {
        Err(error)
    } else {
        Ok(())
    }
}
async fn run() -> Result<()> {
    let mut args = std::env::args().skip(1);
    let mut config = "/etc/airdrop/config.toml".to_string();
    let mut operation = args.next().unwrap_or_else(|| "help".into());
    if operation == "--config" {
        config = args.next().context("缺少配置路径")?;
        operation = args.next().context("缺少命令")?;
    }
    if ["help", "--help", "-h"].contains(&operation.as_str()) {
        println!(
            "用法: airdrop [--config PATH] serve | status | monitor | receive | send NAME FILE | accept ID | decline ID | stop"
        );
        return Ok(());
    }
    if operation == "serve" {
        return serve(Configuration::load(&config)?).await;
    }
    let connection = bus::connect().await?;
    let proxy = bus::proxy(&connection, NAME, PATH, API).await?;
    if operation == "status" {
        let status: String = proxy.get_property("Status").await?;
        println!("{status}");
        return Ok(());
    }
    let mut signals = proxy.receive_signal("Event").await?;
    let job = match operation.as_str() {
        "send" => {
            let name = args.next().context("缺少系统设备名")?;
            let path = PathBuf::from(args.next().context("缺少文件路径")?).canonicalize()?;
            Some(
                proxy
                    .call::<_, _, u64>("Send", &(name, path.to_string_lossy().as_ref()))
                    .await?,
            )
        }
        "receive" => {
            let id: u64 = proxy.call("Receive", &()).await?;
            println!("{}", json!({"job":id,"state":"receiving"}));
            return Ok(());
        }
        "stop" => Some(proxy.call::<_, _, u64>("Stop", &()).await?),
        "accept" | "decline" => {
            let id: u64 = args.next().context("缺少请求 ID")?.parse()?;
            let _: u64 = proxy
                .call(
                    if operation == "accept" {
                        "Accept"
                    } else {
                        "Decline"
                    },
                    &(id,),
                )
                .await?;
            return Ok(());
        }
        "monitor" => None,
        _ => bail!("未知命令 {operation}"),
    };
    while let Some(message) = signals.next().await {
        let (data,): (String,) = message.body().deserialize()?;
        let event: Value = serde_json::from_str(&data)?;
        if job.is_none() || event["job"].as_u64() == job {
            println!("{data}");
        }
        if job.is_some() && event["job"].as_u64() == job && event["event"] == "finished" {
            ensure!(
                event["ok"] == true || operation == "stop",
                "{}",
                event["error"]
            );
            return Ok(());
        }
    }
    bail!("AirDrop 服务事件通道关闭")
}
#[tokio::main]
async fn main() {
    aware_common::unix::restrict_permissions();
    if let Err(error) = run().await {
        eprintln!("{error:#}");
        std::process::exit(1);
    }
}

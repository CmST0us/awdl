mod configuration;
mod files;
mod io;
mod publisher;

use anyhow::Result;
use std::sync::Arc;
use tokio::{net::TcpListener, sync::watch, task::JoinSet};

async fn listener(port: u16) -> Result<TcpListener> {
    #[cfg(target_os = "linux")]
    {
        use socket2::{Domain, Protocol, Socket, Type};
        let socket = Socket::new(Domain::IPV6, Type::STREAM, Some(Protocol::TCP))?;
        socket.set_only_v6(true)?;
        socket.set_reuse_address(true)?;
        socket.bind_device(Some(b"ndi0"))?;
        socket.bind(
            &std::net::SocketAddrV6::new(std::net::Ipv6Addr::UNSPECIFIED, port, 0, 0).into(),
        )?;
        socket.listen(128)?;
        socket.set_nonblocking(true)?;
        Ok(TcpListener::from_std(socket.into())?)
    }
    #[cfg(not(target_os = "linux"))]
    {
        let _ = port;
        anyhow::bail!("文件服务运行需要 Linux");
    }
}
async fn run(cfg: configuration::Configuration) -> Result<()> {
    let directory = files::Files::directory(&cfg)?;
    let sessions = Arc::new(publisher::Sessions::default());
    let (failure, mut failed) = watch::channel(None);
    let (mut publisher, signals) = publisher::Publisher::open(sessions.clone(), failure).await?;
    let listener = listener(cfg.port).await?;
    publisher.register(&cfg).await?;
    let publisher = Arc::new(publisher);
    let file_server = Arc::new(files::Files::new(cfg.clone(), publisher.clone(), directory));
    let watcher = {
        let publisher = publisher.clone();
        tokio::spawn(async move {
            if let Err(e) = publisher.watch(signals).await {
                publisher.fail(e);
            }
        })
    };
    let mut workers = JoinSet::new();
    let shutdown = aware_common::unix::shutdown()?;
    tokio::pin!(shutdown);
    eprintln!(
        "HTTP 文件服务已启动 service={} port={} files={} io_timeout={}",
        cfg.service, cfg.port, cfg.files, cfg.io_timeout
    );
    let result = loop {
        tokio::select! {
            _ = &mut shutdown => break Ok(()),
            _ = failed.changed() => break Err(anyhow::anyhow!(failed.borrow().clone().unwrap_or_else(|| "文件服务通知通道已关闭".into()))),
            accepted = listener.accept() => {
                let (socket, remote) = match accepted { Ok(result) => result, Err(e) => break Err(e.into()) };
                let files = file_server.clone(); let sessions = sessions.clone();
                let (id, mut cancelled) = sessions.track(remote.ip());
                workers.spawn(async move {
                    let socket = hyper_util::rt::TokioIo::new(io::IdleIo::new(socket, files.cfg.timeout()));
                    let timeout = files.cfg.timeout();
                    let service = hyper::service::service_fn(move |request| files.clone().serve(request, remote.ip()));
                    let mut builder = hyper::server::conn::http1::Builder::new();
                    builder.keep_alive(true).max_header_size(64 * 1024).max_buf_size(aware_common::TRANSFER_BLOCK + 64 * 1024).timer(hyper_util::rt::TokioTimer::new()).header_read_timeout(timeout);
                    tokio::select! {
                        _ = cancelled.changed() => {},
                        result = builder.serve_connection(socket, service) => { if let Err(e) = result { eprintln!("HTTP 连接 address={} error={e}", remote.ip()); } }
                    }
                    sessions.untrack(id);
                });
            }
            Some(result) = workers.join_next(), if !workers.is_empty() => { if let Err(e) = result { break Err(e.into()); } }
        }
    };
    drop(listener);
    sessions.stop();
    while workers.join_next().await.is_some() {}
    watcher.abort();
    let _ = watcher.await;
    result
}
#[tokio::main]
async fn main() {
    aware_common::unix::restrict_permissions();
    let result = match configuration::Configuration::load() {
        Ok(Some(cfg)) => run(cfg).await,
        Ok(None) => Ok(()),
        Err(e) => Err(e),
    };
    if let Err(e) = result {
        eprintln!("{e:#}");
        std::process::exit(1);
    }
}

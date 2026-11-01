use anyhow::{Context, Result, bail};
use std::{
    path::{Path, PathBuf},
    time::Duration,
};
use tokio::{net::UnixDatagram, time::timeout};

pub struct Control {
    pub socket: UnixDatagram,
    path: PathBuf,
    pending: bool,
    timeout: Duration,
}

impl Control {
    pub fn open(remote: &Path, local: &Path, seconds: u32) -> Result<Self> {
        match std::fs::remove_file(local) {
            Err(e) if e.kind() != std::io::ErrorKind::NotFound => return Err(e.into()),
            _ => {}
        }
        let result = Self {
            socket: UnixDatagram::bind(local)?,
            path: local.to_owned(),
            pending: false,
            timeout: Duration::from_secs(u64::from(seconds)),
        };
        result.socket.connect(remote)?;
        Ok(result)
    }

    pub async fn request(&mut self, command: &str) -> Result<String> {
        let mut reply = vec![0; 65536];
        if self.pending {
            timeout(self.timeout, self.socket.recv(&mut reply))
                .await
                .context("hostap 尚未回复上一条超时命令")??;
            self.pending = false;
            eprintln!("已收回 hostap 超时命令的回复");
        }
        self.socket.send(command.as_bytes()).await?;
        self.pending = true;
        let length = timeout(self.timeout, self.socket.recv(&mut reply))
            .await
            .context("等待 hostap 回复超时")??;
        self.pending = false;
        let reply = String::from_utf8(reply[..length].to_vec())?
            .trim_end()
            .to_owned();
        if reply.starts_with("FAIL") || reply.starts_with("UNKNOWN") {
            bail!(
                "{}: {}",
                command.split_whitespace().next().unwrap_or(""),
                reply
            );
        }
        Ok(reply)
    }
}

impl Drop for Control {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.path);
    }
}

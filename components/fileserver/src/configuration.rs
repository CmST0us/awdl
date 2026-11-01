use anyhow::{Context, Result, bail, ensure};
use serde::Deserialize;
use std::{path::Path, time::Duration};

#[derive(Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Configuration {
    pub service: String,
    pub files: String,
    pub port: u16,
    pub io_timeout: u32,
}
impl Default for Configuration {
    fn default() -> Self {
        aware_common::configuration::load(
            include_str!("../../../config/fileserver.toml"),
            include_str!("../../../config/fileserver-discovery.toml"),
        )
        .expect("版本化文件服务默认配置")
    }
}
impl Configuration {
    pub fn load() -> Result<Option<Self>> {
        let mut args = std::env::args().skip(1);
        let mut overrides = vec![];
        let mut path = "/etc/fileserver/config.toml".to_string();
        let mut explicit = false;
        while let Some(arg) = args.next() {
            if ["--help", "-h"].contains(&arg.as_str()) {
                println!(
                    "用法: fileserver [--config PATH] [--service NAME] [--files DIRECTORY] [--port PORT] [--io-timeout SECONDS]\n--config= 仅使用命令行和默认值"
                );
                return Ok(None);
            }
            let (name, inline) = arg
                .split_once('=')
                .map_or((arg.as_str(), None), |(k, v)| (k, Some(v.to_string())));
            ensure!(
                ["--config", "--service", "--files", "--port", "--io-timeout"].contains(&name),
                "未知参数 {arg}"
            );
            let value = inline
                .or_else(|| args.next())
                .ok_or_else(|| anyhow::anyhow!("{name} 缺少参数"))?;
            if name == "--config" {
                path = value;
                explicit = true;
            } else {
                overrides.push((name.to_string(), value));
            }
        }
        let mut cfg = Self::default();
        if !path.is_empty() {
            match std::fs::read_to_string(&path) {
                Ok(text) => {
                    cfg = aware_common::configuration::load(
                        &format!(
                            "{}\n{}",
                            include_str!("../../../config/fileserver.toml"),
                            include_str!("../../../config/fileserver-discovery.toml")
                        ),
                        &text,
                    )
                    .with_context(|| format!("解析文件服务配置 {path}"))?;
                    eprintln!("文件服务配置: {path}");
                }
                Err(e) if !explicit && e.kind() == std::io::ErrorKind::NotFound => {}
                Err(e) => return Err(e.into()),
            }
        }
        for (name, value) in overrides {
            match name.as_str() {
                "--service" => cfg.service = value,
                "--files" => cfg.files = value,
                "--port" => cfg.port = value.parse()?,
                "--io-timeout" => cfg.io_timeout = value.parse()?,
                _ => bail!("未知参数 {name}"),
            }
        }
        ensure!(
            Path::new(&cfg.files).is_absolute()
                && cfg.port > 0
                && cfg.io_timeout > 0
                && cfg.io_timeout <= i32::MAX as u32,
            "files 必须为绝对路径，port 为 1–65535，io_timeout 为有效正整数"
        );
        ensure!(
            cfg.service.ends_with("._tcp"),
            "service 必须为 TCP 服务名，以 ._tcp 结尾"
        );
        Ok(Some(cfg))
    }
    pub fn timeout(&self) -> Duration {
        Duration::from_secs(u64::from(self.io_timeout))
    }
}

use anyhow::{Context, Result, ensure};
use serde::Deserialize;
use std::path::PathBuf;
#[derive(Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Configuration {
    pub name: String,
    pub model: String,
    pub hostname: String,
    pub directory: PathBuf,
    pub receive_write_buffer_bytes: usize,
    pub port: u16,
    pub auto_accept: bool,
    pub hci: u16,
    pub discovery_seconds: u64,
    pub service_seconds: u64,
    pub accept_seconds: u64,
    pub receive_idle_seconds: u64,
    pub upload_seconds: u64,
    pub announcement_count: u32,
    pub announcement_interval_us: u64,
    pub announcement_pause_ms: u64,
}
impl Configuration {
    pub fn load(path: &str) -> Result<Self> {
        let value: Self = aware_common::configuration::load(
            include_str!("../../../config/airdrop.toml"),
            &std::fs::read_to_string(path).with_context(|| format!("读取 {path}"))?,
        )?;
        ensure!(
            !value.name.is_empty() && !value.model.is_empty(),
            "AirDrop 名称和型号不能为空"
        );
        ensure!(
            !value.hostname.is_empty()
                && value.hostname.len() <= 63
                && value
                    .hostname
                    .bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b == b'-'),
            "hostname 必须为 1–63 字节的 DNS 标签"
        );
        ensure!(
            value.directory.is_absolute() && value.port != 0,
            "接收目录必须为绝对路径，端口不能为 0"
        );
        ensure!(
            value.receive_write_buffer_bytes > 0,
            "接收写入缓冲必须为正整数字节"
        );
        ensure!(
            [
                value.discovery_seconds,
                value.service_seconds,
                value.accept_seconds,
                value.receive_idle_seconds,
                value.upload_seconds,
                value.announcement_count as u64,
                value.announcement_interval_us,
                value.announcement_pause_ms
            ]
            .iter()
            .all(|n| *n > 0),
            "期限与广播参数必须为正整数"
        );
        Ok(value)
    }
}

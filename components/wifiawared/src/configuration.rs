use anyhow::{Context, Result, ensure};
use serde::Deserialize;
use std::path::PathBuf;

#[derive(Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Configuration {
    pub state: PathBuf,
    pub runtime: PathBuf,
    pub supplicant: String,
    pub frequency: u32,
    pub pci: String,
    pub radio_profile: Option<PathBuf>,
    pub identity: Identity,
    pub timeouts: Timeouts,
    pub retention: Retention,
}

#[derive(Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Identity {
    pub vendor_name: String,
    pub model_name: String,
    pub pairing_name: String,
}
#[derive(Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Timeouts {
    pub control_reply_seconds: u32,
    pub hostap_ready_seconds: u32,
    pub cluster_join_seconds: u32,
    pub authentication_seconds: u32,
    pub ndp_setup_seconds: u32,
    pub ndp_end_seconds: u32,
    pub hostap_stop_seconds: u32,
}
#[derive(Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Retention {
    pub inactive_peer_seconds: u32,
}

impl Configuration {
    pub fn load(path: &str) -> Result<Self> {
        let mut cfg: Self = aware_common::configuration::load(
            include_str!("../../../config/wifiawared.toml"),
            &std::fs::read_to_string(path).with_context(|| format!("读取无线配置 {path}"))?,
        )
        .with_context(|| format!("解析无线配置 {path}"))?;
        ensure!(
            cfg.frequency > 0,
            "frequency 必须为正整数 MHz；信道支持范围由驱动确认"
        );
        ensure!(
            cfg.state.is_absolute() && cfg.runtime.is_absolute() && !cfg.supplicant.is_empty(),
            "目录必须是绝对路径，supplicant 不能为空"
        );
        if !cfg.pci.is_empty() {
            let parts: Vec<_> = cfg.pci.split([':', '.']).collect();
            ensure!(
                parts.len() == 4
                    && parts
                        .iter()
                        .zip([4, 2, 2, 1])
                        .all(|(s, n)| s.len() == n && s.bytes().all(|b| b.is_ascii_hexdigit())),
                "pci 必须是完整 PCI 地址，例如 0000:03:00.0"
            );
            ensure!(
                u8::from_str_radix(parts[2], 16)? <= 31 && u8::from_str_radix(parts[3], 16)? <= 7,
                "PCI 设备或功能号无效"
            );
            cfg.pci.make_ascii_lowercase();
        }
        let t = &cfg.timeouts;
        ensure!(
            [
                t.control_reply_seconds,
                t.hostap_ready_seconds,
                t.cluster_join_seconds,
                t.authentication_seconds,
                t.ndp_setup_seconds,
                t.ndp_end_seconds,
                t.hostap_stop_seconds,
                cfg.retention.inactive_peer_seconds,
            ]
            .iter()
            .all(|s| *s > 0),
            "期限必须为正整数秒"
        );
        ensure!(
            cfg.radio_profile.as_ref().is_none_or(|p| p.is_absolute()),
            "radio_profile 必须为绝对路径"
        );
        for name in [
            &cfg.identity.vendor_name,
            &cfg.identity.model_name,
            &cfg.identity.pairing_name,
        ] {
            ensure!(!name.is_empty(), "identity 名称不能为空");
        }
        let identity_bytes = cfg.identity.vendor_name.len()
            + cfg.identity.model_name.len()
            + cfg.identity.pairing_name.len()
            + "vendorName=modelName=pairingName=".len()
            + 3;
        ensure!(
            identity_bytes <= 255,
            "identity 信息总长度超过 SSI 的 255 字节限制"
        );
        eprintln!("无线运行配置: {path}");
        Ok(cfg)
    }
}

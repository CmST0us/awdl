use crate::configuration::Configuration;
use anyhow::{Context, Result, ensure};
use serde::Deserialize;
use std::path::Path;

#[path = "catalog.rs"]
mod catalog;
pub use catalog::Hardware as Backend;
#[path = "be200/radio.rs"]
mod be200;
#[path = "rtl8852be/radio.rs"]
mod rtl8852be;

impl Backend {
    pub fn awdl_program(self) -> Result<&'static str> {
        match self {
            Self::Rtl8852be => Ok("/usr/local/libexec/wirelessd/rtl-awdl"),
            Self::Be200 => Ok("/usr/local/libexec/wirelessd/be200-awdl"),
        }
    }
    pub fn shared_awdl(self) -> bool {
        matches!(self, Self::Be200)
    }
    pub fn awdl_environment(self, cfg: &Configuration) -> Result<Vec<(String, String)>> {
        if self != Self::Be200 {
            return Ok(Vec::new());
        }
        let policy = self
            .profile(cfg)?
            .awdl
            .context("BE200 无线策略缺少 awdl 配置")?;
        ensure!(
            !policy.tcp_congestion_control.is_empty()
                && policy.tcp_congestion_control.len() < 16
                && policy
                    .tcp_congestion_control
                    .bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b == b'_'),
            "tcp_congestion_control 必须是内核拥塞控制算法名"
        );
        ensure!(
            policy.initial_window_segments > 0
                && policy.tx_queue_packets > 0
                && policy.reordering_segments > 0,
            "AWDL 初始窗口、乱序阈值与发送队列长度必须为正整数"
        );
        ensure!(
            (1..=60).contains(&policy.tx_drain_margin_tu),
            "AWDL 排空余量必须为 1–60 TU"
        );
        Ok(vec![
            (
                "AWARE_AWDL_TCP_CONGESTION".into(),
                policy.tcp_congestion_control,
            ),
            (
                "AWARE_AWDL_INITIAL_WINDOW".into(),
                policy.initial_window_segments.to_string(),
            ),
            (
                "AWARE_AWDL_REORDERING".into(),
                policy.reordering_segments.to_string(),
            ),
            (
                "AWARE_AWDL_TX_DRAIN_TU".into(),
                policy.tx_drain_margin_tu.to_string(),
            ),
            (
                "AWARE_AWDL_TX_QUEUE".into(),
                policy.tx_queue_packets.to_string(),
            ),
        ])
    }
    pub fn awdl_channel(self, frequency: u32) -> Result<u8> {
        match self {
            Self::Be200 => {
                ensure!(
                    matches!(frequency, 5745 | 5805),
                    "BE200 AWDL 需要 NAN 数据频率为 5745 或 5805 MHz"
                );
                Ok(((frequency - 5000) / 5) as u8)
            }
            Self::Rtl8852be => Ok(149),
        }
    }
    pub fn driver(self) -> &'static str {
        match self {
            Self::Be200 => "nl80211",
            Self::Rtl8852be => "rtl_nan",
        }
    }
    pub fn data_requires_nm_release(self) -> bool {
        // RTL NDI 使用原厂 netdev 类型；BE200 NAN_DATA 不由 NetworkManager 管理。
        matches!(self, Self::Rtl8852be)
    }
    pub fn removes_bootstrap(self) -> bool {
        matches!(self, Self::Be200)
    }
    pub fn bootstrap(self, entries: &[(String, String)]) -> Result<Option<String>> {
        match self {
            Self::Be200 => be200::bootstrap(entries),
            Self::Rtl8852be => rtl8852be::bootstrap(entries),
        }
    }
    pub fn check(
        self,
        entries: &[(String, String)],
        bootstrap: &str,
        require_data: bool,
        owns_awdl: bool,
    ) -> Result<()> {
        match self {
            Self::Be200 => be200::check(entries, require_data, owns_awdl),
            Self::Rtl8852be => rtl8852be::check(entries, bootstrap, require_data),
        }
    }
    pub fn profile(self, cfg: &Configuration) -> Result<Profile> {
        let path = cfg.radio_profile.clone().unwrap_or_else(|| {
            Path::new("/usr/local/share/wifi-aware/hardware")
                .join(self.name())
                .join("radio.toml")
        });
        let profile: Profile = toml::from_str(
            &std::fs::read_to_string(&path)
                .with_context(|| format!("读取无线策略 {}", path.display()))?,
        )
        .with_context(|| format!("解析无线策略 {}", path.display()))?;
        ensure!(
            Self::from_name(&profile.hardware) == Some(self),
            "radio_profile 与实际硬件不匹配"
        );
        // 当前两个后端使用 2.4 GHz 发现信道，数据频率单独协商。
        ensure!(
            profile.nan.discovery_frequency_mhz == 2437,
            "后端发现信道必须为 2437 MHz"
        );
        ensure!(
            profile.nan.discovery_beacon_interval_tu > 0,
            "发现信标间隔必须为正整数 TU"
        );
        if matches!(self, Self::Rtl8852be) {
            rtl8852be::validate(&profile)?;
        }
        let discovery = bitmap(&profile.schedule.discovery_slots)?;
        let data = bitmap(&profile.schedule.data_slots)?;
        ensure!(discovery & data == 0, "发现与数据时隙不能重叠");
        eprintln!("无线策略: {}", path.display());
        Ok(profile)
    }
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Profile {
    pub hardware: String,
    pub nan: Nan,
    pub schedule: Schedule,
    pub awdl: Option<Awdl>,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Awdl {
    pub tcp_congestion_control: String,
    pub initial_window_segments: u32,
    pub reordering_segments: u32,
    pub tx_queue_packets: u32,
    pub tx_drain_margin_tu: u32,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Nan {
    pub discovery_frequency_mhz: u32,
    pub dual_band: bool,
    pub master_preference: u8,
    pub discovery_beacon_interval_tu: u32,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Schedule {
    pub discovery_slots: Vec<u8>,
    pub data_slots: Vec<u8>,
}
fn bitmap(slots: &[u8]) -> Result<u32> {
    let mut bits = 0;
    for &slot in slots {
        ensure!(slot < 32, "时隙号必须为 0–31");
        ensure!(bits & (1 << slot) == 0, "重复时隙 {slot}");
        bits |= 1 << slot;
    }
    ensure!(bits != 0, "时隙列表不能为空");
    Ok(bits)
}
impl Profile {
    pub fn map(&self, frequency: u32) -> Result<String> {
        let discovery = self.nan.discovery_frequency_mhz;
        if frequency == discovery {
            return Ok(format!("{frequency}:ffffffff"));
        }
        Ok(format!(
            "{frequency}:{} {discovery}:{}",
            crate::support::hex(&bitmap(&self.schedule.data_slots)?.to_le_bytes()),
            crate::support::hex(&bitmap(&self.schedule.discovery_slots)?.to_le_bytes())
        ))
    }
}

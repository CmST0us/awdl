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
    pub fn awdl_channel(self) -> Result<u8> {
        self.awdl_program()?;
        // RTL 当前固定在 149 启动，随后跟随同一 80 MHz 块内的 149/161。
        Ok(149)
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

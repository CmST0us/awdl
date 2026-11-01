use crate::backend::Profile;
use anyhow::{Result, ensure};

pub fn bootstrap(entries: &[(String, String)]) -> Result<Option<String>> {
    ensure!(
        entries.len() <= 2
            && entries
                .iter()
                .all(|(name, kind)| !name.is_empty() && kind == "managed"),
        "RTL 接管前只允许未关联的物理控制接口及 p2p0"
    );
    ensure!(
        entries.len() != 2 || entries.iter().any(|(name, _)| name == "p2p0"),
        "RTL 设备包含非默认的额外接口"
    );
    let control = entries.iter().find(|(name, _)| name != "p2p0");
    ensure!(
        entries.is_empty() || control.is_some(),
        "RTL 缺少物理控制接口"
    );
    Ok(control.map(|(name, _)| name.clone()))
}
pub fn check(entries: &[(String, String)], control: &str, require_data: bool) -> Result<()> {
    ensure!(
        (1..=2).contains(&entries.len())
            && entries.iter().any(|(name, _)| name == control)
            && entries
                .iter()
                .all(|(name, kind)| kind == "managed" && (name == control || name == "p2p0")),
        "RTL 原生 NAN 需要独占物理控制接口及 p2p0"
    );
    ensure!(
        !require_data || aware_common::unix::interface_exists("ndi0")?,
        "RTL 持久 ndi0 已消失"
    );
    Ok(())
}
pub fn validate(profile: &Profile) -> Result<()> {
    ensure!(
        !profile.nan.dual_band
            && profile.nan.master_preference == 200
            && profile.nan.discovery_beacon_interval_tu == 100,
        "RTL 当前支持 dual_band=false、master_preference=200、discovery_beacon_interval_tu=100"
    );
    Ok(())
}

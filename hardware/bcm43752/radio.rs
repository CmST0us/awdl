use crate::backend::Profile;
use anyhow::{Result, ensure};

pub fn bootstrap(entries: &[(String, String)]) -> Result<Option<String>> {
    ensure!(
        entries.len() == 1 && !entries[0].0.is_empty() && entries[0].1 == "managed",
        "BCM43752 需要唯一的物理命令节点；拒绝额外 STA/AP/P2P/monitor 接口"
    );
    Ok(Some(entries[0].0.clone()))
}

pub fn check(
    entries: &[(String, String)],
    control: &str,
    require_data: bool,
    owns_awdl: bool,
) -> Result<()> {
    ensure!(!owns_awdl, "BCM43752 当前只允许原生 NAN");
    ensure!(
        entries.len() == 1 && entries[0].0 == control && entries[0].1 == "managed",
        "BCM43752 的物理命令节点或单一 NAN 约束已改变"
    );
    // The control node retains DHD's Linux type. Its driver exposes no
    // connect/scan/AP/P2P/monitor operation and rejects primary-node TX.
    // The real NAN NDI is a bare Ethernet netdev, absent from `iw dev`.
    ensure!(
        !require_data || aware_common::unix::interface_exists("ndi0")?,
        "BCM43752 持久 ndi0 已消失"
    );
    Ok(())
}

pub fn validate(profile: &Profile) -> Result<()> {
    ensure!(
        (1..=254).contains(&profile.nan.master_preference)
            && (50..=200).contains(&profile.nan.discovery_beacon_interval_tu),
        "BCM43752 master_preference 必须为 1–254，发现信标间隔为 50–200 TU"
    );
    ensure!(profile.awdl.is_none(), "BCM43752 无线策略不能包含 AWDL");
    Ok(())
}

use anyhow::{Result, bail, ensure};

pub fn bootstrap(entries: &[(String, String)]) -> Result<Option<String>> {
    ensure!(
        entries.is_empty()
            || (entries.len() == 1 && !entries[0].0.is_empty() && entries[0].1 == "managed"),
        "MT7925 启动前仅允许一个未关联的 managed 接口"
    );
    Ok(entries.first().map(|entry| entry.0.clone()))
}

pub fn check(entries: &[(String, String)], require_data: bool, owns_awdl: bool) -> Result<()> {
    let (mut nan, mut data) = (0, 0);
    for (name, kind) in entries {
        if (name.is_empty() || name == "nan0") && kind == "NAN" {
            nan += 1;
        } else if name == "ndi0"
            && ["NAN data", "NAN-data", "NAN_DATA", "Unknown mode (13)"].contains(&kind.as_str())
        {
            data += 1;
        } else if owns_awdl
            && name == "awdl0"
            && ["NAN data", "NAN-data", "NAN_DATA", "Unknown mode (13)"].contains(&kind.as_str())
        {
            // Ownership remains until the native backend confirms release.
        } else {
            bail!("MT7925 NAN 服务独占所选 PHY，发现 {name} ({kind})");
        }
    }
    ensure!(
        nan == 1 && (!require_data || data == 1),
        "MT7925 NAN/NDI 接口未就绪"
    );
    Ok(())
}

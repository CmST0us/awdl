use crate::{copy, run, settings::Settings};
use anyhow::{Context, Result, ensure};
use std::{path::Path, process::Command};

pub fn install(root: &Path, cfg: &Settings) -> Result<()> {
    let name = cfg
        .build
        .platform
        .as_deref()
        .or(cfg.install.platform.as_deref())
        .context("请明确 --platform")?;
    ensure!(["rock5b", "dragon-q8b", "pro-rk3568"].contains(&name), "未知平台 {name}");
    let entries: &[(&str, &str)] = match name {
        "dragon-q8b" => &[
            (
                "wifi-aware-performance.service",
                "etc/systemd/system/wifi-aware-performance.service",
            ),
            (
                "performance.conf",
                "etc/systemd/system/wirelessd.service.d/q8b-performance.conf",
            ),
            ("tcp.conf", "etc/sysctl.d/90-wifi-aware-q8b.conf"),
        ],
        "pro-rk3568" => &[
            (
                "wifi-aware-performance.service",
                "etc/systemd/system/wifi-aware-performance.service",
            ),
            (
                "performance.conf",
                "etc/systemd/system/wirelessd.service.d/rk3568-performance.conf",
            ),
            ("tcp.conf", "etc/sysctl.d/90-wifi-aware-rk3568.conf"),
        ],
        _ => &[(
            "usbmoded-shutdown.conf",
            "etc/systemd/system/usbmoded.service.d/shutdown.conf",
        )],
    };
    for (source, target) in entries {
        copy(
            &root.join("platforms").join(name).join(source),
            &cfg.dest.join(target),
            0o644,
        )?;
    }
    if cfg.dest == Path::new("/") {
        run(Command::new("systemctl").arg("daemon-reload"))?;
    }
    Ok(())
}

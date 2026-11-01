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
    ensure!(name == "rock5b", "未知平台 {name}");
    copy(
        &root.join("platforms/rock5b/usbmoded-shutdown.conf"),
        &cfg.dest
            .join("etc/systemd/system/usbmoded.service.d/shutdown.conf"),
        0o644,
    )?;
    if cfg.dest == Path::new("/") {
        run(Command::new("systemctl").arg("daemon-reload"))?;
    }
    Ok(())
}

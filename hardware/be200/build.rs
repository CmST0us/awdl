use crate::{copy, run, settings::Settings, source::files};
use anyhow::{Result, ensure};
use serde::Deserialize;
use std::{
    path::{Path, PathBuf},
    process::Command,
};

#[derive(Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Options {
    pub platform_cn: bool,
}

pub const MODULES: &[&str] = &[
    "cfg80211",
    "mac80211",
    "iwlwifi",
    "iwldvm",
    "iwlmvm",
    "iwlmld",
    "iwlwifi-compat",
];

pub fn build(
    root: &Path,
    source: &Path,
    headers: &Path,
    kernel: &str,
    command: &mut Command,
) -> Result<()> {
    copy(
        &root.join("hardware/be200/driver/be200.defconfig"),
        &source.join("defconfig"),
        0o644,
    )?;
    command
        .arg(format!("KLIB=/lib/modules/{kernel}"))
        .arg(format!("KLIB_BUILD={}", headers.display()));
    let mut configure = Command::new("make");
    configure.args(command.get_args()).arg("usedefconfig");
    run(&mut configure)?;
    run(command.arg("modules"))
}

pub fn installation(root: &Path, cfg: &Settings) -> Result<Vec<(PathBuf, PathBuf)>> {
    let directory = root.join("hardware/be200");
    let firmware: Vec<_> = files(&directory.join("firmware"))?
        .into_iter()
        .filter(|p| p.extension().is_some_and(|e| e == "ucode" || e == "pnvm"))
        .collect();
    ensure!(firmware.len() == 2, "BE200 需要 c108 与 PNVM");
    let mut result = vec![];
    for file in firmware {
        let target = cfg
            .dest
            .join("lib/firmware")
            .join(file.file_name().unwrap());
        result.push((file, target));
    }
    for (source, target) in [
        ("driver/be200.conf", "etc/modprobe.d/be200.conf"),
        (
            "firmware/LICENCE.iwlwifi_firmware",
            "usr/local/share/licenses/wifi-aware/be200/LICENCE.iwlwifi_firmware",
        ),
        (
            "driver/COPYING",
            "usr/local/share/licenses/wifi-aware/be200/COPYING",
        ),
    ] {
        result.push((directory.join(source), cfg.dest.join(target)));
    }
    if cfg.install.be200.platform_cn {
        result.push((
            directory.join("driver/be200-cn.conf"),
            cfg.dest.join("etc/modprobe.d/be200-cn.conf"),
        ));
    }
    Ok(result)
}

pub fn obsolete(cfg: &Settings, updates: &Path) -> Vec<PathBuf> {
    let mut paths = vec![updates.join("compat.ko")];
    if !cfg.install.be200.platform_cn {
        paths.push(cfg.dest.join("etc/modprobe.d/be200-cn.conf"));
    }
    paths
}

pub fn build_awdl(root: &Path, cfg: &Settings) -> Result<()> {
    super::build_native_awdl(root, cfg, "be200")
}
pub fn awdl_installation(root: &Path, cfg: &Settings) -> Vec<(PathBuf, PathBuf)> {
    vec![(
        cfg.user_dir(root).join("aware-awdl/be200-awdl"),
        cfg.dest.join("usr/local/libexec/wirelessd/be200-awdl"),
    )]
}

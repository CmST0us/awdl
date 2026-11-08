use crate::{run, settings::Settings};
use anyhow::{Result, bail, ensure};
use std::{
    fs,
    path::{Path, PathBuf},
    process::Command,
};

pub const MODULES: &[&str] = &["dhdpcie"];

pub fn prepare(root: &Path, output: &Path, archive: Option<&Path>) -> Result<PathBuf> {
    let mut command = Command::new("python3");
    command
        .arg(root.join("hardware/bcm43752/prepare.py"))
        .arg("--output")
        .arg(output);
    if let Some(archive) = archive {
        command.arg("--archive").arg(archive);
    }
    run(&mut command)?;
    Ok(output.into())
}

pub fn build(
    cfg: &Settings,
    source: &Path,
    headers: &Path,
    kernel: &str,
    command: &mut Command,
) -> Result<()> {
    ensure!(
        cfg.target == "aarch64-unknown-linux-gnu" && kernel.starts_with("6.1."),
        "BCM43752 A2 当前驱动面向 ARM64 Linux 6.1"
    );
    run(command
        .arg("-C")
        .arg(headers)
        .arg(format!("M={}", source.display()))
        .arg(format!("CC={}", cfg.build.c_compiler))
        .args([
            "CONFIG_AP6XXX=m",
            "CONFIG_BCMDHD_PCIE=y",
            "CONFIG_BCMDHD_SDIO=",
            "CONFIG_BCMDHD_USB=",
            "CONFIG_BCMDHD_MULTIPLE_DRIVER=y",
            "CONFIG_BCMDHD_NAN=y",
            "modules",
        ]))
}

pub fn installation() -> Result<Vec<(PathBuf, PathBuf)>> {
    bail!("BCM43752 固件尚未通过实机验收及交付授权核对；暂不替换开机驱动和固件")
}

pub fn build_hostap(root: &Path, cfg: &Settings, source: &Path) -> Result<Vec<String>> {
    let adapter = cfg.user_dir(root).join("bcm-hostap");
    run(cfg
        .c_make(&root.join("hardware/bcm43752/hostap"))
        .arg(format!("UP={}", source.display()))
        .arg(format!("BUILD={}", adapter.display())))?;
    Ok(vec![format!(
        "BCM_NAN_LIB={}",
        adapter.join("libwa-bcm-nan.a").display()
    )])
}

pub fn clean_hostap(root: &Path, cfg: &Settings) -> Result<()> {
    let adapter = cfg.user_dir(root).join("bcm-hostap");
    if adapter.exists() {
        fs::remove_dir_all(adapter)?;
    }
    Ok(())
}

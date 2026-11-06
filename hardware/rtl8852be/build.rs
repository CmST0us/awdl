use crate::{run, settings::Settings};
use anyhow::{Context, Result, ensure};
use serde::Deserialize;
use std::{
    fs,
    path::{Path, PathBuf},
    process::Command,
};

#[derive(Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Options {
    pub country: Option<String>,
}

pub const MODULES: &[&str] = &["8852be"];

pub fn prepare(root: &Path, output: &Path, archive: Option<&Path>) -> Result<PathBuf> {
    let mut command = Command::new("python3");
    command
        .arg(root.join("hardware/rtl8852be/driver/prepare.py"))
        .arg("--out")
        .arg(output);
    if let Some(archive) = archive {
        command.arg("--archive").arg(archive);
    }
    run(&mut command)?;
    Ok(output.to_owned())
}

pub fn build(
    root: &Path,
    cfg: &Settings,
    source: &Path,
    headers: &Path,
    command: &mut Command,
) -> Result<()> {
    let firmware = cfg.driver_dir(root)?.join("firmware");
    if firmware.exists() {
        fs::remove_dir_all(&firmware)?;
    }
    run(Command::new("python3")
        .current_dir(root)
        .env(
            "U2_CC",
            cfg.hardware_build
                .mips_compiler
                .as_deref()
                .context("缺少 U2 的 mips_compiler")?,
        )
        .arg(root.join("hardware/rtl8852be/firmware/tools/build_nan.py"))
        .arg("--output")
        .arg(&firmware)
        .arg("--array")
        .arg(source.join("phl/hal_g6/mac/fw_ax/rtl8852b/hal8852b_fw.c")))?;
    let platform = cfg
        .hardware_build
        .rtl_platform
        .as_deref()
        .context("RTL 驱动需要平台配置，例如 --platform rock5b")?;
    command
        .arg(format!("KSRC={}", headers.display()))
        .arg(format!("{platform}=y"))
        .args([
            "CONFIG_WIFI_MONITOR=n",
            "CONFIG_RTW_IPCAM_APPLICATION=n",
            "CONFIG_RTL8852B=y",
            "CONFIG_RTL8852BP=n",
            "modules",
        ]);
    run(command)?;
    verify(
        root,
        &source.join("8852be.ko"),
        &firmware.join("candidate.bin"),
    )
}

fn verify(root: &Path, module: &Path, firmware: &Path) -> Result<()> {
    run(Command::new("python3")
        .arg(root.join("hardware/rtl8852be/firmware/tools/u2fw.py"))
        .arg("verify")
        .arg(module)
        .arg(firmware))
}

pub fn installation(
    root: &Path,
    cfg: &Settings,
    modules: &[&PathBuf],
) -> Result<Vec<(PathBuf, PathBuf)>> {
    let directory = root.join("hardware/rtl8852be");
    let country = cfg
        .install
        .country
        .as_deref()
        .or(cfg.install.rtl8852be.country.as_deref())
        .context("RTL 安装需要明确 country 或 --country")?;
    ensure!(
        country.len() == 2 && country.bytes().all(|b| b.is_ascii_uppercase()),
        "country 必须为两个大写字母"
    );
    let text =
        fs::read_to_string(directory.join("driver/rtl8852be.conf"))?.replace("@COUNTRY@", country);
    let generated = root
        .join(".build/config")
        .join(&cfg.target)
        .join("rtl8852be.conf");
    fs::create_dir_all(generated.parent().unwrap())?;
    fs::write(&generated, text)?;
    verify(
        root,
        modules[0],
        &cfg.driver_dir(root)?.join("firmware/candidate.bin"),
    )?;
    Ok(vec![
        (generated, cfg.dest.join("etc/modprobe.d/rtl8852be.conf")),
        (
            directory.join("driver/UPSTREAM-LICENSE"),
            cfg.dest
                .join("usr/local/share/licenses/wifi-aware/rtl8852be/UPSTREAM-LICENSE"),
        ),
    ])
}

pub fn build_hostap(root: &Path, cfg: &Settings, source: &Path) -> Result<Vec<String>> {
    let adapter = cfg.user_dir(root).join("rtl-hostap");
    run(cfg
        .c_make(&root.join("hardware/rtl8852be/hostap/hostap"))
        .arg(format!("UP={}", source.display()))
        .arg(format!("BUILD={}", adapter.display())))?;
    Ok(vec![format!(
        "RTL_NAN_LIB={}",
        adapter.join("libwa-rtl-nan.a").display()
    )])
}

pub fn clean_hostap(root: &Path, cfg: &Settings) -> Result<()> {
    let adapter = cfg.user_dir(root).join("rtl-hostap");
    if adapter.exists() {
        fs::remove_dir_all(adapter)?;
    }
    Ok(())
}

pub fn build_awdl(root: &Path, cfg: &Settings) -> Result<()> {
    let output = cfg.user_dir(root).join("aware-awdl/rtl-awdl");
    run(Command::new(&cfg.build.c_compiler)
        .current_dir(root)
        .args(["-O2", "-Wall", "-Wextra", "-Werror"])
        .arg(root.join("hardware/rtl8852be/awdl/main.c"))
        .arg(root.join("hardware/rtl8852be/hostap/backend.c"))
        .arg(cfg.user_dir(root).join("aware-awdl/libaware_awdl.a"))
        .args(["-lpthread", "-ldl", "-lm", "-o"])
        .arg(output))
}

pub fn awdl_installation(root: &Path, cfg: &Settings) -> Vec<(PathBuf, PathBuf)> {
    vec![(
        cfg.user_dir(root).join("aware-awdl/rtl-awdl"),
        cfg.dest.join("usr/local/libexec/wirelessd/rtl-awdl"),
    )]
}

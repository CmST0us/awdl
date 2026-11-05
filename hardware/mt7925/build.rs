use crate::{run, settings::Settings};
use anyhow::{Result, ensure};
use serde_json::Value;
use std::{
    fs,
    path::{Path, PathBuf},
    process::Command,
};

pub const MODULES: &[&str] = &[
    "cfg80211",
    "mac80211",
    "mt76",
    "mt76-connac-lib",
    "mt792x-lib",
    "mt7925-common",
    "mt7925e",
];

pub fn prepare(root: &Path, output: &Path, archive: Option<&Path>) -> Result<PathBuf> {
    ensure!(
        archive.is_none(),
        "MT7925 使用 source.json 固定的缓存归档；请运行 hardware/mt7925/prepare.py"
    );
    run(Command::new("python3")
        .arg(root.join("hardware/mt7925/prepare.py"))
        .arg("--modules-to")
        .arg(output))?;
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
        kernel.starts_with("7.0."),
        "MT7925 外部模块当前针对 Linux 7.0；拒绝复用到 {kernel}"
    );
    command
        .arg("-C")
        .arg(headers)
        .arg(format!("M={}", source.display()))
        .arg(format!("CC={}", cfg.build.c_compiler))
        .arg(format!(
            "NOSTDINC_FLAGS=-I{}/include -I{}/include/uapi",
            source.display(),
            source.display()
        ));
    run(command.arg("modules"))
}

pub fn installation(root: &Path, cfg: &Settings) -> Result<Vec<(PathBuf, PathBuf)>> {
    let directory = root.join(".build/dependencies/mt7925/firmware");
    let manifest: Value = serde_json::from_str(&fs::read_to_string(
        root.join("hardware/mt7925/firmware/source.json"),
    )?)?;
    let mut result = vec![];
    for entry in manifest["files"].as_array().unwrap() {
        let name = entry["file"].as_str().unwrap();
        let file = directory.join(name);
        let actual = crate::output(Command::new("sha256sum").arg(&file))?;
        ensure!(
            actual.split_whitespace().next() == entry["sha256"].as_str(),
            "MT7925 固件交付摘要不匹配: {name}"
        );
        let target = if name.ends_with(".bin") {
            // Kernel firmware lookup prefers updates/ over distribution-owned files.
            cfg.dest
                .join("lib/firmware/updates/mediatek/mt7925")
                .join(name)
        } else {
            cfg.dest
                .join("usr/local/share/licenses/wifi-aware/mt7925")
                .join(name)
        };
        result.push((file, target));
    }
    for name in [
        "COPYING",
        "LICENSES/preferred/GPL-2.0",
        "LICENSES/preferred/BSD-3-Clause-Clear",
    ] {
        result.push((
            root.join(".build/dependencies/mt7925/source/wireless-next")
                .join(name),
            cfg.dest
                .join("usr/local/share/licenses/wifi-aware/mt7925")
                .join(name),
        ));
    }
    Ok(result)
}

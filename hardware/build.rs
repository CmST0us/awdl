#[path = "catalog.rs"]
mod catalog;
pub use catalog::Hardware;
#[path = "be200/build.rs"]
mod be200;
#[path = "bcm43752/build.rs"]
mod bcm43752;
#[path = "mt7925/build.rs"]
mod mt7925;
#[path = "rtl8852be/build.rs"]
mod rtl8852be;

use crate::{settings::Settings, source};
use anyhow::{Result, ensure};
use serde::Deserialize;
use std::{
    path::{Path, PathBuf},
    process::Command,
};

#[derive(Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct BuildOptions {
    pub rtl_platform: Option<String>,
    pub mips_compiler: Option<String>,
}

impl BuildOptions {
    pub fn take(table: &mut toml::Table) -> Result<Self> {
        let mut options = toml::Table::new();
        for field in ["rtl_platform", "mips_compiler"] {
            if let Some(value) = table.remove(field) {
                options.insert(field.into(), value);
            }
        }
        Ok(toml::Value::Table(options).try_into()?)
    }
}

pub fn build_field(option: &str) -> Option<&'static str> {
    match option {
        "--mips-cc" => Some("mips_compiler"),
        _ => None,
    }
}

pub const BUILD_ENV: &[(&str, &str)] = &[("U2_CC", "mips_compiler")];

#[derive(Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Install {
    pub hardware: Option<String>,
    pub platform: Option<String>,
    pub pci: String,
    pub rtl8852be: rtl8852be::Options,
    pub be200: be200::Options,
    pub mt7925: mt7925::Options,
    #[serde(skip)]
    pub country: Option<String>,
}

impl Install {
    pub fn flag(&mut self, flag: &str) -> bool {
        if flag == "--cn" {
            self.be200.platform_cn = true;
            true
        } else {
            false
        }
    }

    pub fn option(&mut self, key: &str, value: &str) -> bool {
        if key == "--country" {
            self.country = Some(value.into());
            true
        } else {
            false
        }
    }
}

impl Hardware {
    pub fn prepare(self, root: &Path, output: &Path, archive: Option<&Path>) -> Result<PathBuf> {
        match self {
            Self::Be200 => source::prepare(
                root,
                &root.join("hardware/be200/driver"),
                output,
                archive,
                &[],
            ),
            Self::Rtl8852be => rtl8852be::prepare(root, output, archive),
            Self::Mt7925 => mt7925::prepare(root, output, archive),
            Self::Bcm43752 => bcm43752::prepare(root, output, archive),
        }
    }

    pub fn build(
        self,
        root: &Path,
        cfg: &Settings,
        source: &Path,
        headers: &Path,
        kernel: &str,
        command: &mut Command,
    ) -> Result<()> {
        match self {
            Self::Be200 => be200::build(root, source, headers, kernel, command),
            Self::Rtl8852be => rtl8852be::build(root, cfg, source, headers, command),
            Self::Mt7925 => mt7925::build(cfg, source, headers, kernel, command),
            Self::Bcm43752 => bcm43752::build(cfg, source, headers, kernel, command),
        }
    }

    pub fn modules(self) -> &'static [&'static str] {
        match self {
            Self::Be200 => be200::MODULES,
            Self::Rtl8852be => rtl8852be::MODULES,
            Self::Mt7925 => mt7925::MODULES,
            Self::Bcm43752 => bcm43752::MODULES,
        }
    }

    pub fn installation(
        self,
        root: &Path,
        cfg: &Settings,
        modules: &[&PathBuf],
    ) -> Result<Vec<(PathBuf, PathBuf)>> {
        match self {
            Self::Be200 => be200::installation(root, cfg),
            Self::Rtl8852be => rtl8852be::installation(root, cfg, modules),
            Self::Mt7925 => mt7925::installation(root, cfg),
            Self::Bcm43752 => bcm43752::installation(),
        }
    }

    pub fn obsolete(self, cfg: &Settings, updates: &Path) -> Vec<PathBuf> {
        match self {
            Self::Be200 => be200::obsolete(cfg, updates),
            Self::Rtl8852be | Self::Mt7925 | Self::Bcm43752 => vec![],
        }
    }
}

pub fn prepare(
    root: &Path,
    component: &str,
    output: &Path,
    archive: Option<&Path>,
) -> Result<PathBuf> {
    if component == "hostap" {
        return source::prepare(
            root,
            &root.join("components/hostap"),
            output,
            archive,
            &[
                root.join("hardware/rtl8852be/hostap/patches"),
                root.join("hardware/bcm43752/hostap/patches"),
            ],
        );
    }
    let hardware = Hardware::from_name(component);
    ensure!(hardware.is_some(), "未知源码组件 {component}");
    hardware.unwrap().prepare(root, output, archive)
}

pub fn hostap_config(root: &Path) -> Result<String> {
    Ok(format!(
        "{}\n{}",
        std::fs::read_to_string(root.join("hardware/rtl8852be/hostap/hostap.config"))?,
        std::fs::read_to_string(root.join("hardware/bcm43752/hostap/hostap.config"))?
    ))
}

pub fn build_hostap(root: &Path, cfg: &Settings, source: &Path) -> Result<Vec<String>> {
    let mut libraries = rtl8852be::build_hostap(root, cfg, source)?;
    libraries.extend(bcm43752::build_hostap(root, cfg, source)?);
    Ok(libraries)
}

pub fn clean_hostap(root: &Path, cfg: &Settings) -> Result<()> {
    rtl8852be::clean_hostap(root, cfg)?;
    bcm43752::clean_hostap(root, cfg)
}

pub const OBSOLETE_USERSPACE_FILES: &[&str] = &[
    "etc/dbus-1/system.d/org.be200.WiFiAware1.conf",
    "etc/systemd/system/wifiawared.service.d/rtl.conf",
    "etc/NetworkManager/conf.d/91-wifiawared-rtl.conf",
];

pub fn build_awdl(root: &Path, cfg: &Settings) -> Result<()> {
    be200::build_awdl(root, cfg)?;
    build_native_awdl(root, cfg, "mt7925")?;
    rtl8852be::build_awdl(root, cfg)
}

pub fn awdl_installation(root: &Path, cfg: &Settings) -> Result<Vec<(PathBuf, PathBuf)>> {
    Ok(match cfg.hardware()? {
        Hardware::Be200 => be200::awdl_installation(root, cfg),
        Hardware::Rtl8852be => rtl8852be::awdl_installation(root, cfg),
        Hardware::Mt7925 => vec![(
            cfg.user_dir(root).join("aware-awdl/mt7925-awdl"),
            cfg.dest.join("usr/local/libexec/wirelessd/mt7925-awdl"),
        )],
        Hardware::Bcm43752 => anyhow::bail!("BCM43752 后端未实现 AWDL"),
    })
}
pub fn awdl_service(cfg: &Settings) -> Result<&'static str> {
    Ok(match cfg.hardware()? {
        Hardware::Be200 | Hardware::Mt7925 => "wirelessd.service",
        Hardware::Rtl8852be => "wirelessd-awdl.service",
        Hardware::Bcm43752 => anyhow::bail!("BCM43752 后端未实现 AWDL"),
    })
}

fn build_native_awdl(root: &Path, cfg: &Settings, hardware: &str) -> Result<()> {
    let headers = cfg.user_dir(root).join("hostap/src/drivers");
    ensure!(
        headers.join("nl80211_copy.h").is_file(),
        "请先 prepare-source hostap"
    );
    crate::run(Command::new(&cfg.build.c_compiler)
        .current_dir(root)
        .args(["-O2", "-Wall", "-Wextra", "-Werror", "-I"])
        .arg(headers)
        .arg(root.join(format!("hardware/{hardware}/awdl/main.c")))
        .arg(cfg.user_dir(root).join("aware-awdl/libaware_awdl.a"))
        .args(["-lpthread", "-ldl", "-lm", "-o"])
        .arg(cfg.user_dir(root).join(format!("aware-awdl/{hardware}-awdl"))))
}

use crate::{
    hardware::{self, Hardware, Install},
    output,
};
use anyhow::{Context, Result, bail, ensure};
use serde::Deserialize;
use std::{
    env, fs,
    path::{Path, PathBuf},
    process::Command,
};

#[derive(Default, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Build {
    pub platform: Option<String>,
    pub rust_target: Option<String>,
    pub kernel_arch: Option<String>,
    pub kernel_headers: Option<PathBuf>,
    pub kernel_release: Option<String>,
    pub c_compiler: String,
    pub cross_compile: Option<String>,
    pub jobs: usize,
}
pub struct Settings {
    pub build: Build,
    pub install: Install,
    pub hardware_build: hardware::BuildOptions,
    pub target: String,
    pub host: String,
    pub dest: PathBuf,
    pub archive: Option<PathBuf>,
    pub rest: Vec<String>,
    pub explicit_pci: bool,
}
fn table(path: &Path) -> Result<toml::Table> {
    match fs::read_to_string(path) {
        Ok(text) => Ok(toml::from_str(&text).with_context(|| format!("读取 {}", path.display()))?),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(toml::Table::new()),
        Err(e) => Err(e.into()),
    }
}
impl Settings {
    pub fn load(root: &Path, args: &[String]) -> Result<Self> {
        let mut local = table(&root.join("config/local/build.toml"))?;
        let mut install: Install =
            toml::Value::Table(table(&root.join("config/local/install.toml"))?).try_into()?;
        let mut cli = toml::Table::new();
        let mut dest = env::var_os("DESTDIR")
            .filter(|s| !s.is_empty())
            .map(PathBuf::from)
            .unwrap_or_else(|| "/".into());
        let mut archive = None;
        let mut rest = vec![];
        let mut explicit_pci = false;
        let mut args = args.iter();
        while let Some(arg) = args.next() {
            if Hardware::from_name(arg).is_some() {
                install.hardware = Some(arg.clone());
                continue;
            }
            if install.flag(arg) {
                continue;
            }
            let (key, inline) = arg
                .split_once('=')
                .map_or((arg.as_str(), None), |(k, v)| (k, Some(v.to_string())));
            if !key.starts_with("--") {
                rest.push(arg.clone());
                continue;
            }
            let value = inline
                .or_else(|| args.next().cloned())
                .with_context(|| format!("{key} 缺少参数"))?;
            if install.option(key, &value) {
                continue;
            }
            let field = match key {
                "--platform" => "platform",
                "--target" => "rust_target",
                "--kernel-release" => "kernel_release",
                "--headers" => "kernel_headers",
                "--cc" => "c_compiler",
                "--cross-compile" => "cross_compile",
                "--kernel-arch" => "kernel_arch",
                "--jobs" => "jobs",
                "--hardware" => {
                    install.hardware = Some(value);
                    continue;
                }
                "--pci" => {
                    install.pci = value;
                    explicit_pci = true;
                    continue;
                }
                "--destdir" => {
                    dest = PathBuf::from(value);
                    continue;
                }
                "--archive" => {
                    archive = Some(PathBuf::from(value).canonicalize()?);
                    continue;
                }
                _ => hardware::build_field(key).with_context(|| format!("未知参数 {key}"))?,
            };
            cli.insert(
                field.into(),
                if field == "jobs" {
                    toml::Value::Integer(value.parse()?)
                } else {
                    toml::Value::String(value)
                },
            );
        }
        for (var, field) in [
            ("CARGO_BUILD_TARGET", "rust_target"),
            ("KERNEL_RELEASE", "kernel_release"),
            ("KSRC", "kernel_headers"),
            ("KLIB_BUILD", "kernel_headers"),
            ("CC", "c_compiler"),
            ("CROSS_COMPILE", "cross_compile"),
            ("ARCH", "kernel_arch"),
            ("JOBS", "jobs"),
        ]
        .into_iter()
        .chain(hardware::BUILD_ENV.iter().copied())
        {
            if let Ok(value) = env::var(var) {
                local.insert(
                    field.into(),
                    if field == "jobs" {
                        toml::Value::Integer(value.parse()?)
                    } else {
                        toml::Value::String(value)
                    },
                );
            }
        }
        local.extend(cli);
        let mut defaults = table(&root.join("config/build.toml"))?;
        if let Some(name) = &install.hardware {
            if let Some(hardware) = Hardware::from_name(name) {
                defaults.extend(table(
                    &root
                        .join("hardware")
                        .join(hardware.name())
                        .join("build.toml"),
                )?);
            }
        }
        if let Some(platform) = local
            .get("platform")
            .and_then(|v| v.as_str())
            .or(install.platform.as_deref())
        {
            let path = root.join("platforms").join(platform).join("build.toml");
            ensure!(path.is_file(), "未知平台 {platform}");
            defaults.extend(table(&path)?);
        }
        defaults.extend(local);
        let hardware_build = hardware::BuildOptions::take(&mut defaults)?;
        let build: Build = toml::Value::Table(defaults).try_into()?;
        ensure!(build.jobs > 0, "jobs 必须为正整数");
        if let Some(path) = &build.kernel_headers {
            ensure!(path.is_absolute(), "kernel_headers 必须为构建机绝对路径");
        }
        ensure!(dest.is_absolute(), "DESTDIR 必须为绝对路径");
        let host = match (std::env::consts::ARCH, std::env::consts::OS) {
            ("aarch64", "linux") => "aarch64-unknown-linux-gnu",
            ("x86_64", "linux") => "x86_64-unknown-linux-gnu",
            ("aarch64", "macos") => "aarch64-apple-darwin",
            ("x86_64", "macos") => "x86_64-apple-darwin",
            _ => bail!("不支持的构建工具运行平台"),
        }
        .to_string();
        let target = build.rust_target.clone().unwrap_or_else(|| host.clone());
        Ok(Self {
            build,
            install,
            hardware_build,
            target,
            host,
            dest,
            archive,
            rest,
            explicit_pci,
        })
    }
    pub fn hardware(&self) -> Result<Hardware> {
        let name = self
            .install
            .hardware
            .as_deref()
            .context("请指定硬件或设置 config/local/install.toml 的 hardware")?;
        Hardware::from_name(name).with_context(|| format!("未知硬件 {name}"))
    }
    pub fn linux(&self) -> Result<()> {
        ensure!(
            self.target.contains("linux"),
            "目标必须为 Linux，使用 --target 或 config/local/build.toml 指定"
        );
        Ok(())
    }
    pub fn kernel(&self) -> Result<String> {
        if let Some(kernel) = &self.build.kernel_release {
            return Ok(kernel.clone());
        }
        ensure!(
            self.host == self.target && cfg!(target_os = "linux"),
            "交叉构建必须明确 kernel_release"
        );
        output(Command::new("uname").arg("-r"))
    }
    pub fn user_dir(&self, root: &Path) -> PathBuf {
        root.join(".build/targets").join(&self.target)
    }
    pub fn driver_dir(&self, root: &Path) -> Result<PathBuf> {
        Ok(self
            .user_dir(root)
            .join("drivers")
            .join(self.hardware()?.name())
            .join(self.kernel()?))
    }
    pub fn c_make(&self, directory: &Path) -> Command {
        let mut cmd = Command::new("make");
        cmd.arg("-C")
            .arg(directory)
            .arg(format!("-j{}", self.build.jobs))
            .arg(format!("CC={}", self.build.c_compiler));
        if let Some(prefix) = &self.build.cross_compile {
            cmd.arg(format!("AR={prefix}ar"));
        }
        cmd
    }
    pub fn build_driver(&self, root: &Path) -> Result<()> {
        self.linux()?;
        let hardware = self.hardware()?;
        let destination = self.driver_dir(root)?;
        let source =
            hardware.prepare(root, &destination.join("source"), self.archive.as_deref())?;
        let kernel = self.kernel()?;
        let headers = self
            .build
            .kernel_headers
            .clone()
            .unwrap_or_else(|| PathBuf::from(format!("/lib/modules/{kernel}/build")));
        ensure!(headers.is_dir(), "缺少内核 headers {}", headers.display());
        let arch = self
            .build
            .kernel_arch
            .as_deref()
            .or_else(|| {
                if self.target.starts_with("aarch64-") {
                    Some("arm64")
                } else if self.target.starts_with("x86_64-") {
                    Some("x86")
                } else {
                    None
                }
            })
            .context("请设置 kernel_arch")?;
        let mut command = Command::new("make");
        command
            .arg("-C")
            .arg(&source)
            .arg(format!("-j{}", self.build.jobs))
            .arg(format!("ARCH={arch}"));
        if let Some(prefix) = &self.build.cross_compile {
            command.arg(format!("CROSS_COMPILE={prefix}"));
        }
        hardware.build(root, self, &source, &headers, &kernel, &mut command)
    }
}

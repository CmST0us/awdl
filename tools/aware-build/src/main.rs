#[path = "../../../hardware/build.rs"]
mod hardware;
mod install;
mod ios;
mod licenses;
#[path = "../../../platforms/install.rs"]
mod platform;
mod settings;
mod source;

use anyhow::{Context, Result, bail, ensure};
use std::{
    env, fs,
    path::{Path, PathBuf},
    process::Command,
};

pub fn run(command: &mut Command) -> Result<()> {
    let status = command
        .status()
        .with_context(|| format!("执行 {:?}", command.get_program()))?;
    ensure!(
        status.success(),
        "命令执行失败: {:?} ({status})",
        command.get_program()
    );
    Ok(())
}
pub fn output(command: &mut Command) -> Result<String> {
    let out = command
        .output()
        .with_context(|| format!("执行 {:?}", command.get_program()))?;
    ensure!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    Ok(String::from_utf8(out.stdout)?.trim().to_owned())
}
pub fn copy(source: &Path, destination: &Path, mode: u32) -> Result<()> {
    use std::os::unix::fs::PermissionsExt;
    ensure!(source.is_file(), "缺少文件 {}", source.display());
    fs::create_dir_all(destination.parent().context("目标没有父目录")?)?;
    let temporary = destination.with_extension("install");
    fs::copy(source, &temporary)?;
    fs::set_permissions(&temporary, fs::Permissions::from_mode(mode))?;
    fs::rename(temporary, destination)?;
    Ok(())
}
pub fn env_or(name: &str, fallback: impl Into<String>) -> String {
    env::var(name).unwrap_or_else(|_| fallback.into())
}
fn build_rust(root: &Path, cfg: &settings::Settings, package: &str) -> Result<()> {
    cfg.linux()?;
    let target = &cfg.target;
    let cargo = env_or("CARGO", "cargo");
    run(Command::new(&cargo)
        .current_dir(root)
        .env("CARGO_TARGET_DIR", root.join(".build/rust"))
        .args([
            "build",
            "--locked",
            "--release",
            "--package",
            package,
            "--target",
            &target,
        ]))?;
    let destination = cfg.user_dir(root).join(package);
    copy(
        &root
            .join(".build/rust")
            .join(&target)
            .join("release")
            .join(package),
        &destination.join(package),
        0o755,
    )?;
    licenses::collect(root, package, &target, &destination.join("licenses"))
}
fn save_tool(root: &Path, host: &str) -> Result<()> {
    let destination = root.join(".build/tools").join(host).join("aware-build");
    let current = env::current_exe()?;
    if current != destination {
        copy(&current, &destination, 0o755)?;
    }
    Ok(())
}
fn execute(root: &Path, action: &str, args: &[String]) -> Result<()> {
    if ["--help", "-h", "help"].contains(&action) {
        let hardware = hardware::Hardware::ALL
            .iter()
            .map(|hardware| hardware.name())
            .collect::<Vec<_>>()
            .join("|");
        println!(
            "用法: scripts/aware-build <命令> [参数]\nprepare-source hostap|{hardware}\nbuild-hostap | build-publisher | build-fileserver | build-ios\nbuild-driver {hardware}\ninstall-driver {hardware} | install-publisher | install-fileserver | install-platform\ngenerate-ios-config\n构建: --platform --target --kernel-release --headers --cc --mips-cc --cross-compile --kernel-arch --jobs --archive\n安装: --hardware --pci --country --cn --destdir\n配置: config/local/build.toml、config/local/install.toml"
        );
        return Ok(());
    }
    let cfg = settings::Settings::load(root, args)?;
    match action {
        "prepare-source" => {
            let component = cfg
                .rest
                .first()
                .map(String::as_str)
                .or(cfg.install.hardware.as_deref())
                .context("请指定源码组件")?;
            let directory = if component == "hostap" {
                cfg.user_dir(root).join("hostap")
            } else {
                cfg.driver_dir(root)?.join("source")
            };
            hardware::prepare(root, component, &directory, cfg.archive.as_deref())?;
        }
        "build-driver" => cfg.build_driver(root)?,
        "build-hostap" => {
            cfg.linux()?;
            let source = hardware::prepare(
                root,
                "hostap",
                &cfg.user_dir(root).join("hostap"),
                cfg.archive.as_deref(),
            )?;
            let build = source.join("wpa_supplicant");
            let toolchain_path = cfg.user_dir(root).join("hostap-toolchain.txt");
            let toolchain = format!(
                "CC={}\nAR={}ar\n",
                cfg.build.c_compiler,
                cfg.build.cross_compile.as_deref().unwrap_or("")
            );
            if fs::read_to_string(&toolchain_path).ok().as_deref() != Some(&toolchain) {
                run(cfg.c_make(&build).arg("clean"))?;
                hardware::clean_hostap(root, &cfg)?;
            }
            let driver_args = hardware::build_hostap(root, &cfg, &source)?;
            let config = format!(
                "{}\n{}",
                fs::read_to_string(root.join("components/hostap/hostap.config"))?,
                hardware::hostap_config(root)?
            );
            if fs::read_to_string(build.join(".config")).ok().as_deref() != Some(&config) {
                fs::write(build.join(".config"), config)?;
            }
            run(cfg
                .c_make(&build)
                .args(driver_args)
                .args(["wpa_supplicant", "wpa_cli"]))?;
            fs::write(toolchain_path, toolchain)?;
        }
        "build-publisher" => build_rust(root, &cfg, "wifiawared")?,
        "build-fileserver" => build_rust(root, &cfg, "fileserver")?,
        "generate-ios-config" | "build-ios" => {
            ios::generate(root)?;
            if action == "build-ios" {
                let scheme = cfg
                    .rest
                    .first()
                    .map(String::as_str)
                    .unwrap_or("AwareClient");
                let project = root.join("ios/AwareClient.xcodeproj");
                let configuration = output(
                    Command::new("xmllint")
                        .args([
                            "--xpath",
                            "string(/Scheme/ArchiveAction/@buildConfiguration)",
                        ])
                        .arg(
                            project
                                .join("xcshareddata/xcschemes")
                                .join(format!("{scheme}.xcscheme")),
                        ),
                )?;
                ensure!(!configuration.is_empty(), "scheme 缺少 Archive 构建配置");
                run(Command::new("xcodebuild")
                    .arg("-project")
                    .arg(&project)
                    .args([
                        "-scheme",
                        scheme,
                        "-destination",
                        "generic/platform=iOS",
                        "-derivedDataPath",
                    ])
                    .arg(root.join(".build/ios"))
                    .args([
                        "-configuration",
                        &configuration,
                        "CODE_SIGNING_ALLOWED=NO",
                        "build",
                    ]))?;
            }
        }
        "install-driver" => install::driver(root, &cfg)?,
        "install-publisher" => install::service(root, &cfg, true)?,
        "install-fileserver" => install::service(root, &cfg, false)?,
        "install-platform" => platform::install(root, &cfg)?,
        _ => bail!("未知命令 {action}"),
    }
    if action.starts_with("build-") {
        save_tool(root, &cfg.host)?;
    }
    Ok(())
}

fn main() {
    let mut args = env::args().skip(1);
    let action = args.next().unwrap_or_else(|| "help".into());
    let result = (|| {
        let root = PathBuf::from(
            env::var_os("AWARE_PROJECT_ROOT")
                .context("通过 scripts/aware-build 启动，或设置 AWARE_PROJECT_ROOT")?,
        )
        .canonicalize()?;
        execute(&root, &action, &args.collect::<Vec<_>>())
    })();
    if let Err(e) = result {
        eprintln!("{e:#}");
        std::process::exit(1);
    }
}

use crate::{
    copy,
    hardware::{self, Hardware},
    output, run,
    settings::Settings,
    source::files,
};
use anyhow::{Context, Result, bail, ensure};
use std::{env, fs, path::Path, process::Command};
use toml_edit::{DocumentMut, Item, value};

fn require_program(name: &str) -> Result<()> {
    ensure!(
        env::var_os("PATH")
            .is_some_and(|path| env::split_paths(&path).any(|p| p.join(name).is_file())),
        "缺少程序 {name}"
    );
    Ok(())
}
fn remove(path: &Path) -> Result<()> {
    match fs::remove_file(path) {
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(e.into()),
        _ => Ok(()),
    }
}
fn write(path: &Path, text: &str) -> Result<()> {
    fs::create_dir_all(path.parent().context("配置目录")?)?;
    fs::write(path, text)?;
    Ok(())
}
fn set(document: &mut DocumentMut, key: &str, new: Item) {
    let decor = document
        .get(key)
        .and_then(Item::as_value)
        .map(|v| v.decor().clone());
    document[key] = new;
    if let Some(decor) = decor {
        if let Some(v) = document[key].as_value_mut() {
            *v.decor_mut() = decor;
        }
    }
}
fn selected_pci(cfg: &Settings, config: &DocumentMut, live: bool) -> Result<String> {
    let previous = config.get("pci").and_then(Item::as_str).unwrap_or("");
    if !previous.is_empty() && !cfg.explicit_pci {
        return Ok(previous.into());
    }
    if !cfg.install.pci.is_empty() {
        return Ok(cfg.install.pci.clone());
    }
    if !live {
        return Ok(String::new());
    }
    let mut devices = vec![];
    for entry in fs::read_dir("/sys/bus/pci/devices")? {
        let entry = entry?;
        let vendor = fs::read_to_string(entry.path().join("vendor"))?;
        let device = fs::read_to_string(entry.path().join("device"))?;
        if Hardware::detect(vendor.trim(), device.trim()).is_some() {
            devices.push(entry.file_name().to_string_lossy().into_owned());
        }
    }
    ensure!(devices.len() == 1, "安装需要唯一受支持网卡或显式 --pci");
    Ok(devices.remove(0))
}
pub fn service(root: &Path, cfg: &Settings, publisher: bool) -> Result<()> {
    cfg.linux()?;
    let dest = &cfg.dest;
    let live = dest == Path::new("/");
    let name = if publisher { "wirelessd" } else { "fileserver" };
    let build = cfg.user_dir(root).join(name);
    let component = root.join("components").join(name);
    let mut planned = vec![
        (
            build.join(name),
            dest.join("usr/local/bin").join(name),
            0o755,
        ),
        (
            component.join(format!("{name}.service")),
            dest.join("etc/systemd/system")
                .join(format!("{name}.service")),
            0o644,
        ),
    ];
    ensure!(
        build.join("licenses/dependencies.json").is_file(),
        "缺少 Rust 授权清单，请先构建 {name}"
    );
    for file in files(&build.join("licenses"))? {
        planned.push((
            file.clone(),
            dest.join("usr/local/share/licenses")
                .join(name)
                .join(file.strip_prefix(build.join("licenses"))?),
            0o644,
        ));
    }
    let generated = root.join(".build/config").join(&cfg.target);
    let config = dest.join("etc").join(name).join("config.toml");
    let previous_config = dest.join("etc/wifiawared/config.toml");
    let rename_daemon = publisher
        && (dest.join("etc/wifiawared").exists()
            || dest.join("etc/systemd/system/wifiawared.service").exists());
    let old = dest.join("etc/aware-fileserver/config.toml");
    let defaults = if publisher {
        fs::read_to_string(root.join("config/wirelessd.toml"))?
    } else {
        format!(
            "{}\n{}",
            fs::read_to_string(root.join("config/fileserver-discovery.toml"))?,
            fs::read_to_string(root.join("config/fileserver.toml"))?
        )
    };
    let mut document: DocumentMut = if config.exists() {
        fs::read_to_string(&config)?
    } else if publisher && previous_config.exists() {
        fs::read_to_string(&previous_config)?
    } else {
        defaults.clone()
    }
    .parse()?;
    if rename_daemon {
        for (key, previous, current) in [
            ("state", "/var/lib/wifiawared", "/var/lib/wirelessd"),
            ("runtime", "/run/wifiawared", "/run/wirelessd"),
            (
                "supplicant",
                "/usr/local/libexec/wifiawared",
                "/usr/local/libexec/wirelessd",
            ),
            ("radio_profile", "/etc/wifiawared", "/etc/wirelessd"),
        ] {
            if let Some(suffix) = document
                .get(key)
                .and_then(Item::as_str)
                .and_then(|path| path.strip_prefix(previous))
                .filter(|suffix| suffix.is_empty() || suffix.starts_with('/'))
            {
                let path = format!("{current}{suffix}");
                set(&mut document, key, value(path));
            }
        }
    }
    if !publisher && !config.exists() && old.exists() {
        let previous: DocumentMut = fs::read_to_string(&old)?.parse()?;
        for (key, item) in previous.iter() {
            ensure!(
                ["service", "files", "port", "io_timeout"].contains(&key),
                "旧文件服务配置包含未知字段 {key}"
            );
            set(&mut document, key, item.clone());
        }
    }
    if publisher {
        for binary in ["wpa_supplicant", "wpa_cli"] {
            planned.push((
                cfg.user_dir(root)
                    .join("hostap/wpa_supplicant")
                    .join(binary),
                dest.join("usr/local/libexec/wirelessd").join(binary),
                0o755,
            ));
        }
        for (from, to) in [
            (
                "org.wirelessd.conf",
                "etc/dbus-1/system.d/org.wirelessd.conf",
            ),
            (
                "NetworkManager-wirelessd.conf",
                "etc/NetworkManager/conf.d/90-wirelessd.conf",
            ),
        ] {
            planned.push((component.join(from), dest.join(to), 0o644));
        }
        planned.push((
            root.join("components/hostap/LICENSE.hostap"),
            dest.join("usr/local/share/licenses/wirelessd/hostap.txt"),
            0o644,
        ));
        for hardware in Hardware::ALL {
            planned.push((
                root.join("hardware")
                    .join(hardware.name())
                    .join("radio.toml"),
                dest.join("usr/local/share/wifi-aware/hardware")
                    .join(hardware.name())
                    .join("radio.toml"),
                0o644,
            ));
        }
        let pci = selected_pci(cfg, &document, live)?;
        if !pci.is_empty() {
            ensure!(
                pci.len() == 12
                    && pci
                        .bytes()
                        .all(|b| b.is_ascii_hexdigit() || b":.".contains(&b)),
                "pci 必须为完整 PCI 地址"
            );
            if cfg.explicit_pci || !config.exists() {
                set(&mut document, "pci", value(&pci));
            }
            let rules = format!(
                "# 所选 PCI 的物理控制接口交给 wirelessd；udev 重新加载后生效。\nSUBSYSTEM==\"net\", KERNELS==\"{pci}\", ENV{{NM_UNMANAGED}}=\"1\"\n# 为所选 PCI 提供稳定的 systemd 设备依赖名称。\nSUBSYSTEM==\"pci\", KERNEL==\"{pci}\", TAG+=\"systemd\", ENV{{SYSTEMD_ALIAS}}+=\"/sys/subsystem/pci/devices/{pci}\"\n"
            );
            write(&generated.join("90-wirelessd.rules"), &rules)?;
            planned.push((
                generated.join("90-wirelessd.rules"),
                dest.join("etc/udev/rules.d/90-wirelessd.rules"),
                0o644,
            ));
            let unit = format!(
                "# 选定 PCI 的生命周期依赖；修改后执行 systemctl daemon-reload。\n[Unit]\n# PCI 设备出现后才启动，设备消失时停止。\nBindsTo=sys-subsystem-pci-devices-{pci}.device\n# 设备就绪后进入公共无线接管流程。\nAfter=sys-subsystem-pci-devices-{pci}.device\n"
            );
            write(&generated.join("device.conf"), &unit)?;
            planned.push((
                generated.join("device.conf"),
                dest.join("etc/systemd/system/wirelessd.service.d/device.conf"),
                0o644,
            ));
        }
    }
    if !config.exists() || rename_daemon || (publisher && cfg.explicit_pci) {
        write(
            &generated.join(format!("{name}.toml")),
            &document.to_string(),
        )?;
        planned.push((
            generated.join(format!("{name}.toml")),
            config.clone(),
            0o644,
        ));
    }
    let mut renamed_units = Vec::new();
    let mut dependent_units = Vec::new();
    let mut moved_directories = Vec::new();
    if rename_daemon {
        for directory in [
            "etc/wifiawared",
            "var/lib/wifiawared",
            "usr/local/libexec/wifiawared",
            "usr/local/share/licenses/wifiawared",
            "etc/systemd/system/wifiawared.service.d",
            "etc/systemd/system/wifiawared-awdl.service.d",
        ] {
            let previous = dest.join(directory);
            if previous.exists() {
                let current = dest.join(directory.replace("wifiawared", "wirelessd"));
                ensure!(
                    !current.exists(),
                    "{} 与 {} 同时存在，请先明确保留的部署目录",
                    previous.display(),
                    current.display()
                );
                moved_directories.push((previous, current));
            }
        }
        for (previous, current) in [
            ("wifiawared.service", "wirelessd.service"),
            ("wifiawared-awdl.service", "wirelessd-awdl.service"),
        ] {
            if !dest.join("etc/systemd/system").join(previous).exists() {
                continue;
            }
            let enabled = if live {
                Command::new("systemctl")
                    .args(["is-enabled", "--quiet", previous])
                    .status()?
                    .success()
            } else {
                dest.join("etc/systemd/system/multi-user.target.wants")
                    .join(previous)
                    .is_symlink()
            };
            let active = live
                && Command::new("systemctl")
                    .args(["is-active", "--quiet", previous])
                    .status()?
                    .success();
            renamed_units.push((previous, current, enabled, active));
            if current == "wirelessd-awdl.service" {
                planned.push((
                    component.join(current),
                    dest.join("etc/systemd/system").join(current),
                    0o644,
                ));
            }
        }
        for dependent in ["fileserver", "airdrop"] {
            let unit = format!("{dependent}.service");
            let target = dest.join("etc/systemd/system").join(&unit);
            if !target.exists() {
                continue;
            }
            let active = live
                && Command::new("systemctl")
                    .args(["is-active", "--quiet", &unit])
                    .status()?
                    .success();
            dependent_units.push((unit.clone(), active));
            planned.push((
                root.join("components").join(dependent).join(&unit),
                target,
                0o644,
            ));
        }
    }
    for (source, _, _) in &planned {
        ensure!(source.is_file(), "缺少构建产物 {}", source.display());
    }
    if live && rename_daemon {
        for (unit, _) in &dependent_units {
            run(Command::new("systemctl").args(["stop", unit]))?;
        }
        for (previous, _, _, _) in &renamed_units {
            run(Command::new("systemctl").args(["disable", "--now", previous]))?;
        }
    }
    for (previous, current) in moved_directories {
        fs::rename(&previous, &current)
            .with_context(|| format!("迁移 {} 到 {}", previous.display(), current.display()))?;
    }
    let old_unit = dest.join("etc/systemd/system/aware-fileserver.service");
    let takeover = !publisher && old_unit.exists();
    let old_enabled =
        dest.join("etc/systemd/system/multi-user.target.wants/aware-fileserver.service");
    let enabled = if live && takeover {
        Command::new("systemctl")
            .args(["is-enabled", "--quiet", "aware-fileserver.service"])
            .status()?
            .success()
    } else {
        takeover && old_enabled.is_symlink()
    };
    let active = if live && takeover {
        Command::new("systemctl")
            .args(["is-active", "--quiet", "aware-fileserver.service"])
            .status()?
            .success()
    } else {
        false
    };
    if live && takeover {
        run(Command::new("systemctl").args(["disable", "--now", "aware-fileserver.service"]))?;
    }
    for (source, target, mode) in planned {
        copy(&source, &target, mode)?;
    }
    if publisher {
        for obsolete in hardware::OBSOLETE_USERSPACE_FILES {
            remove(&dest.join(obsolete))?;
        }
        if rename_daemon {
            for (previous, current, enabled, _) in &renamed_units {
                remove(&dest.join("etc/systemd/system").join(previous))?;
                let wants = dest.join("etc/systemd/system/multi-user.target.wants");
                remove(&wants.join(previous))?;
                if !live && *enabled {
                    remove(&wants.join(current))?;
                    std::os::unix::fs::symlink(format!("../{current}"), wants.join(current))?;
                }
            }
            for obsolete in [
                "usr/local/bin/wifiawared",
                "etc/dbus-1/system.d/org.wifiawared.conf",
                "etc/NetworkManager/conf.d/90-wifiawared.conf",
                "etc/udev/rules.d/90-wifiawared.rules",
                "etc/systemd/system/wirelessd.service.d/rtl.conf",
            ] {
                remove(&dest.join(obsolete))?;
            }
        }
    } else {
        if takeover {
            remove(&old_unit)?;
            remove(&old_enabled)?;
            if !live && enabled {
                let enabled_unit =
                    dest.join("etc/systemd/system/multi-user.target.wants/fileserver.service");
                remove(&enabled_unit)?;
                std::os::unix::fs::symlink("../fileserver.service", enabled_unit)?;
            }
        }
        let directory = document
            .get("files")
            .and_then(Item::as_str)
            .context("files 目录缺失")?;
        ensure!(Path::new(directory).is_absolute(), "files 必须为绝对路径");
        fs::create_dir_all(dest.join(directory.trim_start_matches('/')))?;
    }
    if live {
        run(Command::new("systemctl").arg("daemon-reload"))?;
        if publisher {
            run(Command::new("systemctl").args(["reload", "dbus"]))?;
            run(Command::new("udevadm").args(["control", "--reload-rules"]))?;
            run(Command::new("udevadm").args([
                "trigger",
                "--subsystem-match=pci",
                "--action=change",
            ]))?;
            run(Command::new("udevadm").args([
                "trigger",
                "--subsystem-match=net",
                "--action=change",
            ]))?;
            if Command::new("systemctl")
                .args(["is-active", "--quiet", "NetworkManager"])
                .status()?
                .success()
            {
                run(Command::new("nmcli").args(["general", "reload", "conf"]))?;
            }
            for (_, current, enabled, active) in &renamed_units {
                if *enabled {
                    run(Command::new("systemctl").args(["enable", current]))?;
                }
                if *active {
                    run(Command::new("systemctl").args(["start", current]))?;
                }
            }
            for (unit, active) in &dependent_units {
                if *active {
                    run(Command::new("systemctl").args(["start", unit]))?;
                }
            }
        } else {
            if enabled {
                run(Command::new("systemctl").args(["enable", "fileserver.service"]))?;
            }
            if active {
                run(Command::new("systemctl").args(["start", "fileserver.service"]))?;
            }
        }
    }
    Ok(())
}
pub fn driver(root: &Path, cfg: &Settings) -> Result<()> {
    cfg.linux()?;
    let hardware = cfg.hardware()?;
    let kernel = cfg.kernel()?;
    let dest = &cfg.dest;
    let live = dest == Path::new("/");
    let initramfs = live && Path::new(&format!("/boot/initrd.img-{kernel}")).exists();
    let mut secure = false;
    if live {
        require_program("depmod")?;
        if initramfs {
            require_program("update-initramfs")?;
        }
        if Path::new("/sys/firmware/efi").is_dir() {
            require_program("mokutil")?;
            let state = output(Command::new("mokutil").arg("--sb-state").env("LC_ALL", "C"))?;
            if state.contains("SecureBoot enabled") {
                secure = true;
            } else if !state.contains("SecureBoot disabled") {
                bail!("无法识别 Secure Boot 状态：{state}");
            }
        }
    }
    require_program("modinfo")?;
    let artifacts = files(&cfg.driver_dir(root)?.join("source"))?;
    let names = hardware.modules();
    let mut modules = vec![];
    for name in names {
        let file = artifacts
            .iter()
            .find(|p| {
                p.file_name()
                    .is_some_and(|f| f == format!("{name}.ko").as_str())
            })
            .with_context(|| format!("缺少模块 {name}"))?;
        let vermagic = output(Command::new("modinfo").args(["-F", "vermagic"]).arg(file))?;
        ensure!(
            vermagic.split_whitespace().next() == Some(kernel.as_str()),
            "模块 {name} 与内核 {kernel} 不匹配"
        );
        if secure {
            ensure!(
                !output(Command::new("modinfo").args(["-F", "signer"]).arg(file))?.is_empty(),
                "Secure Boot 已启用，模块 {name} 尚未签名"
            );
        }
        modules.push(file);
    }
    let planned = hardware.installation(root, cfg, &modules)?;
    let updates = dest.join("lib/modules").join(&kernel).join("updates");
    for file in modules {
        copy(file, &updates.join(file.file_name().unwrap()), 0o644)?;
    }
    for (source, target) in planned {
        copy(&source, &target, 0o644)?;
    }
    for obsolete in hardware.obsolete(cfg, &updates) {
        remove(&obsolete)?;
    }
    if live {
        run(Command::new("depmod").args(["-a", &kernel]))?;
        if initramfs {
            run(Command::new("update-initramfs").args(["-u", "-k", &kernel]))?;
        }
    }
    Ok(())
}

fn extra_component(
    root: &Path,
    cfg: &Settings,
    package: &str,
    entries: &[(&str, &str, u32)],
    defaults: &[(&str, &str)],
) -> Result<()> {
    cfg.linux()?;
    let mut planned = Vec::new();
    for (from, to, mode) in entries {
        planned.push((root.join(from), cfg.dest.join(to), *mode));
    }
    for (from, to) in defaults {
        if !cfg.dest.join(to).exists() {
            planned.push((root.join(from), cfg.dest.join(to), 0o644));
        }
    }
    let licenses = cfg.user_dir(root).join(package).join("licenses");
    ensure!(
        licenses.join("dependencies.json").is_file(),
        "缺少 {package} 构建与依赖授权"
    );
    for source in files(&licenses)? {
        let target = cfg
            .dest
            .join("usr/local/share/licenses")
            .join(package)
            .join(source.strip_prefix(&licenses)?);
        planned.push((source, target, 0o644));
    }
    for (source, _, _) in &planned {
        ensure!(source.is_file(), "缺少 {}", source.display());
    }
    for (source, destination, mode) in planned {
        copy(&source, &destination, mode)?;
    }
    if cfg.dest == Path::new("/") {
        run(Command::new("systemctl").arg("daemon-reload"))?;
        run(Command::new("dbus-send").args([
            "--system",
            "--type=method_call",
            "--dest=org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus.ReloadConfig",
        ]))?;
    }
    Ok(())
}
pub fn awdl(root: &Path, cfg: &Settings) -> Result<()> {
    let hardware_files = hardware::awdl_installation(root, cfg);
    for (source, _) in &hardware_files {
        ensure!(
            source.is_file(),
            "缺少 {}，请先 build-awdl",
            source.display()
        );
    }
    extra_component(
        root,
        cfg,
        "aware-awdl",
        &[
            (
                "components/wirelessd/wirelessd-awdl.service",
                "etc/systemd/system/wirelessd-awdl.service",
                0o644,
            ),
            (
                "components/wirelessd/NetworkManager-wirelessd.conf",
                "etc/NetworkManager/conf.d/90-wirelessd.conf",
                0o644,
            ),
        ],
        &[("config/awdl.toml", "etc/wirelessd/awdl.toml")],
    )?;
    for (source, destination) in hardware_files {
        copy(&source, &destination, 0o755)?;
    }
    if cfg.dest == Path::new("/") {
        run(Command::new("nmcli").args(["general", "reload"]))?;
    }
    Ok(())
}
pub fn airdrop(root: &Path, cfg: &Settings) -> Result<()> {
    let binary = cfg.user_dir(root).join("airdrop/airdrop");
    ensure!(binary.is_file(), "请先 build-airdrop");
    extra_component(
        root,
        cfg,
        "airdrop",
        &[
            (
                "components/airdrop/airdrop.service",
                "etc/systemd/system/airdrop.service",
                0o644,
            ),
            (
                "components/airdrop/org.airdrop.conf",
                "etc/dbus-1/system.d/org.airdrop.conf",
                0o644,
            ),
            (
                "components/airdrop/ble.py",
                "usr/local/libexec/airdrop/ble.py",
                0o755,
            ),
        ],
        &[("config/airdrop.toml", "etc/airdrop/config.toml")],
    )?;
    copy(&binary, &cfg.dest.join("usr/local/bin/airdrop"), 0o755)
}

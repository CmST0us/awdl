use crate::{copy, output, run, settings::Settings, source::files};
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
        if matches!(
            (vendor.trim(), device.trim()),
            ("0x8086", "0x272b") | ("0x10ec", "0xb852")
        ) {
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
    let name = if publisher {
        "wifiawared"
    } else {
        "fileserver"
    };
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
    let old = dest.join("etc/aware-fileserver/config.toml");
    let defaults = if publisher {
        fs::read_to_string(root.join("config/wifiawared.toml"))?
    } else {
        format!(
            "{}\n{}",
            fs::read_to_string(root.join("config/fileserver-discovery.toml"))?,
            fs::read_to_string(root.join("config/fileserver.toml"))?
        )
    };
    let mut document: DocumentMut = if config.exists() {
        fs::read_to_string(&config)?
    } else {
        defaults.clone()
    }
    .parse()?;
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
                dest.join("usr/local/libexec/wifiawared").join(binary),
                0o755,
            ));
        }
        for (from, to) in [
            (
                "org.wifiawared.conf",
                "etc/dbus-1/system.d/org.wifiawared.conf",
            ),
            (
                "NetworkManager-wifiawared.conf",
                "etc/NetworkManager/conf.d/90-wifiawared.conf",
            ),
        ] {
            planned.push((component.join(from), dest.join(to), 0o644));
        }
        planned.push((
            root.join("components/hostap/LICENSE.hostap"),
            dest.join("usr/local/share/licenses/wifiawared/hostap.txt"),
            0o644,
        ));
        for hardware in ["be200", "rtl8852be"] {
            planned.push((
                root.join("hardware").join(hardware).join("radio.toml"),
                dest.join("usr/local/share/wifi-aware/hardware")
                    .join(hardware)
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
                "# 所选 PCI 的物理控制接口交给 Wi-Fi Aware；udev 重新加载后生效。\nSUBSYSTEM==\"net\", KERNELS==\"{pci}\", ENV{{NM_UNMANAGED}}=\"1\"\n# 为所选 PCI 提供稳定的 systemd 设备依赖名称。\nSUBSYSTEM==\"pci\", KERNEL==\"{pci}\", TAG+=\"systemd\", ENV{{SYSTEMD_ALIAS}}+=\"/sys/subsystem/pci/devices/{pci}\"\n"
            );
            write(&generated.join("90-wifiawared.rules"), &rules)?;
            planned.push((
                generated.join("90-wifiawared.rules"),
                dest.join("etc/udev/rules.d/90-wifiawared.rules"),
                0o644,
            ));
            let unit = format!(
                "# 选定 PCI 的生命周期依赖；修改后执行 systemctl daemon-reload。\n[Unit]\n# PCI 设备出现后才启动，设备消失时停止。\nBindsTo=sys-subsystem-pci-devices-{pci}.device\n# 设备就绪后进入公共无线接管流程。\nAfter=sys-subsystem-pci-devices-{pci}.device\n"
            );
            write(&generated.join("device.conf"), &unit)?;
            planned.push((
                generated.join("device.conf"),
                dest.join("etc/systemd/system/wifiawared.service.d/device.conf"),
                0o644,
            ));
        }
    }
    if !config.exists() || (publisher && cfg.explicit_pci) {
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
    for (source, _, _) in &planned {
        ensure!(source.is_file(), "缺少构建产物 {}", source.display());
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
        for obsolete in [
            "etc/dbus-1/system.d/org.be200.WiFiAware1.conf",
            "etc/systemd/system/wifiawared.service.d/rtl.conf",
            "etc/NetworkManager/conf.d/91-wifiawared-rtl.conf",
        ] {
            remove(&dest.join(obsolete))?;
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
    let names: &[&str] = if hardware == "be200" {
        &[
            "cfg80211",
            "mac80211",
            "iwlwifi",
            "iwldvm",
            "iwlmvm",
            "iwlmld",
            "iwlwifi-compat",
        ]
    } else {
        &["8852be"]
    };
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
    let directory = root.join("hardware").join(hardware);
    let mut firmware = vec![];
    if hardware == "be200" {
        firmware = files(&directory.join("firmware"))?
            .into_iter()
            .filter(|p| p.extension().is_some_and(|e| e == "ucode" || e == "pnvm"))
            .collect();
        ensure!(firmware.len() == 2, "BE200 需要 c108 与 PNVM");
    } else {
        let country = cfg
            .install
            .rtl8852be
            .country
            .as_deref()
            .context("RTL 安装需要明确 country 或 --country")?;
        ensure!(
            country.len() == 2 && country.bytes().all(|b| b.is_ascii_uppercase()),
            "country 必须为两个大写字母"
        );
        let text = fs::read_to_string(directory.join("driver/rtl8852be.conf"))?
            .replace("@COUNTRY@", country);
        write(
            &root
                .join(".build/config")
                .join(&cfg.target)
                .join("rtl8852be.conf"),
            &text,
        )?;
        run(Command::new("python3")
            .arg(directory.join("firmware/tools/u2fw.py"))
            .arg("verify")
            .arg(modules[0])
            .arg(cfg.driver_dir(root)?.join("firmware/candidate.bin")))?;
    }
    let updates = dest.join("lib/modules").join(&kernel).join("updates");
    for file in modules {
        copy(file, &updates.join(file.file_name().unwrap()), 0o644)?;
    }
    if hardware == "be200" {
        remove(&updates.join("compat.ko"))?;
        for file in firmware {
            copy(
                &file,
                &dest.join("lib/firmware").join(file.file_name().unwrap()),
                0o644,
            )?;
        }
        copy(
            &directory.join("driver/be200.conf"),
            &dest.join("etc/modprobe.d/be200.conf"),
            0o644,
        )?;
        if cfg.install.be200.platform_cn {
            copy(
                &directory.join("driver/be200-cn.conf"),
                &dest.join("etc/modprobe.d/be200-cn.conf"),
                0o644,
            )?;
        } else {
            remove(&dest.join("etc/modprobe.d/be200-cn.conf"))?;
        }
        copy(
            &directory.join("firmware/LICENCE.iwlwifi_firmware"),
            &dest.join("usr/local/share/licenses/wifi-aware/be200/LICENCE.iwlwifi_firmware"),
            0o644,
        )?;
        copy(
            &directory.join("driver/COPYING"),
            &dest.join("usr/local/share/licenses/wifi-aware/be200/COPYING"),
            0o644,
        )?;
    } else {
        copy(
            &root
                .join(".build/config")
                .join(&cfg.target)
                .join("rtl8852be.conf"),
            &dest.join("etc/modprobe.d/rtl8852be.conf"),
            0o644,
        )?;
        copy(
            &directory.join("driver/UPSTREAM-LICENSE"),
            &dest.join("usr/local/share/licenses/wifi-aware/rtl8852be/UPSTREAM-LICENSE"),
            0o644,
        )?;
    }
    if live {
        run(Command::new("depmod").args(["-a", &kernel]))?;
        if initramfs {
            run(Command::new("update-initramfs").args(["-u", "-k", &kernel]))?;
        }
    }
    Ok(())
}
pub fn platform(root: &Path, cfg: &Settings) -> Result<()> {
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

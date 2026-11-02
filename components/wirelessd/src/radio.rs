use crate::{
    backend::{Backend, Profile},
    configuration::Configuration,
    control::Control,
    events::Events,
    support::{hex, link_local, redact, unhex, write_configuration},
};
use anyhow::{Context, Result, bail, ensure};
use aware_common::{
    bus,
    unix::{command, interface_exists, random},
};
use std::{
    path::Path,
    process::Stdio,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    time::Duration,
};
use tokio::{
    io::{AsyncBufReadExt, BufReader},
    process::Child,
    task::JoinHandle,
    time::{Instant, timeout_at},
};

pub struct Radio {
    pub cfg: Configuration,
    pub control: Option<Control>,
    pub events: Option<Control>,
    global: Option<Control>,
    pub child: Option<Child>,
    pub links: Option<Events>,
    backend: Option<Backend>,
    profile: Option<Profile>,
    logs: Vec<JoinHandle<()>>,
    phy: String,
    pci: String,
    bootstrap: String,
    owns_bootstrap: bool,
    device_lock: Option<std::fs::File>,
    pub address: String,
    pub awdl_owned: Arc<AtomicBool>,
}

impl Radio {
    pub fn new(cfg: Configuration) -> Self {
        Self {
            cfg,
            control: None,
            events: None,
            global: None,
            child: None,
            links: None,
            backend: None,
            profile: None,
            logs: vec![],
            phy: String::new(),
            pci: String::new(),
            bootstrap: String::new(),
            owns_bootstrap: false,
            device_lock: None,
            address: String::new(),
            awdl_owned: Arc::new(AtomicBool::new(false)),
        }
    }

    async fn interfaces(&self) -> Result<Vec<(String, String)>> {
        let text = command("iw", &["dev"]).await?;
        let target = self.phy.trim_start_matches("phy");
        let mut selected = false;
        let mut result: Vec<(String, String)> = vec![];
        for line in text.lines().map(str::trim) {
            if let Some(phy) = line.strip_prefix("phy#") {
                selected = phy == target;
            } else if selected {
                if let Some(name) = line.strip_prefix("Interface ") {
                    result.push((name.into(), String::new()));
                } else if line == "Unnamed/non-netdev interface" {
                    result.push((String::new(), String::new()));
                } else if let Some(kind) = line.strip_prefix("type ")
                    && let Some(entry) = result.last_mut()
                {
                    entry.1 = kind.into();
                }
            }
        }
        Ok(result)
    }

    pub async fn check_interfaces(&self, require_data: bool) -> Result<()> {
        self.backend.unwrap().check(
            &self.interfaces().await?,
            &self.bootstrap,
            require_data,
            self.awdl_owned.load(Ordering::Acquire),
        )
    }

    fn select_phy(&mut self) -> Result<()> {
        let mut candidates = vec![];
        for entry in std::fs::read_dir("/sys/class/ieee80211").context("未找到已加载的无线驱动")?
        {
            let entry = entry?;
            let device = entry.path().join("device").canonicalize()?;
            let vendor = std::fs::read_to_string(device.join("vendor")).unwrap_or_default();
            let product = std::fs::read_to_string(device.join("device")).unwrap_or_default();
            let Some(backend) = Backend::detect(vendor.trim(), product.trim()) else {
                continue;
            };
            let pci = device
                .file_name()
                .context("PCI 地址缺失")?
                .to_string_lossy()
                .into_owned();
            if self.cfg.pci.is_empty() || self.cfg.pci == pci {
                candidates.push((
                    entry.file_name().to_string_lossy().into_owned(),
                    pci,
                    backend,
                ));
            }
        }
        ensure!(
            candidates.len() <= 1,
            "发现多块受支持网卡，请通过 pci 选择目标"
        );
        let (phy, pci, backend) = candidates
            .pop()
            .context("未找到已就绪的受支持网卡，请检查驱动及 pci 配置")?;
        self.phy = phy;
        self.pci = pci;
        self.backend = Some(backend);
        self.device_lock = Some(aware_common::unix::lock(
            &Path::new("/run/lock").join(format!("wireless-service-{}.lock", self.pci)),
            false,
        )?);
        Ok(())
    }

    async fn check_system_supplicant(&self) -> Result<()> {
        let connection = bus::connect().await?;
        let dbus = zbus::fdo::DBusProxy::new(&connection).await?;
        if dbus
            .name_has_owner("fi.w1.wpa_supplicant1".try_into()?)
            .await?
        {
            let proxy = bus::proxy(
                &connection,
                "fi.w1.wpa_supplicant1",
                "/fi/w1/wpa_supplicant1",
                "fi.w1.wpa_supplicant1",
            )
            .await?;
            let result: zbus::Result<zbus::zvariant::OwnedObjectPath> =
                proxy.call("GetInterface", &(&self.bootstrap,)).await;
            match result {
                Ok(_) => bail!(
                    "系统 wpa_supplicant 正在管理 {}，请先释放该接口",
                    self.bootstrap
                ),
                Err(zbus::Error::MethodError(name, _, _))
                    if name.as_str() == "fi.w1.wpa_supplicant1.InterfaceUnknown" => {}
                Err(e) => return Err(e.into()),
            }
        }
        let remote = Path::new("/run/wpa_supplicant").join(&self.bootstrap);
        if remote.exists()
            && let Ok(mut probe) = Control::open(
                &remote,
                &self.cfg.runtime.join("system-probe"),
                self.cfg.timeouts.control_reply_seconds,
            )
            && probe.request("PING").await.is_ok_and(|s| s == "PONG")
        {
            bail!(
                "系统 wpa_supplicant 正在管理 {}，请先释放该接口",
                self.bootstrap
            );
        }
        Ok(())
    }

    pub async fn request(&mut self, command: &str) -> Result<String> {
        self.control
            .as_mut()
            .context("hostap 控制通道未就绪")?
            .request(command)
            .await
    }
    async fn global_request(&mut self, command: &str) -> Result<String> {
        self.global
            .as_mut()
            .context("hostap 全局通道未就绪")?
            .request(command)
            .await
    }
    pub async fn install_schedule(&mut self) -> Result<()> {
        let map = self.profile.as_ref().unwrap().map(self.cfg.frequency)?;
        self.request(&format!("NAN_SCHED_CONFIG_MAP map_id=1 {map}"))
            .await?;
        Ok(())
    }

    async fn check_unmanaged(&self, name: &str) -> Result<()> {
        let out = tokio::process::Command::new("nmcli")
            .args(["--get-values", "GENERAL.NM-MANAGED", "device", "show", name])
            .env("LC_ALL", "C")
            .output()
            .await?;
        ensure!(
            out.status.success() && String::from_utf8_lossy(&out.stdout).trim() == "no",
            "接口 {name} 仍由 NetworkManager 管理；请加载设备规则并释放接口"
        );
        Ok(())
    }

    async fn prepare(&mut self, awdl: bool) -> Result<bool> {
        std::fs::create_dir_all(&self.cfg.state)?;
        std::fs::create_dir_all(&self.cfg.runtime)?;
        self.links = Some(Events::links()?);
        self.select_phy()?;
        let backend = self.backend.unwrap();
        if awdl {
            ensure!(
                !backend.shared_awdl(),
                "该硬件由 wirelessd.service 同时承载 NAN 与 AWDL"
            );
            backend.awdl_program()?;
        }
        for name in ["nan0", "ndi0", "awdl0"] {
            ensure!(!interface_exists(name)?, "接口名 {name} 已被占用");
        }
        let entries = self.interfaces().await?;
        for (name, _) in &entries {
            ensure!(
                command("iw", &["dev", name, "link"])
                    .await?
                    .contains("Not connected."),
                "接口 {name} 已关联，无法接管"
            );
        }
        if let Some(name) = backend.bootstrap(&entries)? {
            self.bootstrap = name;
        } else {
            self.bootstrap = "waboot0".into();
            ensure!(!interface_exists("waboot0")?, "接口名 waboot0 已被占用");
            command(
                "iw",
                &[
                    "phy",
                    &self.phy,
                    "interface",
                    "add",
                    &self.bootstrap,
                    "type",
                    "managed",
                ],
            )
            .await?;
            self.owns_bootstrap = true;
        }
        let network_manager = command("systemctl", &["is-active", "--quiet", "NetworkManager"])
            .await
            .is_ok();
        if network_manager {
            self.check_unmanaged(&self.bootstrap).await?;
            for (name, _) in &entries {
                if name != &self.bootstrap {
                    self.check_unmanaged(name).await?;
                }
            }
        }
        self.check_system_supplicant().await?;
        if !backend.removes_bootstrap() {
            command("ip", &["link", "set", &self.bootstrap, "up"]).await?;
        }
        eprintln!(
            "使用 {} pci={} phy={} bootstrap={}",
            backend.name(),
            self.pci,
            self.phy,
            self.bootstrap
        );
        Ok(network_manager)
    }

    pub fn shared_awdl(&self) -> Result<Option<(String, String, u8, Option<String>)>> {
        let backend = self.backend.context("无线设备尚未启动")?;
        if !backend.shared_awdl() {
            return Ok(None);
        }
        let channel = backend.awdl_channel()?;
        let constraint = (self.cfg.frequency != 5000 + u32::from(channel) * 5).then(|| {
            format!(
                "AWDL 需要 NAN 数据频率 {} MHz",
                5000 + u32::from(channel) * 5
            )
        });
        Ok(Some((
            backend.awdl_program()?.into(),
            self.phy.clone(),
            channel,
            constraint,
        )))
    }

    pub async fn prepare_awdl(&mut self) -> Result<(String, String, u8)> {
        self.prepare(true).await?;
        Ok((
            self.backend.unwrap().awdl_program()?.into(),
            self.bootstrap.clone(),
            self.backend.unwrap().awdl_channel()?,
        ))
    }

    pub async fn start(&mut self) -> Result<()> {
        let network_manager = self.prepare(false).await?;
        let backend = self.backend.unwrap();
        self.profile = Some(self.backend.unwrap().profile(&self.cfg)?);
        let nan = self.cfg.state.join("nan.conf");
        let nik_path = self.cfg.state.join("local-nik");
        let bootstrap = self.cfg.runtime.join("bootstrap.conf");
        let global = self.cfg.runtime.join("global");
        let ctrl = self.cfg.runtime.join("ctrl");
        write_configuration(&nan, "update_config=1\n", true)?;
        write_configuration(&bootstrap, "update_config=0\n", false)?;
        if !nik_path.exists() {
            write_configuration(&nik_path, &format!("{}\n", hex(&random::<16>()?)), true)?;
        }
        let nik = std::fs::read_to_string(&nik_path)?.trim().to_owned();
        unhex(&nik, 16).context("local-nik 必须包含 16 字节身份密钥")?;
        let watch = Events::directory(&self.cfg.runtime)?;
        let mut child = tokio::process::Command::new(&self.cfg.supplicant)
            .arg("-g")
            .arg(&global)
            .env("LC_ALL", "C")
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .kill_on_drop(true)
            .spawn()?;
        let stdout = child.stdout.take().unwrap();
        let stderr = child.stderr.take().unwrap();
        self.child = Some(child);
        self.logs.push(tokio::spawn(log_output(stdout)));
        self.logs.push(tokio::spawn(log_output(stderr)));
        let deadline =
            Instant::now() + Duration::from_secs(u64::from(self.cfg.timeouts.hostap_ready_seconds));
        while !global.exists() {
            tokio::select! {
                status = self.child.as_mut().unwrap().wait() => bail!("hostap 启动失败: {}", status?),
                event = watch.changed() => event?,
                _ = tokio::time::sleep_until(deadline) => bail!("hostap 控制接口未在配置期限内就绪"),
            }
        }
        self.global = Some(Control::open(
            &global,
            &self.cfg.runtime.join("global-client"),
            self.cfg.timeouts.control_reply_seconds,
        )?);
        self.global_request(&format!(
            "INTERFACE_ADD {}\t{}\t{}\t{}\t\t",
            self.bootstrap,
            bootstrap.display(),
            backend.driver(),
            ctrl.display()
        ))
        .await?;
        if backend.removes_bootstrap() {
            self.owns_bootstrap = true;
        }
        self.global_request(&format!(
            "INTERFACE_ADD nan0\t{}\t{}\t{}\t\t\tcreate\tnan",
            nan.display(),
            backend.driver(),
            ctrl.display()
        ))
        .await?;
        self.global_request(&format!("INTERFACE_REMOVE {}", self.bootstrap))
            .await?;
        if backend.removes_bootstrap() {
            command("iw", &["dev", &self.bootstrap, "del"]).await?;
            self.owns_bootstrap = false;
        }
        self.check_interfaces(false).await?;
        self.control = Some(Control::open(
            &ctrl.join("nan0"),
            &self.cfg.runtime.join("command"),
            self.cfg.timeouts.control_reply_seconds,
        )?);
        let mut events = Control::open(
            &ctrl.join("nan0"),
            &self.cfg.runtime.join("events"),
            self.cfg.timeouts.control_reply_seconds,
        )?;
        events.request("ATTACH").await?;
        self.events = Some(events);
        self.request(&format!("NAN_SET nik {nik}")).await?;
        let nan = &self.profile.as_ref().unwrap().nan;
        let settings = [
            format!("dual_band {}", u8::from(nan.dual_band)),
            format!("master_pref {}", nan.master_preference),
            format!(
                "discovery_beacon_interval {}",
                nan.discovery_beacon_interval_tu
            ),
        ];
        for setting in settings {
            self.request(&format!("NAN_SET {setting}")).await?;
        }
        for setting in [
            "beacon_prot 1",
            "pairing_setup 0",
            "pairing_verification 1",
            "bootstrap_config 2,2,0",
        ] {
            self.request(&format!("NAN_SET {setting}")).await?;
        }
        self.request("NAN_START").await?;
        let deadline =
            Instant::now() + Duration::from_secs(u64::from(self.cfg.timeouts.cluster_join_seconds));
        let mut buffer = vec![0; 65536];
        loop {
            let size = tokio::select! {
                result = self.events.as_ref().unwrap().socket.recv(&mut buffer) => result?,
                status = self.child.as_mut().unwrap().wait() => bail!("hostap 在合簇期间退出: {}", status?),
                _ = tokio::time::sleep_until(deadline) => bail!("NAN 集群未能在配置期限内建立日程"),
            };
            let text = String::from_utf8_lossy(&buffer[..size]);
            ensure!(!text.contains("NAN-STOPPED"), "固件已停止 NAN");
            if text.contains("NAN-CLUSTER-JOIN ") {
                match self.install_schedule().await {
                    Ok(()) => break,
                    Err(error) => eprintln!("集群日程未提交，等待新的合簇事件: {error}"),
                }
            }
        }
        self.global_request(&format!(
            "INTERFACE_ADD ndi0\t\t{}\t{}\t\t\tcreate\tnan_data",
            backend.driver(),
            ctrl.display()
        ))
        .await?;
        command("ip", &["link", "set", "ndi0", "addrgenmode", "none"]).await?;
        command("ip", &["link", "set", "ndi0", "up"]).await?;
        if network_manager && backend.data_requires_nm_release() {
            self.check_unmanaged("ndi0").await?;
        }
        self.address = link_local(
            &std::fs::read_to_string("/sys/class/net/ndi0/address")?,
            None,
        )?;
        command(
            "ip",
            &[
                "-6",
                "addr",
                "replace",
                &format!("{}/64", self.address),
                "dev",
                "ndi0",
                "nodad",
            ],
        )
        .await?;
        self.check_interfaces(true).await
    }

    pub async fn close(&mut self) -> Result<()> {
        ensure!(
            !self.awdl_owned.load(Ordering::Acquire),
            "AWDL 资源尚未释放，保留 NAN 所有权"
        );
        self.events.take();
        self.control.take();
        self.global.take();
        let owned_interfaces = self.child.is_some();
        let deadline =
            Instant::now() + Duration::from_secs(u64::from(self.cfg.timeouts.hostap_stop_seconds));
        let mut result = Ok(());
        if let Some(mut child) = self.child.take() {
            if let Some(pid) = child.id() {
                // Child 仍持有未回收的进程，PID 不会在这里复用。
                unsafe {
                    libc::kill(pid as i32, libc::SIGTERM);
                }
                match timeout_at(deadline, child.wait()).await {
                    Ok(Ok(_)) => {}
                    Ok(Err(error)) => result = Err(error.into()),
                    Err(_) => {
                        let _ = child.kill().await;
                        result = Err(anyhow::anyhow!("hostap 未在退出期限内结束"));
                    }
                }
            }
        }
        for log in self.logs.drain(..) {
            let _ = log.await;
        }
        if self.owns_bootstrap {
            match command("iw", &["dev", &self.bootstrap, "del"]).await {
                Ok(_) => self.owns_bootstrap = false,
                Err(error) => result = Err(error.context("清理启动接口")),
            }
        }
        if owned_interfaces {
            let removed = async {
                while interface_exists("nan0")? || interface_exists("ndi0")? {
                    self.links
                        .as_ref()
                        .context("缺少接口退出事件源")?
                        .changed()
                        .await?;
                }
                Ok::<_, anyhow::Error>(())
            };
            match timeout_at(deadline, removed).await {
                Ok(Ok(())) => {}
                Ok(Err(error)) => result = Err(error),
                Err(_) => result = Err(anyhow::anyhow!("NAN 接口未在退出期限内删除")),
            }
        }
        self.links.take();
        for name in [
            "bootstrap.conf",
            "global",
            "global-client",
            "command",
            "events",
        ] {
            let _ = std::fs::remove_file(self.cfg.runtime.join(name));
        }
        result
    }
}

async fn log_output(stream: impl tokio::io::AsyncRead + Unpin) {
    let mut lines = BufReader::new(stream).lines();
    while let Ok(Some(line)) = lines.next_line().await {
        eprintln!("hostap: {}", redact(&line));
    }
}

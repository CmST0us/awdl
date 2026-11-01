use crate::{
    api::{self, Request},
    radio::Radio,
    support::{field, hex, link_local, unhex},
};
use anyhow::{Context, Result, bail, ensure};
use aware_common::{
    bus,
    unix::{command, monotonic_micros, random},
};
use futures_util::StreamExt;
use std::{
    collections::BTreeMap,
    sync::{Arc, RwLock},
    time::Duration,
};
use tokio::sync::mpsc;
use zbus::zvariant::OwnedObjectPath;

#[derive(Default)]
struct Peer {
    key: String,
    instance: String,
    service: String,
    pin: String,
    verifying: bool,
    pending_until: u64,
    active_at: u64,
}
struct Session {
    peer: String,
    id: String,
    ndi: String,
    address: String,
    service: Option<String>,
    pending_until: u64,
}
struct Registration {
    name: String,
    owner: String,
    port: u16,
    protocol: u8,
    handle: String,
    state: Arc<RwLock<api::ServiceState>>,
    dirty: bool,
}
pub struct Publisher {
    pub radio: Radio,
    peers: BTreeMap<String, Peer>,
    sessions: Vec<Session>,
    services: BTreeMap<String, Registration>,
    state: Arc<RwLock<api::ManagerState>>,
    bus: zbus::Connection,
    sender: mpsc::Sender<Request>,
    receiver: mpsc::Receiver<Request>,
    next_id: u64,
    pin: String,
    pair_until: u64,
    schedule_until: u64,
}

impl Publisher {
    pub async fn new(mut radio: Radio) -> Result<Self> {
        let prepared = async {
            let connection = bus::connect().await?;
            let (sender, receiver) = mpsc::channel(32);
            let state = Arc::new(RwLock::new(api::ManagerState::default()));
            connection
                .object_server()
                .at(
                    bus::PATH,
                    api::Manager {
                        sender: sender.clone(),
                        state: state.clone(),
                        address: radio.address.clone(),
                        frequency: radio.cfg.frequency,
                    },
                )
                .await?;
            connection
                .object_server()
                .at(bus::PATH, zbus::fdo::ObjectManager)
                .await?;
            Ok::<_, anyhow::Error>((connection, sender, receiver, state))
        }
        .await;
        let (connection, sender, receiver, state) = match prepared {
            Ok(values) => values,
            Err(error) => {
                if let Err(cleanup) = radio.close().await {
                    eprintln!("无线清理: {cleanup:#}");
                }
                return Err(error);
            }
        };
        Ok(Self {
            radio,
            bus: connection,
            state,
            sender,
            receiver,
            next_id: 0,
            pin: String::new(),
            pair_until: 0,
            schedule_until: 0,
            peers: BTreeMap::new(),
            sessions: vec![],
            services: BTreeMap::new(),
        })
    }

    async fn publish(&mut self, name: &str) -> Result<String> {
        let mut ssi = vec![2, name.len() as u8, 0];
        ssi.extend_from_slice(name.as_bytes());
        let mut info = vec![];
        for item in [
            format!("vendorName={}", self.radio.cfg.identity.vendor_name),
            format!("modelName={}", self.radio.cfg.identity.model_name),
            format!("pairingName={}", self.radio.cfg.identity.pairing_name),
        ] {
            info.push(item.len() as u8);
            info.extend_from_slice(item.as_bytes());
        }
        ssi.extend_from_slice(&[4, info.len() as u8, 0]);
        ssi.extend(info);
        self.radio.request(&format!("NAN_PUBLISH service_name={name} sync=1 fsd=0 data_path=1 gtk_required=1 srv_proto_type=2 cipher_suites=5,7 pbm=2 ttl=86400 ssi={}", hex(&ssi))).await
    }
    async fn register(
        &mut self,
        name: String,
        transport: String,
        port: u16,
        owner: String,
    ) -> Result<OwnedObjectPath, api::Error> {
        let protocol = match transport.as_str() {
            "tcp" => 6,
            "udp" => 17,
            _ => return Err(api::Error::NotSupported("传输协议必须为 tcp 或 udp".into())),
        };
        if port == 0
            || !(6..128).contains(&name.len())
            || !name.ends_with(&format!("._{transport}"))
            || !name
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"_-.".contains(&b))
        {
            return Err(api::Error::InvalidArgs(
                "服务名必须为 ASCII 名称，后缀匹配传输协议，端口为 1–65535".into(),
            ));
        }
        if self
            .services
            .values()
            .any(|s| s.name.eq_ignore_ascii_case(&name))
        {
            return Err(api::Error::AlreadyRegistered("服务名已注册".into()));
        }
        let handle = self.publish(&name).await?;
        self.next_id += 1;
        let path = format!("{}/services/s{}", bus::PATH, self.next_id);
        let state = Arc::new(RwLock::new(api::ServiceState {
            publish_id: handle.clone(),
            sessions: vec![],
        }));
        let service = api::Service {
            sender: self.sender.clone(),
            state: state.clone(),
            path: path.clone(),
            name: name.clone(),
            transport,
            port,
            owner: owner.clone(),
        };
        if let Err(e) = self.bus.object_server().at(path.as_str(), service).await {
            let _ = self
                .radio
                .request(&format!("NAN_CANCEL_PUBLISH publish_id={handle}"))
                .await;
            return Err(api::Error::Failed(e.to_string()));
        }
        eprintln!("上层服务已注册 service={name} port={port} publish_id={handle} owner={owner}");
        self.services.insert(
            path.clone(),
            Registration {
                name,
                owner,
                port,
                protocol,
                handle,
                state,
                dirty: false,
            },
        );
        self.manager_changed()
            .await
            .map_err(|e| api::Error::Failed(e.to_string()))?;
        Ok(OwnedObjectPath::try_from(path).unwrap())
    }
    async fn manager_changed(&self) -> Result<()> {
        {
            let mut state = self.state.write().unwrap();
            state.services = self
                .services
                .keys()
                .map(|s| OwnedObjectPath::try_from(s.clone()).unwrap())
                .collect();
            state.pair_until = self.pair_until;
        }
        api::changed(
            &self.bus,
            bus::PATH,
            bus::MANAGER,
            &["Services", "PairingActive", "PairingEndsAt"],
        )
        .await?;
        Ok(())
    }
    async fn end_session(&mut self, index: usize) -> Result<()> {
        let s = &self.sessions[index];
        self.radio
            .request(&format!(
                "NAN_NDP_TERMINATE peer_nmi={} init_ndi={} ndp_id={}",
                s.peer, s.ndi, s.id
            ))
            .await?;
        Ok(())
    }
    async fn remove_session(&mut self, index: usize) {
        let session = self.sessions.remove(index);
        if let Some(peer) = self.peers.get_mut(&session.peer) {
            peer.active_at = monotonic_micros();
        }
        if !session.address.is_empty()
            && !self.sessions.iter().any(|s| s.address == session.address)
            && let Err(e) = command(
                "ip",
                &["-6", "neigh", "del", &session.address, "dev", "ndi0"],
            )
            .await
        {
            eprintln!("移除 NDP 邻居: {e}");
        }
        if let Some(service) = session
            .service
            .and_then(|path| self.services.get_mut(&path))
        {
            service.dirty = true;
        }
    }
    async fn unregister(&mut self, path: &str) -> Result<()> {
        for peer in self.peers.values_mut().filter(|p| p.service == path) {
            peer.service.clear();
            peer.pending_until = 0;
            peer.pin.clear();
        }
        for index in (0..self.sessions.len()).rev() {
            if self.sessions[index].service.as_deref() != Some(path) {
                continue;
            }
            if self.end_session(index).await.is_ok() {
                self.remove_session(index).await;
            } else {
                self.sessions[index].service = None;
                self.sessions[index].pending_until = monotonic_micros()
                    + u64::from(self.radio.cfg.timeouts.ndp_end_seconds) * 1_000_000;
            }
        }
        if let Some(service) = self.services.remove(path) {
            if let Err(e) = self
                .radio
                .request(&format!("NAN_CANCEL_PUBLISH publish_id={}", service.handle))
                .await
            {
                eprintln!("撤销服务发布 service={}: {e}", service.name);
            }
            self.bus
                .object_server()
                .remove::<api::Service, _>(path)
                .await?;
            self.manager_changed().await?;
            eprintln!("上层服务已注销 service={}", service.name);
        }
        Ok(())
    }
    async fn close_pairing(&mut self) -> Result<()> {
        self.radio.request("NAN_SET pairing_setup 0").await?;
        self.pin.clear();
        self.pair_until = 0;
        self.manager_changed().await
    }
    async fn open_pairing(&mut self, seconds: u32) -> Result<String, api::Error> {
        if !(30..=600).contains(&seconds) {
            return Err(api::Error::InvalidArgs("配对窗口应为 30–600 秒".into()));
        }
        if self.services.is_empty() {
            return Err(api::Error::NotReady("请先启动并注册上层服务".into()));
        }
        if self.peers.values().any(|p| p.pending_until != 0) {
            return Err(api::Error::Busy("认证正在执行".into()));
        }
        let number = loop {
            let n = u32::from_ne_bytes(random()?);
            if n < u32::MAX - u32::MAX % 1_000_000 {
                break n % 1_000_000;
            }
        };
        self.radio.request("NAN_SET pairing_setup 1").await?;
        self.pin = format!("{number:06}");
        self.pair_until = monotonic_micros() + u64::from(seconds) * 1_000_000;
        self.manager_changed().await?;
        Ok(self.pin.clone())
    }
    async fn start_pairing(&mut self, address: &str) {
        let peer = self.peers.get_mut(address).unwrap();
        let Some(service) = self.services.get(&peer.service) else {
            peer.pending_until = 0;
            return;
        };
        let mut cmd = format!(
            "NAN_PAIR {address} handle={} peer_instance_id={} auth={} cipher=CCMP responder",
            service.handle,
            peer.instance,
            if peer.verifying { 2 } else { 1 }
        );
        if !peer.verifying {
            cmd.push_str(&format!(" pwd_hex={}", hex(peer.pin.as_bytes())));
        }
        peer.pin.clear();
        if let Err(e) = self.radio.request(&cmd).await {
            peer.pending_until = 0;
            eprintln!("认证请求失败 peer={address} error={e}");
        }
    }
    fn service_for_handle(&self, handle: &str) -> Option<String> {
        self.services
            .iter()
            .find_map(|(path, s)| (s.handle == handle).then(|| path.clone()))
    }
    fn find_session(&self, peer: &str, ndi: &str, id: &str) -> Option<usize> {
        self.sessions
            .iter()
            .position(|s| s.peer == peer && s.ndi == ndi && s.id == id)
    }

    async fn event(&mut self, event: &str) -> Result<()> {
        let now = monotonic_micros();
        let required = |name| field(event, name).context("hostap NAN 事件缺少有效字段");
        if event.contains("NAN-CLUSTER-JOIN ") {
            match self.radio.install_schedule().await {
                Ok(()) => self.schedule_until = 0,
                Err(error) => {
                    if self.schedule_until == 0 {
                        self.schedule_until = now
                            + u64::from(self.radio.cfg.timeouts.cluster_join_seconds) * 1_000_000;
                    }
                    eprintln!("集群日程未提交，等待新的合簇事件: {error}");
                }
            }
        } else if event.contains("NAN-NIK-RECEIVED") {
            self.radio.request("SAVE_CONFIG").await?;
            eprintln!("对端身份已保存");
        } else if event.contains("NAN-PAIRING-REQUEST") {
            let address = required("peer_nmi")?;
            let id = required("instance_id")?;
            let service = self.service_for_handle(id);
            let verifying = field(event, "verify") == Some("1");
            let peer = self.peers.entry(address.into()).or_insert_with(|| Peer {
                active_at: now,
                ..Default::default()
            });
            if peer.pending_until != 0 {
                return Ok(());
            }
            let reason = if service.is_none() {
                Some("服务实例未注册")
            } else if field(event, "csid") != Some("7")
                || (!verifying && field(event, "key_mgmt") != Some("SAE"))
            {
                Some("认证参数不受支持")
            } else if !verifying && self.pin.is_empty() {
                Some("配对窗口未开启")
            } else {
                None
            };
            if let Some(reason) = reason {
                eprintln!("认证请求失败 peer={address} error={reason}");
                return Ok(());
            }
            peer.active_at = now;
            peer.service = service.unwrap();
            peer.verifying = verifying;
            peer.instance = id.into();
            peer.pin = if verifying {
                String::new()
            } else {
                self.pin.clone()
            };
            peer.pending_until =
                now + u64::from(self.radio.cfg.timeouts.authentication_seconds) * 1_000_000;
            self.start_pairing(address).await;
        } else if event.contains("NAN-PAIRING-STATUS") {
            let address = required("addr")?;
            let peer = self.peers.entry(address.into()).or_insert_with(|| Peer {
                active_at: now,
                ..Default::default()
            });
            peer.pending_until = 0;
            peer.active_at = now;
            peer.key.clear();
            peer.pin.clear();
            let status = field(event, "status").unwrap_or("unknown");
            if status == "success"
                && let Some(key) = field(event, "nd_pmk")
                && unhex(key, 32).is_ok()
            {
                peer.key = key.into();
            }
            eprintln!("认证完成 peer={address} status={status}");
        } else if event.contains("NAN-NDP-REQUEST ") {
            let address = required("peer_nmi")?;
            let ndi = required("init_ndi")?;
            let id = required("ndp_id")?;
            let base = format!("peer_nmi={address} init_ndi={ndi} ndp_id={id}");
            let path = field(event, "publish_inst_id").and_then(|id| self.service_for_handle(id));
            let key = self
                .peers
                .get(address)
                .map(|p| p.key.as_str())
                .unwrap_or("");
            let reason = if path.is_none() {
                Some("服务实例未注册")
            } else if key.is_empty() {
                Some("对端未完成认证")
            } else if self.find_session(address, ndi, id).is_some() {
                Some("会话已存在")
            } else if field(event, "csid") != Some("7") {
                Some("加密套件不受支持")
            } else {
                None
            };
            if let Some(reason) = reason {
                eprintln!("NDP 已拒绝 peer={address} id={id} reason={reason}");
            } else {
                let path = path.unwrap();
                let service = &self.services[&path];
                let ssi = hex(&[
                    0,
                    2,
                    0,
                    service.port as u8,
                    (service.port >> 8) as u8,
                    1,
                    1,
                    0,
                    service.protocol,
                ]);
                let cmd = format!(
                    "NAN_NDP_RESPONSE accept {base} handle={} ndi=ndi0 csid=7 pmk={key} ssi={ssi}",
                    service.handle
                );
                match self.radio.request(&cmd).await {
                    Ok(_) => {
                        self.sessions.push(Session {
                            peer: address.into(),
                            ndi: ndi.into(),
                            id: id.into(),
                            address: String::new(),
                            service: Some(path),
                            pending_until: now
                                + u64::from(self.radio.cfg.timeouts.ndp_setup_seconds) * 1_000_000,
                        });
                        return Ok(());
                    }
                    Err(e) => eprintln!("NDP 响应失败 peer={address} id={id} error={e}"),
                }
            }
            if let Err(e) = self
                .radio
                .request(&format!("NAN_NDP_RESPONSE reject {base}"))
                .await
            {
                eprintln!("NDP 拒绝失败 peer={address} id={id} error={e}");
            }
        } else if event.contains("NAN-NDP-CONNECTED ") {
            let peer = required("peer")?;
            let ndi = required("peer_ndi")?;
            let id = required("ndp_id")?;
            let Some(index) = self.find_session(peer, ndi, id) else {
                let _ = self
                    .radio
                    .request(&format!(
                        "NAN_NDP_TERMINATE peer_nmi={peer} init_ndi={ndi} ndp_id={id}"
                    ))
                    .await;
                return Ok(());
            };
            let Some(path) = self.sessions[index].service.clone() else {
                return Ok(());
            };
            let address = link_local(ndi, field(event, "interface_id"))?;
            command(
                "ip",
                &[
                    "-6",
                    "neigh",
                    "replace",
                    &address,
                    "lladdr",
                    ndi,
                    "nud",
                    "permanent",
                    "dev",
                    "ndi0",
                ],
            )
            .await?;
            self.sessions[index].address = address.clone();
            self.sessions[index].pending_until = 0;
            if let Some(service) = self.services.get_mut(&path) {
                service.dirty = true;
                eprintln!(
                    "NDP 已连接 service={} peer={peer} id={id} address={address}",
                    service.name
                );
            }
        } else if event.contains("NAN-NDP-DISCONNECTED") {
            let peer = required("peer")?;
            let ndi = required("peer_ndi")?;
            let id = required("ndp_id")?;
            if let Some(index) = self.find_session(peer, ndi, id) {
                self.remove_session(index).await;
            }
            eprintln!(
                "NDP 已断开 peer={peer} id={id} reason={}",
                field(event, "reason").unwrap_or("unknown")
            );
        } else if event.contains("NAN-PUBLISH-TERMINATED") {
            if let Some(path) =
                field(event, "publish_id").and_then(|id| self.service_for_handle(id))
            {
                let name = self.services[&path].name.clone();
                match self.publish(&name).await {
                    Ok(handle) => {
                        let service = self.services.get_mut(&path).unwrap();
                        service.handle = handle;
                        service.dirty = true;
                    }
                    Err(e) => {
                        eprintln!("重新发布失败 service={name}: {e}");
                        self.unregister(&path).await?;
                    }
                }
            }
        } else if event.contains("NAN-STOPPED") {
            bail!("固件已停止 NAN");
        }
        Ok(())
    }
    async fn refresh_services(&mut self) -> Result<()> {
        for (path, service) in &mut self.services {
            if !service.dirty {
                continue;
            }
            {
                let mut state = service.state.write().unwrap();
                state.publish_id = service.handle.clone();
                state.sessions = self
                    .sessions
                    .iter()
                    .filter(|s| {
                        s.service.as_deref() == Some(path.as_str()) && !s.address.is_empty()
                    })
                    .map(|s| {
                        (
                            s.peer.clone(),
                            s.ndi.clone(),
                            s.id.clone(),
                            s.address.clone(),
                        )
                    })
                    .collect();
            }
            api::changed(&self.bus, path, bus::SERVICE, &["Sessions", "PublishId"]).await?;
            service.dirty = false;
        }
        Ok(())
    }
    async fn expire(&mut self, wireless_changed: bool) -> Result<()> {
        let now = monotonic_micros();
        ensure!(
            self.schedule_until == 0 || now < self.schedule_until,
            "集群变化后未能在配置期限内提交日程"
        );
        for i in (0..self.sessions.len()).rev() {
            let expired =
                self.sessions[i].pending_until != 0 && now >= self.sessions[i].pending_until;
            if !expired && (self.sessions[i].service.is_some() || !wireless_changed) {
                continue;
            }
            match self.end_session(i).await {
                Ok(_) => self.remove_session(i).await,
                Err(e) if expired => {
                    return Err(e).context("NDP 清理期限已到，终止无线服务以释放资源");
                }
                Err(_) => {}
            }
        }
        let addresses: Vec<_> = self.peers.keys().cloned().collect();
        for address in addresses {
            let peer = self.peers.get_mut(&address).unwrap();
            if peer.pending_until != 0 && now >= peer.pending_until {
                eprintln!("认证请求失败 peer={address} error=未收到认证结果");
                peer.pending_until = 0;
                peer.active_at = now;
                peer.pin.clear();
            }
            let peer = self.peers.get_mut(&address).unwrap();
            if peer.pending_until != 0 || self.sessions.iter().any(|s| s.peer == address) {
                peer.active_at = now;
            } else if now - peer.active_at
                >= u64::from(self.radio.cfg.retention.inactive_peer_seconds) * 1_000_000
            {
                self.peers.remove(&address);
            }
        }
        if self.pair_until != 0 && now >= self.pair_until {
            self.close_pairing().await?;
        }
        Ok(())
    }
    fn next_deadline(&self) -> Option<u64> {
        let mut deadlines = vec![self.pair_until, self.schedule_until];
        for (address, peer) in &self.peers {
            deadlines.push(peer.pending_until);
            if peer.pending_until == 0 && !self.sessions.iter().any(|s| &s.peer == address) {
                deadlines.push(
                    peer.active_at
                        + u64::from(self.radio.cfg.retention.inactive_peer_seconds) * 1_000_000,
                );
            }
        }
        deadlines.extend(self.sessions.iter().map(|s| s.pending_until));
        deadlines.into_iter().filter(|d| *d != 0).min()
    }
    async fn handle(&mut self, request: Request) -> Result<()> {
        match request {
            Request::Register {
                name,
                transport,
                port,
                owner,
                reply,
            } => {
                let result = self.register(name, transport, port, owner).await;
                let _ = reply.send(result);
            }
            Request::Unregister { path, owner, reply } => {
                if self.services.get(&path).is_some_and(|s| s.owner == owner) {
                    // 先完成方法调用，释放对象接口的读锁，再移除对象。
                    let _ = reply.send(Ok(()));
                    self.unregister(&path).await?;
                } else {
                    let _ = reply.send(Err(api::Error::AccessDenied(
                        "只能注销当前连接拥有的服务".into(),
                    )));
                }
            }
            Request::Open { seconds, reply } => {
                let result = self.open_pairing(seconds).await;
                let _ = reply.send(result);
            }
            Request::Close(reply) => {
                let result = self.close_pairing().await.map_err(Into::into);
                let _ = reply.send(result);
            }
            Request::Radio(reply) => {
                let result = self.radio.request("NAN_STATUS").await.map_err(Into::into);
                let _ = reply.send(result);
            }
        }
        Ok(())
    }
    pub async fn run(&mut self, shutdown: impl std::future::Future<Output = ()>) -> Result<()> {
        let bus = self.bus.clone();
        let dbus = zbus::fdo::DBusProxy::new(&bus).await?;
        let mut owners = dbus.receive_name_owner_changed().await?;
        self.bus.request_name(bus::NAME).await?;
        eprintln!(
            "Publisher 已启动 frequency={} address={}",
            self.radio.cfg.frequency, self.radio.address
        );
        tokio::pin!(shutdown);
        let mut buffer = vec![0; 65536];
        let mut wireless_changed = false;
        loop {
            // 排空已收到的无线事件后再处理本机管理请求。
            loop {
                match self
                    .radio
                    .events
                    .as_ref()
                    .unwrap()
                    .socket
                    .try_recv(&mut buffer)
                {
                    Ok(n) => {
                        ensure!(n > 0, "hostap 事件通道已关闭");
                        self.event(std::str::from_utf8(&buffer[..n])?).await?;
                        wireless_changed = true;
                    }
                    Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                    Err(e) => return Err(e.into()),
                }
            }
            self.expire(wireless_changed).await?;
            wireless_changed = false;
            self.refresh_services().await?;
            let deadline = self.next_deadline();
            let expires = async {
                match deadline {
                    Some(at) => {
                        tokio::time::sleep(Duration::from_micros(
                            at.saturating_sub(monotonic_micros()),
                        ))
                        .await
                    }
                    None => std::future::pending::<()>().await,
                }
            };
            tokio::select! {
                _ = &mut shutdown => return Ok(()),
                result = self.radio.events.as_ref().unwrap().socket.recv(&mut buffer) => { let n = result?; ensure!(n > 0, "hostap 事件通道已关闭"); self.event(std::str::from_utf8(&buffer[..n])?).await?; wireless_changed = true; }
                request = self.receiver.recv() => { if let Some(request) = request { self.handle(request).await?; } }
                signal = owners.next() => {
                    let signal = signal.context("D-Bus 连接已关闭")?; let args = signal.args()?;
                    if args.name().as_str().starts_with(':') && args.new_owner().is_none() {
                        let paths: Vec<_> = self.services.iter().filter(|(_, s)| s.owner == args.name().as_str()).map(|(p, _)| p.clone()).collect();
                        for path in paths { self.unregister(&path).await?; }
                    }
                }
                _ = expires => {},
                status = self.radio.child.as_mut().unwrap().wait() => bail!("wpa_supplicant 已退出: {}", status?),
                result = self.radio.links.as_ref().unwrap().changed() => { result?; self.radio.check_interfaces(true).await?; },
            }
        }
    }
    pub async fn close(&mut self) -> Result<()> {
        let _ = self.bus.release_name(bus::NAME).await;
        self.radio.close().await
    }
}

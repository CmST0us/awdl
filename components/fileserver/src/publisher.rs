use crate::configuration::Configuration;
use anyhow::{Context, Result, bail, ensure};
use aware_common::bus;
use futures_util::StreamExt;
use std::{
    collections::{HashMap, HashSet},
    net::IpAddr,
    sync::{Arc, Mutex},
};
use tokio::sync::watch;
use zbus::{MessageStream, zvariant::OwnedObjectPath};

#[derive(Default)]
pub struct Sessions {
    inner: Mutex<SessionState>,
}
#[derive(Default)]
struct SessionState {
    addresses: HashSet<IpAddr>,
    connections: HashMap<u64, (IpAddr, watch::Sender<bool>)>,
    next_id: u64,
}
impl Sessions {
    fn update(&self, addresses: HashSet<IpAddr>) {
        let mut state = self.inner.lock().unwrap();
        for (address, cancel) in state.connections.values() {
            if state.addresses.contains(address) && !addresses.contains(address) {
                let _ = cancel.send(true);
            }
        }
        state.addresses = addresses;
    }
    pub fn authorized(&self, address: IpAddr) -> bool {
        self.inner.lock().unwrap().addresses.contains(&address)
    }
    pub fn track(&self, address: IpAddr) -> (u64, watch::Receiver<bool>) {
        let mut state = self.inner.lock().unwrap();
        state.next_id += 1;
        let id = state.next_id;
        let (tx, rx) = watch::channel(false);
        state.connections.insert(id, (address, tx));
        (id, rx)
    }
    pub fn untrack(&self, id: u64) {
        self.inner.lock().unwrap().connections.remove(&id);
    }
    pub fn stop(&self) {
        for (_, cancel) in self.inner.lock().unwrap().connections.values() {
            let _ = cancel.send(true);
        }
    }
}
pub struct Publisher {
    connection: zbus::Connection,
    owner: String,
    path: String,
    refresh_lock: tokio::sync::Mutex<()>,
    pub sessions: Arc<Sessions>,
    failure: watch::Sender<Option<String>>,
}
pub struct Signals {
    owners: MessageStream,
    service: MessageStream,
}
impl Publisher {
    pub async fn open(
        sessions: Arc<Sessions>,
        failure: watch::Sender<Option<String>>,
    ) -> Result<(Self, Signals)> {
        let connection = bus::connect().await?;
        let owners = MessageStream::for_match_rule(format!("type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='{}'", bus::NAME).as_str(), &connection, Some(32)).await?;
        let dbus = zbus::fdo::DBusProxy::new(&connection).await?;
        let owner = dbus
            .get_name_owner(bus::NAME.try_into()?)
            .await?
            .to_string();
        let service = MessageStream::for_match_rule(
            format!("type='signal',sender='{owner}'").as_str(),
            &connection,
            Some(64),
        )
        .await?;
        let manager = bus::proxy(&connection, &owner, bus::PATH, bus::MANAGER).await?;
        let interface: String = manager.get_property("Interface").await?;
        let transports: Vec<String> = manager.get_property("SupportedTransports").await?;
        ensure!(
            interface == "ndi0" && transports.iter().any(|s| s == "tcp"),
            "文件服务需要 ndi0 和 TCP Publisher 能力"
        );
        drop(manager);
        drop(dbus);
        Ok((
            Self {
                connection,
                owner,
                path: String::new(),
                refresh_lock: tokio::sync::Mutex::new(()),
                sessions,
                failure,
            },
            Signals { owners, service },
        ))
    }
    pub async fn register(&mut self, cfg: &Configuration) -> Result<()> {
        let manager = bus::proxy(&self.connection, &self.owner, bus::PATH, bus::MANAGER).await?;
        let path: OwnedObjectPath = manager
            .call("RegisterService", &(&cfg.service, "tcp", cfg.port))
            .await?;
        self.path = path.to_string();
        self.refresh().await
    }
    pub async fn refresh(&self) -> Result<()> {
        let _lock = self.refresh_lock.lock().await;
        let service = bus::proxy(&self.connection, &self.owner, &self.path, bus::SERVICE).await?;
        let sessions: Vec<bus::Session> = service.get_property("Sessions").await?;
        let addresses = sessions
            .into_iter()
            .map(|s| s.3.parse())
            .collect::<std::result::Result<HashSet<IpAddr>, _>>()?;
        self.sessions.update(addresses);
        Ok(())
    }
    pub fn fail(&self, error: impl ToString) {
        let _ = self.failure.send(Some(error.to_string()));
    }
    pub async fn watch(&self, mut signals: Signals) -> Result<()> {
        loop {
            tokio::select! {
                signal = signals.owners.next() => {
                    let signal = signal.context("D-Bus 连接已关闭")??;
                    let (name, previous, _): (String, String, String) = signal.body().deserialize()?;
                    if name == bus::NAME && previous == self.owner { bail!("wifiawared 已退出"); }
                }
                signal = signals.service.next() => {
                    let signal = signal.context("D-Bus 连接已关闭")??;
                    let header = signal.header();
                    if header.interface().is_some_and(|i| i.as_str() == bus::OBJECT_MANAGER) && header.member().is_some_and(|m| m.as_str() == "InterfacesRemoved") {
                        let (path, _): (OwnedObjectPath, Vec<String>) = signal.body().deserialize()?;
                        if path.as_str() == self.path { bail!("文件服务注册已撤销"); }
                    } else if header.interface().is_some_and(|i| i.as_str() == bus::PROPERTIES) && header.member().is_some_and(|m| m.as_str() == "PropertiesChanged") && header.path().is_some_and(|p| p.as_str() == self.path) { self.refresh().await?; }
                }
            }
        }
    }
}

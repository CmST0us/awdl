use aware_common::bus::{self, Session};
use std::sync::{Arc, RwLock};
use tokio::sync::{mpsc, oneshot};
use zbus::{message::Header, zvariant::OwnedObjectPath};

#[derive(Debug, zbus::DBusError)]
#[zbus(prefix = "org", impl_display = true)]
pub enum Error {
    #[zbus(name = "freedesktop.DBus.Error.InvalidArgs")]
    InvalidArgs(String),
    #[zbus(name = "freedesktop.DBus.Error.NotSupported")]
    NotSupported(String),
    #[zbus(name = "freedesktop.DBus.Error.AccessDenied")]
    AccessDenied(String),
    #[zbus(name = "freedesktop.DBus.Error.Failed")]
    Failed(String),
    #[zbus(name = "wirelessd1.Error.AlreadyRegistered")]
    AlreadyRegistered(String),
    #[zbus(name = "wirelessd1.Error.NotReady")]
    NotReady(String),
    #[zbus(name = "wirelessd1.Error.Busy")]
    Busy(String),
    #[zbus(name = "wirelessd1.Error.Radio")]
    Radio(String),
}
impl From<anyhow::Error> for Error {
    fn from(e: anyhow::Error) -> Self {
        Self::Radio(e.to_string())
    }
}
pub type Reply<T> = oneshot::Sender<Result<T, Error>>;
pub enum Request {
    Register {
        name: String,
        transport: String,
        port: u16,
        owner: String,
        reply: Reply<OwnedObjectPath>,
    },
    Unregister {
        path: String,
        owner: String,
        reply: Reply<()>,
    },
    Open {
        seconds: u32,
        reply: Reply<String>,
    },
    Close(Reply<()>),
    Radio(Reply<String>),
}
#[derive(Default)]
pub struct ManagerState {
    pub services: Vec<OwnedObjectPath>,
    pub pair_until: u64,
}
#[derive(Default)]
pub struct ServiceState {
    pub publish_id: String,
    pub sessions: Vec<Session>,
}
pub struct Manager {
    pub sender: mpsc::Sender<Request>,
    pub state: Arc<RwLock<ManagerState>>,
    pub address: String,
    pub frequency: u32,
}
pub struct Service {
    pub sender: mpsc::Sender<Request>,
    pub state: Arc<RwLock<ServiceState>>,
    pub path: String,
    pub name: String,
    pub transport: String,
    pub port: u16,
    pub owner: String,
}
async fn request<T>(
    sender: &mpsc::Sender<Request>,
    make: impl FnOnce(Reply<T>) -> Request,
) -> Result<T, Error> {
    let (tx, rx) = oneshot::channel();
    sender
        .send(make(tx))
        .await
        .map_err(|_| Error::Failed("无线服务已停止".into()))?;
    rx.await
        .map_err(|_| Error::Failed("无线服务已停止".into()))?
}
fn sender(header: &Header<'_>) -> Result<String, Error> {
    header
        .sender()
        .map(ToString::to_string)
        .ok_or_else(|| Error::AccessDenied("缺少总线发送者".into()))
}

#[zbus::interface(name = "org.wirelessd1.Manager")]
impl Manager {
    async fn register_service(
        &self,
        name: String,
        transport: String,
        port: u16,
        #[zbus(header)] header: Header<'_>,
    ) -> Result<OwnedObjectPath, Error> {
        let owner = sender(&header)?;
        request(&self.sender, |reply| Request::Register {
            name,
            transport,
            port,
            owner,
            reply,
        })
        .await
    }
    async fn open_pairing(&self, seconds: u32) -> Result<String, Error> {
        request(&self.sender, |reply| Request::Open { seconds, reply }).await
    }
    async fn close_pairing(&self) -> Result<(), Error> {
        request(&self.sender, Request::Close).await
    }
    async fn get_radio_status(&self) -> Result<String, Error> {
        request(&self.sender, Request::Radio).await
    }
    #[zbus(property(emits_changed_signal = "const"))]
    fn supported_roles(&self) -> Vec<&str> {
        vec!["publisher"]
    }
    #[zbus(property(emits_changed_signal = "const"))]
    fn supported_transports(&self) -> Vec<&str> {
        vec!["tcp", "udp"]
    }
    #[zbus(property(emits_changed_signal = "const"))]
    fn supports_multiple_services(&self) -> bool {
        true
    }
    #[zbus(property(emits_changed_signal = "const"))]
    fn can_initiate_data_path(&self) -> bool {
        false
    }
    #[zbus(property(emits_changed_signal = "const"))]
    fn interface(&self) -> &str {
        "ndi0"
    }
    #[zbus(property(emits_changed_signal = "const"))]
    fn local_address(&self) -> &str {
        &self.address
    }
    #[zbus(property(emits_changed_signal = "const"))]
    fn configured_frequency(&self) -> u32 {
        self.frequency
    }
    #[zbus(property)]
    fn services(&self) -> Vec<OwnedObjectPath> {
        self.state.read().unwrap().services.clone()
    }
    #[zbus(property)]
    fn pairing_active(&self) -> bool {
        self.state.read().unwrap().pair_until != 0
    }
    #[zbus(property)]
    fn pairing_ends_at(&self) -> u64 {
        self.state.read().unwrap().pair_until
    }
}

#[zbus::interface(name = "org.wirelessd1.Service")]
impl Service {
    async fn unregister(&self, #[zbus(header)] header: Header<'_>) -> Result<(), Error> {
        let owner = sender(&header)?;
        request(&self.sender, |reply| Request::Unregister {
            path: self.path.clone(),
            owner,
            reply,
        })
        .await
    }
    #[zbus(property(emits_changed_signal = "const"))]
    fn name(&self) -> &str {
        &self.name
    }
    #[zbus(property(emits_changed_signal = "const"))]
    fn transport(&self) -> &str {
        &self.transport
    }
    #[zbus(property(emits_changed_signal = "const"))]
    fn port(&self) -> u16 {
        self.port
    }
    #[zbus(property(emits_changed_signal = "const"))]
    fn owner(&self) -> &str {
        &self.owner
    }
    #[zbus(property)]
    fn publish_id(&self) -> String {
        self.state.read().unwrap().publish_id.clone()
    }
    #[zbus(property)]
    fn sessions(&self) -> Vec<Session> {
        self.state.read().unwrap().sessions.clone()
    }
}

pub async fn changed(
    connection: &zbus::Connection,
    path: &str,
    interface: &str,
    fields: &[&str],
) -> zbus::Result<()> {
    connection
        .emit_signal(
            None::<&str>,
            path,
            bus::PROPERTIES,
            "PropertiesChanged",
            &(
                interface,
                std::collections::HashMap::<&str, zbus::zvariant::Value<'_>>::new(),
                fields,
            ),
        )
        .await
}

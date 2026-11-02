use std::time::Duration;

pub const NAME: &str = "org.wirelessd1";
pub const PATH: &str = "/org/wirelessd1";
pub const MANAGER: &str = "org.wirelessd1.Manager";
pub const SERVICE: &str = "org.wirelessd1.Service";
pub const PROPERTIES: &str = "org.freedesktop.DBus.Properties";
pub const OBJECT_MANAGER: &str = "org.freedesktop.DBus.ObjectManager";
pub type Session = (String, String, String, String);

pub async fn connect() -> zbus::Result<zbus::Connection> {
    zbus::connection::Builder::system()?
        .method_timeout(Duration::from_secs(5))
        .build()
        .await
}

pub async fn proxy<'a>(
    bus: &'a zbus::Connection,
    owner: &'a str,
    path: &'a str,
    interface: &'a str,
) -> zbus::Result<zbus::Proxy<'a>> {
    zbus::proxy::Builder::new(bus)
        .destination(owner)?
        .path(path)?
        .interface(interface)?
        .cache_properties(zbus::proxy::CacheProperties::No)
        .build()
        .await
}

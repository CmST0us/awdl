mod api;
mod awdl;
#[path = "../../../hardware/radio.rs"]
mod backend;
mod configuration;
mod control;
mod events;
mod publisher;
mod radio;
mod support;

use anyhow::{Result, bail, ensure};
use aware_common::bus;
use std::collections::HashMap;
use zbus::zvariant::{OwnedObjectPath, OwnedValue};

fn value_json(value: &OwnedValue) -> Result<serde_json::Value> {
    use serde_json::json;
    let sig = value.value_signature().to_string();
    Ok(match sig.as_str() {
        "s" => json!(<&str>::try_from(value)?),
        "b" => json!(bool::try_from(value)?),
        "q" => json!(u16::try_from(value)?),
        "u" => json!(u32::try_from(value)?),
        "t" => json!(u64::try_from(value)?),
        "as" => json!(Vec::<String>::try_from(value.try_clone()?)?),
        "ao" => json!(Vec::<OwnedObjectPath>::try_from(value.try_clone()?)?.iter().map(|p| p.as_str()).collect::<Vec<_>>()),
        "a(ssss)" => json!(Vec::<bus::Session>::try_from(value.try_clone()?)?.into_iter().map(|(peer, peer_ndi, id, address)| json!({"peer":peer,"peer_ndi":peer_ndi,"id":id,"address":address})).collect::<Vec<_>>()),
        _ => bail!("未知 D-Bus 属性类型 {sig}"),
    })
}
fn properties(
    values: HashMap<String, OwnedValue>,
) -> Result<serde_json::Map<String, serde_json::Value>> {
    values
        .into_iter()
        .map(|(key, value)| Ok((key, value_json(&value)?)))
        .collect()
}
async fn client(operation: &str, seconds: u32) -> Result<()> {
    use serde_json::json;
    let connection = bus::connect().await?;
    let manager = bus::proxy(&connection, bus::NAME, bus::PATH, bus::MANAGER).await?;
    let output = match operation {
        "pair" => {
            let pin: String = manager.call("OpenPairing", &(seconds,)).await?;
            json!({"ok":true,"pin":pin,"seconds":seconds})
        }
        "close-pairing" => {
            let _: () = manager.call("ClosePairing", &()).await?;
            json!({"ok":true})
        }
        _ => {
            let p = bus::proxy(&connection, bus::NAME, bus::PATH, bus::PROPERTIES).await?;
            let all: HashMap<String, OwnedValue> = p.call("GetAll", &(bus::MANAGER,)).await?;
            let objects =
                bus::proxy(&connection, bus::NAME, bus::PATH, bus::OBJECT_MANAGER).await?;
            let all_objects: HashMap<
                OwnedObjectPath,
                HashMap<String, HashMap<String, OwnedValue>>,
            > = objects.call("GetManagedObjects", &()).await?;
            let mut services = vec![];
            for (path, mut interfaces) in all_objects {
                if let Some(values) = interfaces.remove(bus::SERVICE) {
                    let mut item = properties(values)?;
                    item.insert("path".into(), json!(path.as_str()));
                    services.push(item);
                }
            }
            let radio: String = manager.call("GetRadioStatus", &()).await?;
            json!({"ok":true,"manager":properties(all)?,"services":services,"radio":radio})
        }
    };
    println!("{}", serde_json::to_string_pretty(&output)?);
    Ok(())
}
async fn run() -> Result<()> {
    let mut config = "/etc/wirelessd/config.toml".to_string();
    let mut seconds = 120;
    let mut operation = None;
    let mut protocol = "nan".to_string();
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        let (key, inline) = arg
            .split_once('=')
            .map_or((arg.as_str(), None), |(k, v)| (k, Some(v.to_string())));
        match key {
            "--help" | "-h" => {
                println!(
                    "用法: wirelessd [--config PATH] [--protocol nan|awdl] {{serve|status|pair|close-pairing}} [--seconds 30..600]"
                );
                return Ok(());
            }
            "--config" | "-c" => {
                config = inline
                    .or_else(|| args.next())
                    .ok_or_else(|| anyhow::anyhow!("缺少配置路径"))?
            }
            "--protocol" => {
                protocol = inline
                    .or_else(|| args.next())
                    .ok_or_else(|| anyhow::anyhow!("缺少协议"))?;
                ensure!(
                    ["nan", "awdl"].contains(&protocol.as_str()),
                    "协议必须为 nan 或 awdl"
                );
            }
            "--seconds" | "-s" => {
                seconds = inline
                    .or_else(|| args.next())
                    .ok_or_else(|| anyhow::anyhow!("缺少配对秒数"))?
                    .parse()?;
                ensure!((30..=600).contains(&seconds), "配对窗口应为 30–600 秒");
            }
            "serve" | "status" | "pair" | "close-pairing"
                if operation.is_none() && inline.is_none() =>
            {
                operation = Some(arg)
            }
            _ => bail!("未知参数 {arg}"),
        }
    }
    let operation = operation
        .ok_or_else(|| anyhow::anyhow!("命令必须是 serve、status、pair 或 close-pairing"))?;
    if operation != "serve" {
        if protocol == "awdl" {
            ensure!(operation == "status", "AWDL 配对与传输由 airdrop 管理");
            let connection = bus::connect().await?;
            let proxy =
                bus::proxy(&connection, bus::NAME, bus::PATH, "org.wirelessd1.Awdl").await?;
            let status: String = proxy.get_property("Status").await?;
            println!("{status}");
            return Ok(());
        }
        return client(&operation, seconds).await;
    }
    let cfg = configuration::Configuration::load(&config)?;
    std::fs::create_dir_all(&cfg.runtime)?;
    let _lock = aware_common::unix::lock(&cfg.runtime.join("lock"), false)?;
    let mut radio = radio::Radio::new(cfg);
    let shutdown = aware_common::unix::shutdown()?;
    tokio::pin!(shutdown);
    if protocol == "awdl" {
        return awdl::run(radio, shutdown).await;
    }
    let start = tokio::select! {
        result = radio.start() => result,
        _ = &mut shutdown => return radio.close().await,
    };
    if let Err(e) = start {
        if let Err(cleanup) = radio.close().await {
            eprintln!("无线清理: {cleanup:#}");
        }
        return Err(e);
    }
    let mut publisher = publisher::Publisher::new(radio).await?;
    let result = publisher.run(shutdown).await;
    let closed = publisher.close().await;
    result.and(closed)
}
#[tokio::main]
async fn main() {
    aware_common::unix::restrict_permissions();
    if let Err(e) = run().await {
        eprintln!("{e:#}");
        std::process::exit(1);
    }
}

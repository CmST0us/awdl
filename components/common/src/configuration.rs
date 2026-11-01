use anyhow::Result;
use serde::de::DeserializeOwned;

/// 表按字段覆盖，数组和值整体替换；最终类型负责拒绝未知字段。
pub fn overlay(base: &mut toml::Value, update: toml::Value) {
    match (base, update) {
        (toml::Value::Table(base), toml::Value::Table(update)) => {
            for (key, value) in update {
                if let Some(previous) = base.get_mut(&key) {
                    overlay(previous, value);
                } else {
                    base.insert(key, value);
                }
            }
        }
        (base, value) => *base = value,
    }
}

pub fn load<T: DeserializeOwned>(defaults: &str, supplied: &str) -> Result<T> {
    let mut value: toml::Value = toml::from_str(defaults)?;
    overlay(&mut value, toml::from_str(supplied)?);
    Ok(value.try_into()?)
}

use anyhow::{Result, ensure};
use serde::Deserialize;
use std::{fs, path::Path};

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Contract {
    service: String,
}
#[derive(Deserialize)]
struct Publisher {
    identity: Identity,
}
#[derive(Deserialize)]
struct Identity {
    vendor_name: String,
    model_name: String,
    pairing_name: String,
}
fn xml(text: &str) -> String {
    text.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
}
fn setting(root: &Path, configuration: &str, key: &str) -> Result<String> {
    let text = fs::read_to_string(root.join("ios/Configurations").join(configuration))?;
    text.lines()
        .filter_map(|line| line.split_once('='))
        .find(|(name, _)| name.trim() == key)
        .map(|(_, value)| value.trim().to_string())
        .ok_or_else(|| anyhow::anyhow!("{configuration} 缺少 {key}"))
}
pub fn generate(root: &Path) -> Result<()> {
    let contract: Contract = toml::from_str(&fs::read_to_string(
        root.join("config/fileserver-discovery.toml"),
    )?)?;
    ensure!(
        (6..128).contains(&contract.service.len())
            && contract.service.ends_with("._tcp")
            && contract
                .service
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"_-.".contains(&b)),
        "无效的文件服务发现名称"
    );
    let publisher: Publisher =
        toml::from_str(&fs::read_to_string(root.join("config/wirelessd.toml"))?)?;
    let replacements = [
        ("__AWARE_FILE_SERVICE__", contract.service),
        ("__AWARE_VENDOR_NAME__", publisher.identity.vendor_name),
        ("__AWARE_MODEL_NAME__", publisher.identity.model_name),
        ("__AWARE_PAIRING_NAME__", publisher.identity.pairing_name),
        (
            "__AWARE_CLIENT_BUNDLE__",
            setting(root, "AwareClient.xcconfig", "PRODUCT_BUNDLE_IDENTIFIER")?,
        ),
        (
            "__AWARE_CLIENT_DISPLAY__",
            setting(root, "AwareClient.xcconfig", "AWARE_DISPLAY_NAME")?,
        ),
    ];
    let directory = root.join(".build/config/ios");
    fs::create_dir_all(&directory)?;
    for (app, output) in [
        ("AwareClient", "Info.plist"),
        ("AwareLab", "AwareLab-Info.plist"),
    ] {
        let template = fs::read_to_string(root.join("ios").join(app).join("Info.plist"))?;
        ensure!(
            template.contains("__AWARE_FILE_SERVICE__"),
            "{app}/Info.plist 缺少服务契约占位符"
        );
        let mut result = template;
        for (placeholder, value) in &replacements {
            result = result.replace(placeholder, &xml(value));
        }
        let path = directory.join(output);
        if fs::read_to_string(&path).ok().as_deref() != Some(&result) {
            fs::write(path, result)?;
        }
    }
    Ok(())
}

use anyhow::{Result, ensure};
use serde::Deserialize;
use std::{fs, path::Path};

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Contract {
    service: String,
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
        let result = template.replace("__AWARE_FILE_SERVICE__", &contract.service);
        let path = directory.join(output);
        if fs::read_to_string(&path).ok().as_deref() != Some(&result) {
            fs::write(path, result)?;
        }
    }
    Ok(())
}

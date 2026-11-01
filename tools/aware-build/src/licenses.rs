use crate::{copy, env_or, output, source::files};
use anyhow::{Context, Result, ensure};
use serde_json::Value;
use std::{
    collections::HashSet,
    fs,
    path::{Path, PathBuf},
    process::Command,
};

pub fn collect(root: &Path, name: &str, target: &str, destination: &Path) -> Result<()> {
    let metadata: Value = serde_json::from_str(&output(
        Command::new(env_or("CARGO", "cargo"))
            .current_dir(root)
            .args([
                "metadata",
                "--locked",
                "--format-version",
                "1",
                "--filter-platform",
                target,
            ]),
    )?)?;
    let packages = metadata["packages"]
        .as_array()
        .context("缺少 Cargo packages")?;
    let nodes = metadata["resolve"]["nodes"]
        .as_array()
        .context("缺少 Cargo resolve")?;
    let package = packages
        .iter()
        .find(|p| p["name"] == name && p["source"].is_null())
        .context("找不到项目 package")?;
    let mut queue = vec![package["id"].as_str().unwrap().to_string()];
    let mut included = HashSet::new();
    while let Some(id) = queue.pop() {
        if !included.insert(id.clone()) {
            continue;
        }
        if let Some(node) = nodes.iter().find(|n| n["id"] == id) {
            for dependency in node["dependencies"]
                .as_array()
                .context("缺少 dependencies")?
            {
                queue.push(dependency.as_str().unwrap().to_string());
            }
        }
    }
    let parent = destination.parent().context("授权输出目录无效")?;
    fs::create_dir_all(parent)?;
    let temporary = tempfile::Builder::new()
        .prefix("licenses-")
        .tempdir_in(parent)?;
    let mut inventory = vec![];
    for package in packages
        .iter()
        .filter(|p| !p["source"].is_null() && included.contains(p["id"].as_str().unwrap()))
    {
        let directory = Path::new(package["manifest_path"].as_str().unwrap())
            .parent()
            .unwrap();
        let output = temporary.path().join(format!(
            "{}-{}",
            package["name"].as_str().unwrap(),
            package["version"].as_str().unwrap()
        ));
        let mut count = 0;
        for entry in fs::read_dir(directory)? {
            let entry = entry?;
            let name = entry.file_name().to_string_lossy().to_ascii_uppercase();
            if ["LICENSE", "LICENCE", "COPYING", "COPYRIGHT", "NOTICE"]
                .iter()
                .any(|prefix| name.starts_with(prefix))
            {
                let entries = if entry.file_type()?.is_dir() {
                    files(&entry.path())?
                } else {
                    vec![entry.path()]
                };
                for file in entries {
                    copy(&file, &output.join(file.strip_prefix(directory)?), 0o644)?;
                    count += 1;
                }
            }
        }
        if let Some(file) = package["license_file"].as_str() {
            let file = PathBuf::from(file);
            let source = if file.is_absolute() {
                file
            } else {
                directory.join(file)
            };
            copy(&source, &output.join("package-license.txt"), 0o644)?;
            count += 1;
        }
        ensure!(count > 0, "依赖 {} 没有可交付的授权文件", package["name"]);
        inventory.push(serde_json::json!({"name": package["name"], "version":package["version"], "source":package["source"], "repository":package["repository"], "license":package["license"]}));
    }
    let sysroot = output(Command::new(env_or("RUSTC", "rustc")).args(["--print", "sysroot"]))?;
    let rust_docs = Path::new(&sysroot).join("share/doc/rust");
    copy(
        &rust_docs.join("COPYRIGHT-library.html"),
        &temporary.path().join("rust/COPYRIGHT-library.html"),
        0o644,
    )?;
    for file in files(&rust_docs.join("licenses"))? {
        copy(
            &file,
            &temporary
                .path()
                .join("rust/licenses")
                .join(file.strip_prefix(rust_docs.join("licenses"))?),
            0o644,
        )?;
    }
    let toolchain = output(Command::new(env_or("RUSTC", "rustc")).arg("--version"))?;
    fs::write(
        temporary.path().join("dependencies.json"),
        serde_json::to_vec_pretty(
            &serde_json::json!({"toolchain":toolchain,"target":target,"packages":inventory}),
        )?,
    )?;
    if destination.exists() {
        fs::remove_dir_all(destination)?;
    }
    fs::rename(temporary.path(), destination)?;
    Ok(())
}

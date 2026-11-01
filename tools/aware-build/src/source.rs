use crate::{copy, run};
use anyhow::{Context, Result};
use serde::Deserialize;
use std::{
    fs,
    path::{Path, PathBuf},
    process::Command,
};

#[derive(Deserialize)]
struct Source {
    archive: String,
    directory: String,
    packaging_archive: Option<String>,
    packaging_series: Option<String>,
}
fn series(path: &Path) -> Result<Vec<PathBuf>> {
    Ok(fs::read_to_string(path)?
        .lines()
        .map(|line| line.split('#').next().unwrap().trim())
        .filter(|s| !s.is_empty())
        .map(PathBuf::from)
        .collect())
}
fn apply(source: &Path, patches: &Path) -> Result<()> {
    for name in series(&patches.join("series"))? {
        run(Command::new("patch")
            .current_dir(source)
            .args(["--batch", "--forward", "--fuzz=0", "-E", "-p1", "-i"])
            .arg(patches.join(name)))?;
    }
    Ok(())
}
fn download(url: &str, path: &Path) -> Result<()> {
    run(Command::new("curl")
        .args([
            "--fail",
            "--location",
            "--connect-timeout",
            "30",
            "--speed-limit",
            "1",
            "--speed-time",
            "30",
            "--output",
        ])
        .arg(path)
        .arg(url))
}
fn extract(archive: &Path, directory: &Path) -> Result<()> {
    run(Command::new("tar")
        .arg("-xf")
        .arg(archive)
        .arg("-C")
        .arg(directory))
}
pub fn files(directory: &Path) -> Result<Vec<PathBuf>> {
    let mut result = vec![];
    if directory.exists() {
        for entry in fs::read_dir(directory)? {
            let entry = entry?;
            if entry.file_type()?.is_dir() {
                result.extend(files(&entry.path())?);
            } else if entry.file_type()?.is_file() {
                result.push(entry.path());
            }
        }
    }
    result.sort();
    Ok(result)
}
pub fn prepare(
    root: &Path,
    directory: &Path,
    output: &Path,
    archive: Option<&Path>,
    extra_patches: &[PathBuf],
) -> Result<PathBuf> {
    let manifest = directory.join("source.json");
    let spec: Source = serde_json::from_slice(&fs::read(&manifest)?)?;
    let patch_directories: Vec<_> = std::iter::once(directory.join("patches"))
        .chain(extra_patches.iter().cloned())
        .collect();
    let mut inputs = vec![manifest.clone()];
    for patches in &patch_directories {
        inputs.push(patches.join("series"));
        inputs.extend(
            series(&patches.join("series"))?
                .into_iter()
                .map(|name| patches.join(name)),
        );
    }
    let mut inputs: Vec<_> = inputs
        .into_iter()
        .map(|p| p.strip_prefix(root).map(Path::to_owned))
        .collect::<std::result::Result<_, _>>()?;
    inputs.sort();
    let build = root.join(".build");
    fs::create_dir_all(&build)?;
    let recorded = output.join(".project-inputs");
    let previous: Vec<_> = files(&recorded)?
        .into_iter()
        .map(|p| p.strip_prefix(&recorded).unwrap().to_owned())
        .collect();
    if inputs == previous
        && inputs
            .iter()
            .all(|p| fs::read(root.join(p)).ok() == fs::read(recorded.join(p)).ok())
    {
        println!("{}", output.display());
        return Ok(output.to_path_buf());
    }
    let temporary = tempfile::Builder::new()
        .prefix("source-")
        .tempdir_in(&build)?;
    let downloaded = temporary.path().join("source.tar");
    let archive = if let Some(path) = archive {
        path.to_owned()
    } else {
        download(&spec.archive, &downloaded)?;
        downloaded.clone()
    };
    extract(&archive, temporary.path())?;
    let source = temporary.path().join(&spec.directory);
    if let Some(url) = spec.packaging_archive {
        download(&url, &downloaded)?;
        extract(&downloaded, &source)?;
        let patches = source.join(spec.packaging_series.context("缺少 packaging_series")?);
        apply(&source, patches.parent().context("补丁目录无效")?)?;
    }
    for patches in &patch_directories {
        apply(&source, patches)?;
    }
    copy(&manifest, &source.join("source.json"), 0o644)?;
    for input in inputs {
        copy(
            &root.join(&input),
            &source.join(".project-inputs").join(input),
            0o644,
        )?;
    }
    if output.exists() {
        fs::remove_dir_all(&output)?;
    }
    fs::create_dir_all(output.parent().context("源码目标目录")?)?;
    fs::rename(source, output)?;
    println!("{}", output.display());
    Ok(output.to_path_buf())
}

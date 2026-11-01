use anyhow::{Context, Result, ensure};
use std::{
    fs::{File, OpenOptions},
    io::Read,
    os::{fd::AsRawFd, unix::fs::OpenOptionsExt},
    path::Path,
};

pub fn lock(path: &Path, directory: bool) -> Result<File> {
    let file = if directory {
        File::open(path)?
    } else {
        OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .mode(0o600)
            .open(path)?
    };
    // 文件描述符由 File 持有；锁随它关闭而释放。
    let result = unsafe { libc::flock(file.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) };
    ensure!(
        result == 0,
        "无法取得运行锁 {}: {}",
        path.display(),
        std::io::Error::last_os_error()
    );
    Ok(file)
}

pub fn random<const N: usize>() -> Result<[u8; N]> {
    let mut bytes = [0; N];
    File::open("/dev/urandom")?.read_exact(&mut bytes)?;
    Ok(bytes)
}

pub fn interface_exists(name: &str) -> Result<bool> {
    let name = std::ffi::CString::new(name)?;
    // 网络接口名的占用以内核接口表为准；sysfs 对象可能延后释放。
    Ok(unsafe { libc::if_nametoindex(name.as_ptr()) } != 0)
}

pub fn monotonic_micros() -> u64 {
    let mut value = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    // CLOCK_MONOTONIC 的 timespec 与 PairingEndsAt 的既有单位一致。
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut value) };
    value.tv_sec as u64 * 1_000_000 + value.tv_nsec as u64 / 1_000
}

pub async fn command(program: &str, args: &[&str]) -> Result<String> {
    let output = tokio::process::Command::new(program)
        .args(args)
        .env("LC_ALL", "C")
        .output()
        .await
        .with_context(|| format!("执行 {program}"))?;
    ensure!(
        output.status.success(),
        "{program}: {}{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    Ok(String::from_utf8(output.stdout)?)
}

pub fn shutdown() -> Result<impl std::future::Future<Output = ()>> {
    use tokio::signal::unix::{SignalKind, signal};
    let mut term = signal(SignalKind::terminate())?;
    let mut interrupt = signal(SignalKind::interrupt())?;
    Ok(async move {
        tokio::select! { _ = term.recv() => {}, _ = interrupt.recv() => {} }
    })
}

pub fn restrict_permissions() {
    // 在创建任务和文件之前设置进程的文件创建掩码。
    unsafe {
        libc::umask(0o077);
    }
}

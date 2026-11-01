use anyhow::{Context, Result};
use std::{
    ffi::CString,
    io,
    os::fd::{AsRawFd, FromRawFd, OwnedFd},
    os::unix::ffi::OsStrExt,
    path::Path,
};
use tokio::io::unix::AsyncFd;

/// Linux 就绪通知：由 Tokio 等待 fd，读取后重新盘点实际状态。
pub struct Events(AsyncFd<OwnedFd>);
impl Events {
    pub fn directory(path: &Path) -> Result<Self> {
        let fd = unsafe { libc::inotify_init1(libc::IN_CLOEXEC | libc::IN_NONBLOCK) };
        let result = Self::from_raw(fd)?;
        let path = CString::new(path.as_os_str().as_bytes())?;
        let watch = unsafe {
            libc::inotify_add_watch(
                result.0.get_ref().as_raw_fd(),
                path.as_ptr(),
                libc::IN_CREATE | libc::IN_MOVED_TO,
            )
        };
        if watch < 0 {
            return Err(io::Error::last_os_error()).context("监听控制接口目录");
        }
        Ok(result)
    }
    pub fn links() -> Result<Self> {
        let fd = unsafe {
            libc::socket(
                libc::AF_NETLINK,
                libc::SOCK_RAW | libc::SOCK_CLOEXEC | libc::SOCK_NONBLOCK,
                libc::NETLINK_ROUTE,
            )
        };
        let result = Self::from_raw(fd)?;
        let mut address: libc::sockaddr_nl = unsafe { std::mem::zeroed() };
        address.nl_family = libc::AF_NETLINK as u16;
        address.nl_groups = libc::RTMGRP_LINK as u32;
        let status = unsafe {
            libc::bind(
                result.0.get_ref().as_raw_fd(),
                (&address as *const libc::sockaddr_nl).cast(),
                std::mem::size_of_val(&address) as _,
            )
        };
        if status < 0 {
            return Err(io::Error::last_os_error()).context("监听接口生命周期");
        }
        Ok(result)
    }
    fn from_raw(fd: i32) -> Result<Self> {
        if fd < 0 {
            return Err(io::Error::last_os_error()).context("创建事件 fd");
        }
        // fd 由 OwnedFd 独占，所有读操作使用非阻塞系统调用。
        Ok(Self(unsafe {
            AsyncFd::register(OwnedFd::from_raw_fd(fd))
        }?))
    }
    pub async fn changed(&self) -> Result<()> {
        let mut buffer = [0u8; 16384];
        loop {
            let mut ready = self.0.readable().await?;
            let read = ready.try_io(|inner| {
                let n = unsafe {
                    libc::read(
                        inner.get_ref().as_raw_fd(),
                        buffer.as_mut_ptr().cast(),
                        buffer.len(),
                    )
                };
                if n < 0 {
                    Err(io::Error::last_os_error())
                } else {
                    Ok(())
                }
            });
            match read {
                Ok(result) => return Ok(result?),
                Err(_) => continue,
            }
        }
    }
}

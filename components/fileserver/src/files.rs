use crate::{configuration::Configuration, publisher::Publisher};
use anyhow::Result;
use aware_common::TRANSFER_BLOCK;
use bytes::Bytes;
use http_body_util::{BodyExt, Full, StreamBody, combinators::UnsyncBoxBody};
use hyper::{
    Method, Request, Response, StatusCode,
    body::{Frame, Incoming},
    header::{self, HeaderMap, HeaderValue},
};
use percent_encoding::{NON_ALPHANUMERIC, percent_decode_str, utf8_percent_encode};
use serde::Serialize;
use std::{
    collections::HashSet,
    convert::Infallible,
    fs::File,
    io,
    net::IpAddr,
    os::{fd::AsRawFd, unix::fs::OpenOptionsExt},
    path::Path,
    sync::{Arc, Mutex},
    time::{Instant, SystemTime, UNIX_EPOCH},
};
use tokio::io::{AsyncReadExt, AsyncSeekExt, AsyncWriteExt};

type Body = UnsyncBoxBody<Bytes, io::Error>;
#[derive(Serialize)]
struct RemoteFile {
    name: String,
    size: u64,
}
struct DownloadLog {
    name: String,
    queued: u64,
    expected: u64,
    started: Instant,
}
impl Drop for DownloadLog {
    fn drop(&mut self) {
        eprintln!(
            "HTTP 文件 method=GET name={:?} queued_bytes={} expected_bytes={} seconds={:.3}",
            self.name,
            self.queued,
            self.expected,
            self.started.elapsed().as_secs_f64()
        );
    }
}
pub struct Files {
    pub cfg: Configuration,
    pub publisher: Arc<Publisher>,
    directory: File,
    uploads: Mutex<HashSet<String>>,
}
struct UploadGuard<'a> {
    name: String,
    uploads: &'a Mutex<HashSet<String>>,
}
impl Drop for UploadGuard<'_> {
    fn drop(&mut self) {
        self.uploads.lock().unwrap().remove(&self.name);
    }
}

fn full(bytes: impl Into<Bytes>) -> Body {
    Full::new(bytes.into())
        .map_err(|never: Infallible| match never {})
        .boxed_unsync()
}
fn response(status: StatusCode, text: impl Into<Bytes>) -> Response<Body> {
    let bytes = text.into();
    let len = bytes.len();
    let mut result = Response::new(full(bytes));
    *result.status_mut() = status;
    result
        .headers_mut()
        .insert(header::CONTENT_LENGTH, len.into());
    result
}
fn json(status: StatusCode, value: impl Serialize) -> Response<Body> {
    let mut result = response(
        status,
        serde_json::to_vec(&value).expect("文件元数据可序列化"),
    );
    result.headers_mut().insert(
        header::CONTENT_TYPE,
        HeaderValue::from_static("application/json"),
    );
    result
}
fn text<'a>(headers: &'a HeaderMap, key: &str) -> Option<&'a str> {
    headers.get(key).and_then(|v| v.to_str().ok())
}
fn second(time: SystemTime) -> u64 {
    time.duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}
fn date(headers: &HeaderMap, key: &str) -> Option<u64> {
    text(headers, key)
        .and_then(|s| httpdate::parse_http_date(s).ok())
        .map(second)
}

impl Files {
    pub fn directory(cfg: &Configuration) -> Result<File> {
        std::fs::create_dir_all(&cfg.files)?;
        let directory = aware_common::unix::lock(Path::new(&cfg.files), true)?;
        for entry in std::fs::read_dir(&cfg.files)? {
            let entry = entry?;
            if entry.file_name().to_string_lossy().starts_with(".upload-")
                && entry.file_type()?.is_file()
            {
                std::fs::remove_file(entry.path())?;
            }
        }
        Ok(directory)
    }
    pub fn new(cfg: Configuration, publisher: Arc<Publisher>, directory: File) -> Self {
        Self {
            cfg,
            publisher,
            directory,
            uploads: Mutex::new(HashSet::new()),
        }
    }
    pub async fn serve(
        self: Arc<Self>,
        request: Request<Incoming>,
        address: IpAddr,
    ) -> Result<Response<Body>, Infallible> {
        let head = request.method() == Method::HEAD;
        let put = request.method() == Method::PUT;
        let has_body = request.headers().contains_key(header::TRANSFER_ENCODING)
            || text(request.headers(), "content-length").is_some_and(|s| s != "0");
        let mut result = if let Err(e) = self.publisher.refresh().await {
            self.publisher.fail(e);
            response(StatusCode::SERVICE_UNAVAILABLE, "服务会话不可用")
        } else if !self.publisher.sessions.authorized(address) {
            response(StatusCode::FORBIDDEN, "没有本服务的 NDP 会话")
        } else {
            match self.route(request, address).await {
                Ok(r) => r,
                Err(e) => {
                    eprintln!("HTTP 文件请求失败: {e:#}");
                    response(StatusCode::INTERNAL_SERVER_ERROR, "文件操作失败")
                }
            }
        };
        result
            .headers_mut()
            .insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
        if put || has_body {
            result
                .headers_mut()
                .insert(header::CONNECTION, HeaderValue::from_static("close"));
        }
        if head {
            *result.body_mut() = full(Bytes::new());
        }
        Ok(result)
    }
    async fn route(&self, request: Request<Incoming>, address: IpAddr) -> Result<Response<Body>> {
        let path = match percent_decode_str(request.uri().path()).decode_utf8() {
            Ok(s) => s.into_owned(),
            Err(_) => return Ok(response(StatusCode::BAD_REQUEST, "文件名必须为 UTF-8")),
        };
        if path == "/v1/files" {
            if request.method() != Method::GET && request.method() != Method::HEAD {
                return Ok(methods("GET, HEAD"));
            }
            let mut files = vec![];
            for entry in std::fs::read_dir(&self.cfg.files)? {
                let entry = entry?;
                let name = entry.file_name().to_string_lossy().into_owned();
                if !name.starts_with('.') && entry.file_type()?.is_file() {
                    files.push(RemoteFile {
                        name,
                        size: entry.metadata()?.len(),
                    });
                }
            }
            files.sort_by(|a, b| a.name.cmp(&b.name));
            return Ok(json(StatusCode::OK, serde_json::json!({"files":files})));
        }
        let Some(name) = path.strip_prefix("/v1/files/") else {
            return Ok(response(StatusCode::NOT_FOUND, "文件不存在"));
        };
        if name.is_empty()
            || name.starts_with('.')
            || name.contains(['/', '\0'])
            || name.len() > 255
        {
            return Ok(response(
                StatusCode::BAD_REQUEST,
                "文件名必须是共享目录内的普通文件名",
            ));
        }
        match *request.method() {
            Method::GET | Method::HEAD => self.get(&request, name).await,
            Method::PUT => self.put(request, name, address).await,
            _ => Ok(methods("GET, HEAD, PUT")),
        }
    }
    async fn get(&self, request: &Request<Incoming>, name: &str) -> Result<Response<Body>> {
        let file = match std::fs::OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
            .open(Path::new(&self.cfg.files).join(name))
        {
            Ok(file) => file,
            Err(_) => return Ok(response(StatusCode::NOT_FOUND, "文件不存在")),
        };
        let metadata = file.metadata()?;
        if !metadata.is_file() {
            return Ok(response(StatusCode::NOT_FOUND, "文件不存在"));
        }
        let size = metadata.len();
        let modified = metadata.modified()?;
        let modified_seconds = second(modified);
        let last_modified = httpdate::fmt_http_date(modified);
        let headers = request.headers();
        let condition = if let Some(value) = text(headers, "if-match") {
            (value.trim() != "*").then_some(StatusCode::PRECONDITION_FAILED)
        } else {
            date(headers, "if-unmodified-since")
                .filter(|date| modified_seconds > *date)
                .map(|_| StatusCode::PRECONDITION_FAILED)
        };
        let condition = condition.or_else(|| {
            if let Some(value) = text(headers, "if-none-match") {
                (value.trim() == "*").then_some(StatusCode::NOT_MODIFIED)
            } else {
                date(headers, "if-modified-since")
                    .filter(|date| modified_seconds <= *date)
                    .map(|_| StatusCode::NOT_MODIFIED)
            }
        });
        if let Some(status) = condition {
            let mut result = response(status, Bytes::new());
            result
                .headers_mut()
                .insert(header::LAST_MODIFIED, last_modified.parse()?);
            if status == StatusCode::NOT_MODIFIED {
                result.headers_mut().remove(header::CONTENT_LENGTH);
            }
            return Ok(result);
        }
        let mut ranges = vec![(0, size)];
        let mut partial = false;
        if request.method() == Method::GET
            && let Some(range) = text(headers, "range")
        {
            let use_range = !headers.contains_key("if-range")
                || date(headers, "if-range") == Some(modified_seconds);
            if use_range {
                match parse_ranges(range, size) {
                    Ok(parsed) if parsed.iter().map(|(_, len)| *len).sum::<u64>() <= size => {
                        ranges = parsed;
                        partial = true;
                    }
                    Ok(_) => {}
                    Err(()) => {
                        let mut result =
                            response(StatusCode::RANGE_NOT_SATISFIABLE, "无效的文件范围");
                        result
                            .headers_mut()
                            .insert(header::CONTENT_RANGE, format!("bytes */{size}").parse()?);
                        return Ok(result);
                    }
                }
            }
        }
        let boundary = if ranges.len() > 1 {
            format!(
                "aware-{:016x}",
                u64::from_ne_bytes(aware_common::unix::random()?)
            )
        } else {
            String::new()
        };
        let mut pieces = vec![];
        let mut total = 0u64;
        for (offset, length) in ranges.iter().copied() {
            let prefix = if !boundary.is_empty() {
                format!(
                    "--{boundary}\r\nContent-Type: application/octet-stream\r\nContent-Range: bytes {offset}-{}/{size}\r\n\r\n",
                    offset + length - 1
                )
            } else {
                String::new()
            };
            total += prefix.len() as u64 + length + if boundary.is_empty() { 0 } else { 2 };
            pieces.push((prefix, offset, length));
        }
        let ending = if boundary.is_empty() {
            String::new()
        } else {
            format!("--{boundary}--\r\n")
        };
        total += ending.len() as u64;
        let head = request.method() == Method::HEAD;
        let name_log = name.to_string();
        let expected = ranges.iter().map(|(_, length)| *length).sum();
        let multipart = !boundary.is_empty();
        let body = if head {
            full(Bytes::new())
        } else {
            let download_log = DownloadLog {
                name: name_log,
                queued: 0,
                expected,
                started: Instant::now(),
            };
            let stream = async_stream::try_stream! {
                let mut log = download_log;
                let mut file = tokio::fs::File::from_std(file);
                for (prefix, offset, mut length) in pieces {
                    if !prefix.is_empty() { yield Frame::data(Bytes::from(prefix)); }
                    file.seek(io::SeekFrom::Start(offset)).await?;
                    while length > 0 {
                        let mut block = vec![0; length.min(TRANSFER_BLOCK as u64) as usize];
                        file.read_exact(&mut block).await?; length -= block.len() as u64; log.queued += block.len() as u64;
                        yield Frame::data(Bytes::from(block));
                    }
                    if multipart { yield Frame::data(Bytes::from_static(b"\r\n")); }
                }
                if !ending.is_empty() { yield Frame::data(Bytes::from(ending)); }
            };
            StreamBody::new(stream).boxed_unsync()
        };
        let mut result = Response::new(body);
        *result.status_mut() = if partial {
            StatusCode::PARTIAL_CONTENT
        } else {
            StatusCode::OK
        };
        let headers = result.headers_mut();
        headers.insert(header::CONTENT_LENGTH, total.into());
        headers.insert(
            header::CONTENT_TYPE,
            if multipart {
                format!("multipart/byteranges; boundary={boundary}").parse()?
            } else {
                HeaderValue::from_static("application/octet-stream")
            },
        );
        headers.insert(header::LAST_MODIFIED, last_modified.parse()?);
        headers.insert(header::ACCEPT_RANGES, HeaderValue::from_static("bytes"));
        if partial && ranges.len() == 1 {
            let (start, len) = ranges[0];
            headers.insert(
                header::CONTENT_RANGE,
                format!("bytes {start}-{}/{size}", start + len - 1).parse()?,
            );
        }
        Ok(result)
    }
    async fn put(
        &self,
        mut request: Request<Incoming>,
        name: &str,
        address: IpAddr,
    ) -> Result<Response<Body>> {
        let headers = request.headers();
        let length = text(headers, "content-length")
            .and_then(|s| s.parse::<u64>().ok())
            .filter(|n| *n <= i64::MAX as u64);
        if headers.contains_key(header::TRANSFER_ENCODING) || length.is_none() {
            return Ok(response(
                StatusCode::LENGTH_REQUIRED,
                "上传需要 Content-Length",
            ));
        }
        let length = length.unwrap();
        if headers.contains_key(header::CONTENT_RANGE)
            || text(headers, "if-none-match").is_some_and(|s| s != "*")
        {
            return Ok(response(
                StatusCode::BAD_REQUEST,
                "上传仅支持完整文件及 If-None-Match: *",
            ));
        }
        let create_only = text(headers, "if-none-match") == Some("*");
        if !self.uploads.lock().unwrap().insert(name.to_owned()) {
            return Ok(response(StatusCode::CONFLICT, "目标文件正在上传"));
        }
        let _guard = UploadGuard {
            name: name.to_owned(),
            uploads: &self.uploads,
        };
        let destination = Path::new(&self.cfg.files).join(name);
        let mut status = StatusCode::CREATED;
        match std::fs::symlink_metadata(&destination) {
            Ok(_) if create_only => {
                return Ok(response(StatusCode::PRECONDITION_FAILED, "目标文件已存在"));
            }
            Ok(meta) if !meta.is_file() => {
                return Ok(response(StatusCode::CONFLICT, "目标不是普通文件"));
            }
            Ok(_) => status = StatusCode::OK,
            Err(e) if e.kind() == io::ErrorKind::NotFound => {}
            Err(e) => return Err(e.into()),
        }
        if available(&self.directory)? < length.saturating_add(16 * 1024 * 1024) {
            return Ok(response(
                StatusCode::INSUFFICIENT_STORAGE,
                "共享目录可用空间不足",
            ));
        }
        let temporary = tempfile::Builder::new()
            .prefix(".upload-")
            .tempfile_in(&self.cfg.files)?;
        if let Err(e) = allocate(temporary.as_file(), length) {
            if e.raw_os_error() == Some(libc::ENOSPC) {
                return Ok(response(
                    StatusCode::INSUFFICIENT_STORAGE,
                    "共享目录可用空间不足",
                ));
            }
            return Err(e.into());
        }
        let mut file = tokio::fs::File::from_std(temporary.reopen()?);
        let mut remaining = length;
        let start = Instant::now();
        // Hyper 在首次轮询请求正文时发送 100 Continue；此时校验和预分配已完成。
        while let Some(frame) =
            tokio::time::timeout(self.cfg.timeout(), request.body_mut().frame()).await?
        {
            let frame = frame?;
            if let Ok(data) = frame.into_data() {
                if data.len() as u64 > remaining {
                    return Ok(response(StatusCode::BAD_REQUEST, "上传长度不符"));
                }
                file.write_all(&data).await?;
                remaining -= data.len() as u64;
            }
        }
        if remaining != 0 {
            return Ok(response(StatusCode::BAD_REQUEST, "上传未完整接收"));
        }
        file.flush().await?;
        drop(file);
        if !self.publisher.sessions.authorized(address) {
            return Ok(response(StatusCode::FORBIDDEN, "上传会话已结束"));
        }
        let publish = if create_only {
            temporary.persist_noclobber(&destination)
        } else {
            temporary.persist(&destination)
        };
        match publish {
            Ok(file) => drop(file),
            Err(e) if create_only && e.error.kind() == io::ErrorKind::AlreadyExists => {
                return Ok(response(StatusCode::PRECONDITION_FAILED, "目标文件已存在"));
            }
            Err(e) => return Err(e.error.into()),
        }
        let mut result = json(
            status,
            RemoteFile {
                name: name.into(),
                size: length,
            },
        );
        result.headers_mut().insert(
            header::LOCATION,
            format!("/v1/files/{}", utf8_percent_encode(name, NON_ALPHANUMERIC)).parse()?,
        );
        eprintln!(
            "HTTP 文件 method=PUT name={name:?} status={} bytes={length} seconds={:.3}",
            status.as_u16(),
            start.elapsed().as_secs_f64()
        );
        Ok(result)
    }
}
fn methods(allow: &'static str) -> Response<Body> {
    let mut result = response(StatusCode::METHOD_NOT_ALLOWED, "不支持的方法");
    result
        .headers_mut()
        .insert(header::ALLOW, HeaderValue::from_static(allow));
    result
}
fn parse_ranges(text: &str, size: u64) -> Result<Vec<(u64, u64)>, ()> {
    let text = text.strip_prefix("bytes=").ok_or(())?;
    let mut ranges = vec![];
    for range in text.split(',') {
        let (start, end) = range.trim().split_once('-').ok_or(())?;
        if start.is_empty() {
            let suffix: u64 = end.parse().map_err(|_| ())?;
            if suffix > 0 && size > 0 {
                let len = suffix.min(size);
                ranges.push((size - len, len));
            }
        } else {
            let start: u64 = start.parse().map_err(|_| ())?;
            let end: u64 = if end.is_empty() {
                size.saturating_sub(1)
            } else {
                end.parse().map_err(|_| ())?
            };
            if end < start {
                return Err(());
            }
            if start < size {
                ranges.push((start, end.min(size - 1) - start + 1));
            }
        }
    }
    if ranges.is_empty() {
        Err(())
    } else {
        Ok(ranges)
    }
}
fn available(file: &File) -> io::Result<u64> {
    let mut stat = std::mem::MaybeUninit::<libc::statvfs>::uninit();
    // stat 只有在 fstatvfs 成功填充之后读取。
    if unsafe { libc::fstatvfs(file.as_raw_fd(), stat.as_mut_ptr()) } != 0 {
        return Err(io::Error::last_os_error());
    }
    let stat = unsafe { stat.assume_init() };
    Ok(stat.f_bavail as u64 * stat.f_frsize as u64)
}
#[cfg(target_os = "linux")]
fn allocate(file: &File, length: u64) -> io::Result<()> {
    if length > 0
        && unsafe {
            libc::fallocate(
                file.as_raw_fd(),
                libc::FALLOC_FL_KEEP_SIZE,
                0,
                length as i64,
            )
        } != 0
    {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}
#[cfg(not(target_os = "linux"))]
fn allocate(_file: &File, _length: u64) -> io::Result<()> {
    Err(io::Error::new(
        io::ErrorKind::Unsupported,
        "文件服务运行需要 Linux",
    ))
}

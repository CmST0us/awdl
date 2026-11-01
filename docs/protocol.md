# HTTP 文件协议

`fileserver` 在 `ndi0` 上提供 HTTP/1.1，使用 IPv6/TCP，默认端口 7777。服务名 `_fileserver._tcp` 和端口通过 Wi-Fi Aware 发布；连接和每个请求均受本服务安全 NDP 的 IPv6 会话授权约束。文件字节直接经过 TCP，D-Bus 只管理注册与会话。

API 路径使用 `/v1`。文件名是共享目录中的普通文件名，以 UTF-8 编码并按 URL 路径段转义；不接受子目录、隐藏文件和符号链接。文件正文使用原始字节；列表与上传完成回执使用 JSON。iPad 保留 Network.framework 的 Wi-Fi Aware 路径校验，通过该连接发送 HTTP/1.1。每次目录读取、下载或上传使用新的 TCP 连接，操作完成、取消或失败时关闭；PUT 以外的请求保持 HTTP/1.1 持久连接，多连接下载在同一连接上连续请求多个范围。

## 列表与元数据

```http
GET /v1/files HTTP/1.1
Host: fileserver

HTTP/1.1 200 OK
Content-Type: application/json
Content-Length: …

{"files":[{"name":"sample.bin","size":1073741824}]}
```

`HEAD /v1/files/{name}` 返回文件的 Content-Length、Content-Type、Last-Modified 和 Accept-Ranges，不发送正文。列表只包含普通文件，按名称排序。

## 下载

`GET /v1/files/{name}` 返回 `200 OK` 和完整文件。范围下载使用标准 Range：

```http
GET /v1/files/sample.bin HTTP/1.1
Host: fileserver
Range: bytes=0-268435455

HTTP/1.1 206 Partial Content
Content-Type: application/octet-stream
Content-Length: 268435456
Content-Range: bytes 0-268435455/1073741824
Accept-Ranges: bytes

<268435456 字节文件内容>
```

Rust 服务端支持单范围和 multipart/byteranges 多范围下载、HEAD，以及 If-Modified-Since、If-Unmodified-Since 和 If-Range 修改时间条件。超出文件长度的范围返回 `416` 和 `Content-Range: bytes */总长度`，不存在的文件返回 `404`。

iPad 验证 `206`、Content-Length 和 Content-Range。单连接以一个请求读取整个文件；4 路自适应把文件划分为 1–4 MiB 范围的共享队列，各连接完成一个范围后在同一 TCP 连接上请求下一个，并写入同一临时文件的相应位置。多连接下载的每个分片须与本次最先返回的分片具有相同的 Last-Modified，否则以“远端文件在下载期间已更改”失败；单连接下载在一次请求内读取同一文件，期间同名文件被替换不影响本次内容。全部完成后发布为正式文件，本地同名文件被完整替换。空文件使用普通 GET。取消或任一分片失败时关闭本次所有连接、删除临时文件，已有文件保持不变；下次下载重新开始。

## 上传

```http
PUT /v1/files/upload.bin HTTP/1.1
Host: fileserver
Content-Type: application/octet-stream
Content-Length: 1073741824
Expect: 100-continue

HTTP/1.1 100 Continue

<1073741824 字节文件内容>

HTTP/1.1 201 Created
Location: /v1/files/upload.bin
Content-Type: application/json
Content-Length: …

{"name":"upload.bin","size":1073741824}
```

上传必须携带 Content-Length。服务端检查名称、同名上传和可用空间，并按 Content-Length 为临时文件预分配空间后，读取正文时发送 `100 Continue`。该机制可在发送大文件前得知拒绝结果；同时进行的上传各自占用预分配空间，剩余空间不足的请求直接返回 `507`。零字节上传省略 Expect，直接等待完成回执。

完整接收后关闭临时文件，以原子改名发布并替换同名文件；创建新文件返回 `201`，替换已有文件返回 `200`，两者均携带名称和长度回执，符合 [HTTP PUT 语义](https://www.rfc-editor.org/rfc/rfc9110.html#name-put)。客户端核对回执后才报告成功。传输中断时删除临时文件，旧目标保持不变。

显式携带 `If-None-Match: *` 的请求只允许创建新文件，目标已存在时返回 `412`；App 默认不发送该条件。同名上传正在进行或目标不是普通文件时返回 `409`。缺少 Content-Length 或使用分块上传返回 `411`，空间不足返回 `507`。上传不接受 Content-Range。PUT 响应关闭连接。

## 生命周期

每个请求读取当前 D-Bus Sessions；失去本服务最后一条 NDP 的地址，其所有 TCP 连接被关闭。注册对象或无线守护进程退出时文件服务结束，由 systemd 按依赖关系重新启动。配对窗口与服务发现的结束不会中断已有文件传输。

默认空闲读写上限为 60 秒，持续收发的大文件没有 60 秒总耗时限制。文件服务使用已有 NDP 的链路保护，不另行配置 HTTP 账号或 TLS 证书。

## 已知限制

- 多服务共用 NDI 与 IPv6 地址，地址授权不能区分同一对端不同 NDP 上的 IP 数据包。
- 上传完成表示文件已完整写入并发布，不包含断电持久化保证；客户端取消后不保留断点。
- Last-Modified 精确到秒，多连接下载不能识别同一秒内换入的等长文件；下载期间就地改写共享文件不受支持。

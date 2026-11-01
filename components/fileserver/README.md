# Wi-Fi Aware HTTP 文件服务

独立 Rust 上层服务 `fileserver`，通过 zbus 注册 TCP 服务并订阅自身 NDP 会话。HTTP/1.1 使用 Hyper，异步 I/O 使用 Tokio。

- `configuration.rs`：启动参数、TOML 配置与最终配置校验。
- `publisher.rs`：能力查询、注册、会话刷新与失效通知。
- `files.rs`：共享目录运行锁、HTTP 文件列表、HEAD、Range 下载和 PUT 上传。
- `io.rs`：连接读写空闲期限。
- `main.rs`：NDI 监听、连接任务和进程生命周期。

文件服务在 `ndi0` 上监听 IPv6/TCP。每个 HTTP 请求读取当前会话集合；对端最后一条本服务 NDP 消失时关闭该地址的连接。服务对象或无线守护进程退出时关闭监听与活动连接，上传临时文件随处理程序退出而清理。

下载支持单范围、多范围及基于修改时间的条件请求，按 256 KiB 块写入。上传要求 Content-Length，支持 `Expect: 100-continue`；读取正文前按 Content-Length 为临时文件预分配空间，空间不足返回 `507`。完整接收后原子改名并替换同名文件，创建返回 `201`，替换返回 `200`。显式携带 `If-None-Match: *` 时仅允许创建新文件。中断上传保留旧文件，读写按块刷新空闲期限。

## 构建与安装

使用 Rust 1.93 或更新版本，依赖版本固定于工作区 `Cargo.lock`。

```sh
scripts/build-fileserver.sh
sudo scripts/install-fileserver.sh
sudo systemctl start fileserver
```

从其他架构交叉编译时，需要预先安装 Rust 目标及对应 C linker：

```sh
CARGO_BUILD_TARGET=aarch64-unknown-linux-gnu \
CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=aarch64-linux-gnu-gcc \
  scripts/build-fileserver.sh
```

生成可执行文件 `.build/targets/<target>/fileserver/fileserver` 和对应授权文件，默认 GNU 目标运行时需要 Linux、glibc 与 system D-Bus。配置为 `/etc/fileserver/config.toml`，安装保留已有配置。默认服务名 `_rtl8852be-test._tcp`、端口 7777、共享目录 `/srv/fileserver`；共享目录可由 `files` 或 `--files` 指定，已有安装保留其共享目录。

启动参数支持 `--service`、`--files`、`--port`、`--io-timeout`，显式参数覆盖 TOML 中的同名配置；未指定的字段沿用配置文件或默认值。`--config PATH` 选择配置，`--config=` 仅使用命令行和默认值。默认配置文件不存在时可直接通过参数启动；显式指定的配置文件必须存在。

```sh
sudo fileserver --config= \
  --service _files._tcp --files /srv/files --port 7788 --io-timeout 90
```

参数表、systemd 覆盖示例及客户端服务名要求见[运行文档](../../docs/usage.md#文件服务)。

依赖以 Hyper、Tokio、zbus、serde 和 toml 为主；完整版本、来源与授权表达式随构建产物写入 `licenses/dependencies.json`，依赖授权原文和 Rust 标准库版权一并安装。构建与交叉编译方法见[构建文档](../../docs/build.md)。

协议见[文件协议](../../docs/protocol.md)，本机服务接口见[上层服务接口](../../docs/service-api.md)。

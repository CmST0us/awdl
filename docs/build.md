# 构建与安装

在仓库根目录运行脚本。公共服务使用 Rust 1.93 或更新版本，Cargo.lock 固定依赖；Linux 用户态需要 C 工具链、make、bison、flex、bc、libelf 开发库、pkg-config、libnl 3、libnl-genl 3 和 OpenSSL 开发库。源码准备需要 curl、tar、patch；RTL 驱动还需要 Python 3.12、带 MIPS16 后端的 Clang 21、GNU MIPS little-endian binutils、目标内核 headers 和 Linux 目标工具链。

## 构建选择

`config/build.toml` 提供公共构建默认值，`platforms/rock5b/build.toml` 提供 ARM64 / ROCKCHIP 选择。复制 `config/examples/build.toml` 到 `config/local/build.toml`，按构建机填写路径和工具链。

安装示例按平台和网卡命名：`config/examples/install-rock5b-rtl8852be.toml` 或 `config/examples/install-rock5b-be200.toml`。将对应示例复制到 `config/local/install.toml`，按目标机器填写 PCI 与实际地区。本机文件持久保留，不进入 Git。

优先级为显式参数、对应环境变量、本机文件、平台和公共默认值。`CARGO_BUILD_TARGET`、`KERNEL_RELEASE`、`KLIB_BUILD` / `KSRC`、`CC`、`U2_CC`、`CROSS_COMPILE`、`ARCH`、`JOBS`、`DESTDIR` 分别映射到构建目标；`CARGO` / `RUSTC` 选择执行工具。使用 `scripts/aware-build help` 查看参数。

在 Linux 目标架构原生构建公共用户态：

```sh
scripts/build-hostap.sh
scripts/build-publisher.sh
scripts/build-fileserver.sh
```

交叉构建时设置 `--target aarch64-unknown-linux-gnu` 或 `--target x86_64-unknown-linux-gnu`。Rust 需要对应标准库与 linker；hostap 需要 `--cc` 和目标架构的 pkg-config 库搜索路径。`--archive /path/to/wpa_supplicant-2.12.tar.gz` 可复用固定版本的本地归档。两种无线后端编入同一份 hostap 与 wifiawared。

驱动显式选择硬件、目标内核和平台：

```sh
scripts/build-driver.sh be200 --platform rock5b \
  --kernel-release <目标内核版本> --headers /opt/kernel-headers/rock5b \
  --cross-compile aarch64-linux-gnu-
scripts/build-driver.sh rtl8852be --platform rock5b \
  --kernel-release <目标内核版本> --headers /opt/kernel-headers/rock5b \
  --cross-compile aarch64-linux-gnu- --mips-cc clang
```

BE200 准备 Ubuntu backport 24.70-14867 及其打包补丁后应用项目补丁。RTL 准备固定 Radxa 源码、应用 NAN 补丁、从原始 U2 重建固件、替换数组并核对最终模块中的镜像及下载长度。RTL 镜像重建使用 Clang 21；编译器选择独立于 Linux 目标架构。

产物位于 `.build/targets/<Rust target>/`：公共服务按组件分目录，hostap 展开源码及 RTL 静态库属于同一目标，驱动按 `drivers/<hardware>/<kernel>/` 隔离。同一目标切换 C 编译器或 archiver 时，hostap 和 RTL 静态库重新构建。安装工具按运行主机隔离在 `.build/tools/<host>/aware-build`。Rust 依赖和标准库授权随产物交付。

## 安装

在目标机器安装，或传 `--destdir /absolute/staging/root` 暂存。暂存安装不调用 systemctl、不改变主机驱动。驱动安装要求 `modinfo`，检查目标内核版本；目标启用 Secure Boot 时保留签名检查。

```sh
sudo scripts/install-driver.sh rtl8852be --kernel-release <目标内核版本> --country CN
sudo scripts/install-publisher.sh --pci 0002:21:00.0
sudo scripts/install-fileserver.sh
sudo scripts/aware-build install-platform --platform rock5b
sudo systemctl enable --now wifiawared fileserver
```

`CN` 只代表示例部署地区。RTL 必须明确国家码；BE200 的 `--cn` / `[be200].platform_cn` 是独立的平台提示。ROCK 5B 的 usbmoded 关机配置由 `install-platform` 单独安装。

公共用户态升级时，先停止 `fileserver` 和 `wifiawared`，安装同一版本的程序、systemd 单元及 D-Bus 策略，再启动服务。公共总线名称、接口和错误命名空间为 `org.wifiawared1`，本机 D-Bus 调用方须使用同一契约。

首次安装将带注释的默认配置写入 `/etc`，已有配置保持不变。无线配置里的既有 PCI 优先于安装文件；显式 `--pci` 同时更新运行配置、udev 设备归属及 systemd 设备依赖。物理接口按 PCI 匹配，运行时从 PHY 取得实际名称。已运行的系统 wpa_supplicant 须释放该设备，其他网卡不受影响。

旧文件服务配置按字段保留到 `/etc/fileserver/config.toml`，原共享目录和 I/O 期限继续生效。安装接管旧单元时停止并禁用旧服务，依据其原有启用与运行状态启动 `fileserver.service`。配对身份与共享数据不移动。

## iOS

使用支持 WiFiAware 的 Xcode 和 iOS SDK。`scripts/build-ios.sh` 或直接构建 Xcode scheme 都会从 `config/fileserver-discovery.toml` 生成 Info.plist。构建期需要 Rust 工具链；`Client.plist` 随应用交付，控制连接与 I/O 空闲期限。

| Scheme | Bundle ID |
| --- | --- |
| AwareClient | com.eric3u.rtl8852be.AwareClient |
| AwareClientBE200 | com.eric3u.rtl8852be.AwareLab |
| AwareLab（诊断项目） | com.eric3u.rtl8852be.AwareLabRTL |

```sh
scripts/build-ios.sh AwareClient
scripts/build-ios.sh AwareClientBE200
```

脚本生成未签名 Release 包。真机安装使用对应 scheme 和既有开发团队签名，保留原 bundle 身份。诊断 App 及设备验收入口位于 `ios/AwareLab` 和 `ios/scripts`。

## 内核要求

BE200 backport 替换 cfg80211/mac80211，目标运行内核须将它们配置为模块。ROCK 5B 的板型相同不代表内核配置相同；两块设备的 kernel_release 与 headers 分别取自其实际部署内核。安装器检查 vermagic；它不能证明 headers 的配置与运行内核完全一致。

现有 BE200 板的运行内核两项均为 m，SDK headers 却标为 y，原 SDK 的 modpost 将重复导出降为告警。新部署应提供配置与 Module.symvers 均匹配的 headers；组件不改写内核构建工具。

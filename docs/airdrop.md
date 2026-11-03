# AirDrop 与 AWDL

BE200 与 RTL8852BE 通过原生 `awdl0` 接入公共 AWDL 控制栈。`wirelessd` 管理硬件所有权和 AWDL 会话；`airdrop` 管理 BLE 唤醒、发现、用户接受与传输。数据直接经过内核 IPv6。

## 构建与安装

```sh
scripts/aware-build prepare-source hostap --platform rock5b
scripts/build-wirelessd.sh --platform rock5b
scripts/build-awdl.sh --platform rock5b
scripts/build-airdrop.sh --platform rock5b
scripts/build-driver.sh rtl8852be --platform rock5b

sudo scripts/install-wirelessd.sh
sudo scripts/install-awdl.sh --hardware rtl8852be
sudo scripts/install-airdrop.sh --hardware rtl8852be
sudo scripts/install-driver.sh rtl8852be --country CN
```

BE200 使用相同公共用户态产物，驱动构建、驱动安装与两项应用安装的硬件参数改为 `be200`，模块地区参数按部署选择。AWDL 构建同时生成两个硬件后端，安装器仅安装所选后端。

交叉构建仍使用公共构建配置的编译器、目标内核及 headers。安装接受 `--destdir`；已有运行配置保留。模块替换需要先停止所有无线服务，保存当前模块、参数和运行状态，再通过正常卸载加载完成。生产服务不自行替换驱动。

## 运行模式

| 硬件 | 无线提供者 | AirDrop 依赖 | 与 NAN 的关系 |
| --- | --- | --- | --- |
| BE200 | `wirelessd.service` | 安装器生成对该服务的依赖 | 共用 NAN 管理接口，另建 NAN_DATA `awdl0`，NAN/fileserver 可保持运行 |
| RTL8852BE | `wirelessd-awdl.service` | 安装器生成对该服务的依赖 | 当前驱动需先释放 NAN 占用 |

BE200 启动：

```sh
sudo systemctl start wirelessd fileserver airdrop
```

RTL 启动：

```sh
# 当前 RTL 后端测试前，正常停止占用同一网卡的服务。
sudo systemctl stop fileserver wirelessd
# 启动 AWDL 管理与 AirDrop 应用。
sudo systemctl start airdrop
```

应用操作：

```sh
# 使用接收设备在系统中显示的准确名称。
airdrop send 'My iPad' /absolute/path/file

# 常驻接收；命令返回操作 ID，进程由 airdrop.service 持有。
airdrop receive
airdrop status
airdrop monitor

# 从 request 事件或 status.offers 取得请求 ID。
airdrop accept 1
airdrop decline 1
airdrop stop

# RTL 结束 AirDrop/AWDL 后恢复 NAN 文件服务。
sudo systemctl stop airdrop wirelessd-awdl
sudo systemctl start fileserver
```

iPad 使用系统 AirDrop 的“所有人（10 分钟）”模式，发送前开启并在系统提示中接受。BLE 需要专用、原本关闭的 Linux HCI 适配器；操作结束恢复关闭状态，已有蓝牙会话不会被接管。`airdrop stop` 取消活动操作并等待连接、BLE 和原生 AWDL 释放。

真机自动验收使用 [AirDrop UI 测试](../ios/README.md#airdrop-自动接收验收)：由 XCUITest 开启“所有人”并自动接受指定文件，Mac 入口在 UI 就绪后触发板端发送，分别记录点击接受与协议交付结果。

BE200 在现有守护进程中注册两个协议接口，AWDL 释放不关闭 NAN。RTL 的两个运行单元共用 PCI 设备锁及 `org.wirelessd1`，同一网卡已被占用时启动报告忙。服务启动不自动停止其他无线服务。配置、配对和共享文件保留，默认开机启用设置保持原部署选择。

## 接口与进度

| 接口 | 方法及含义 |
| --- | --- |
| `org.wirelessd1.Awdl`，`/org/wirelessd1` | `Acquire(target)` 申请会话；空目标仅发现，`*` 选择首个可用对端；`SelectPeer(mac)` 选择一个已发现对端；`Release()` 等待原生 STOP |
| `org.airdrop1.Manager`，`/org/airdrop1` | `Send(name,path)`、`Receive()` 返回操作 ID；`Accept(id)`、`Decline(id)` 回答接收请求；`Stop()` 请求取消 |

两个接口的 `Status` 属性和 `Event` 信号使用 JSON 描述状态。AWDL 所有权绑定 D-Bus 调用方；拥有者断开会触发回收。AirDrop 操作由常驻服务持有，发送 CLI 退出不取消传输，取消使用 `Stop`。

AirDrop 进度包括操作 ID、已发送压缩正文与总正文大小；`delivered` 表示收到 Upload 成功响应。`file_bytes` 是原文件长度，`compressed_bytes` 是压缩归档正文长度。`upload_seconds` 从用户接受到 Upload 成功响应，两个 `MBps` 字段分别按原文件和压缩正文计算，单位为十进制 MB/s。

`request`、`withdrawn`、`received` 和 `receive-failed` 关联同一个接收请求 ID。默认只在明确接受后落盘；自动接受策略从应用配置读取。回收失败保留错误及资源所有权，处理现场后可用 `airdrop stop` 再次请求清理。

## 限制

- 当前 RTL 驱动的 NAN 与 AWDL 使用同一组运行资源，活动 NAN 会使 AWDL 启动返回忙；RTL 的驱动资源共享尚未实现。
- BE200 AWDL 仅支持一个数据对端，固定主信道 149、VHT80；要求 NAN 数据频率 5745 MHz，不跟随 161。系统 AirDrop 与活动 NDP 并发传输尚未完成验收。
- RTL AWDL 仅支持一个数据对端，在主信道 149 启动并跟随同一 80 MHz 块内的 149/161；不支持跨块切换或多对端并发。
- AWDL 状态机在主机运行，内核 hrtimer 执行窗口门控；现有 c108 与 U2 固件不承担自主 AWDL/TSF 调度。
- 发送按 TLS 返回的系统设备名匹配；名称不是密码学设备身份。支持系统 Everyone 模式，未实现 Contacts Only。
- Upload 成功响应证明协议交付完成，接收端逐字节回传和长期稳定性需要独立验收。当前验证结果见 [验收记录](acceptance.md)。

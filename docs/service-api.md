# 上层服务接口

`wirelessd` 使用 system D-Bus，无线守护进程和 fileserver 均使用 Rust zbus。总线名称为 `org.wifiawared1`，管理对象为 `/org/wifiawared1`。接口名称中的 `1` 表示协议主版本。类型与方法通过标准 Introspectable 接口查询，实际能力通过 Manager 属性查询。

能力属性面向本机上层应用；空口服务发现和 NDP 参数仍由 hostap 按 Wi-Fi Aware 协议通告。管理协议不承载文件数据。上层应用先在 `ndi0` 建立监听，再注册服务；TCP 或 UDP 数据直接进入上层进程。

## 管理接口

接口：`org.wifiawared1.Manager`。

| 方法 | 输入 | 返回 | 行为 |
| --- | --- | --- | --- |
| `RegisterService` | `ssq`：名称、传输协议、端口 | `o`：服务对象路径 | 发布服务，将端口与传输协议写入对应 NDP 响应 |
| `OpenPairing` | `u`：30–600 秒 | `s`：六位 PIN | 开启配对窗口；需要至少一个服务，不能与进行中的认证重叠 |
| `ClosePairing` | 无 | 无 | 关闭配对窗口，保留服务和现有会话 |
| `GetRadioStatus` | 无 | `s`：hostap NAN 状态 | 读取当前无线状态 |

| 只读属性 | 类型 | 含义 / 当前值 |
| --- | --- | --- |
| `SupportedRoles` | `as` | `['publisher']` |
| `SupportedTransports` | `as` | `['tcp', 'udp']` |
| `SupportsMultipleServices` | `b` | `true` |
| `CanInitiateDataPath` | `b` | `false`；由对端发起 NDP |
| `Interface` | `s` | `ndi0` |
| `LocalAddress` | `s` | 本机 NDI 的 IPv6 link-local 地址 |
| `ConfiguredFrequency` | `u` | 配置的数据频率，单位 MHz；不代表当前协商带宽 |
| `Services` | `ao` | 当前服务对象路径集合 |
| `PairingActive` | `b` | 配对窗口是否开启 |
| `PairingEndsAt` | `t` | 板端 CLOCK_MONOTONIC 微秒截止时间，关闭时为 0 |

能力、接口地址和配置频率在本次守护进程生命周期内不变，带 `const` 注解。服务集合、配对状态变化通过标准 `org.freedesktop.DBus.Properties.PropertiesChanged` 通知。客户端在守护进程所有者变化后重新读取能力。

## 服务对象与所有权

每次注册返回独立路径，例如 `/org/wifiawared1/services/s1`，接口为 `org.wifiawared1.Service`。路径在本次守护进程生命周期内不复用；无线重新发布时保持对象路径，更新 `PublishId`。

| 成员 | 类型 | 含义 |
| --- | --- | --- |
| `Unregister()` | 方法，无参数和返回值 | 仅允许拥有该服务的连接调用 |
| `Name` | `s`，只读常量 | 服务名称 |
| `Transport` | `s`，只读常量 | `tcp` 或 `udp` |
| `Port` | `q`，只读常量 | 1–65535 |
| `Owner` | `s`，只读常量 | 注册者的 D-Bus 唯一连接名称 |
| `PublishId` | `s`，只读，可变 | 当前 hostap 发布实例 |
| `Sessions` | `a(ssss)`，只读，可变 | 本服务已建立的 NDP 集合 |

服务名最多 127 个 ASCII 字符，只含字母、数字、下划线、连字符和点，TCP 服务以 `._tcp` 结尾，UDP 服务以 `._udp` 结尾，后缀必须与传入的 `transport` 一致；名称比较不区分大小写。重复名称返回 `org.wifiawared1.Error.AlreadyRegistered`；不支持的传输协议返回标准 `org.freedesktop.DBus.Error.NotSupported`。无效输入返回 `InvalidArgs`，越权注销返回 `AccessDenied`。`OpenPairing` 在没有已注册服务时返回 `org.wifiawared1.Error.NotReady`；已有对端正在配对或验证时返回 `org.wifiawared1.Error.Busy`，单次认证最长 30 秒，结束后可重新调用。无线操作失败返回 `org.wifiawared1.Error.Radio`。

**一个连接可以注册多个服务。** 所有权由总线分配的唯一名称确定，不接受客户端传入的拥有者标识。单个 `Unregister` 只回收该服务；连接退出产生 `NameOwnerChanged`，回收它拥有的全部服务。回收包括撤销公告、取消该服务的认证调度、终止所属 NDP；其他服务继续运行。共享 IPv6 邻居条目在最后一个使用它的 NDP 移除后删除。

默认 system bus 策略允许 root 拥有总线名称、注册服务及管理配对；本机其他用户可读取属性、枚举和内省。项目的两个 systemd 服务以 root 运行。

## 会话与通知

`Sessions` 每条记录为 `(peer, peer_ndi, id, address)`：

- `peer`：对端 NAN 管理地址。
- `peer_ndi`：发起端数据接口地址。
- `id`：NDP ID；与前两个字段共同标识数据通路。
- `address`：对端在 `ndi0` 上使用的 IPv6 link-local 地址。

会话建立或结束后，所属服务发出 `PropertiesChanged`。上层据此重新读取完整会话集合；某地址在本服务的最后一条 NDP 消失后，关闭该地址的业务连接。FileServer 在处理每个 HTTP 请求时也读取当前集合，已连接的数据收发不经过 D-Bus。

管理对象提供标准 `org.freedesktop.DBus.ObjectManager`：`GetManagedObjects` 一次枚举所有服务及其属性，`InterfacesAdded`、`InterfacesRemoved` 通知服务对象增删。监视客户端先订阅通知，再读取初始快照。FileServer 固定到本次无线守护进程的唯一总线名称；该名称失效或本服务被移除后，关闭监听和活动连接。systemd 随后重启并重新注册。

配对窗口关闭或到期不会删除服务对象，也不会结束已有传输。无线进程在接口与协议栈就绪后取得总线名称，`Type=dbus` 使 FileServer 的启动顺序与此就绪条件一致。

## UDP 数据报

UDP 使用相同的 `RegisterService`、服务对象、NDP 会话及注销规则。上层在 `ndi0` 绑定 IPv6 UDP 套接字后，以 `("_device-control._udp", "udp", 7788)` 注册。NDP 响应的服务信息分别编码端口（16 位小端）和 IP 传输协议号：TCP 为 6，UDP 为 17。对端据此取得该服务的目标端口和传输协议。

UDP 没有 accept 或连接断开事件；上层按照 `Sessions` 中的地址接收和发送数据报。会话移除后停止向对应地址发送并忽略其数据，服务对象或守护进程失效后关闭套接字。消息边界、丢包、乱序和重传由上层业务处理，`wirelessd` 不转发或缓存业务数据。TCP 与 UDP 服务可以同时注册；FileServer 固定使用 TCP。

## 查询与调用

```sh
busctl introspect org.wifiawared1 /org/wifiawared1
busctl get-property org.wifiawared1 /org/wifiawared1 \
  org.wifiawared1.Manager SupportedTransports
busctl call org.wifiawared1 /org/wifiawared1 \
  org.freedesktop.DBus.ObjectManager GetManagedObjects
sudo wirelessd status
sudo wirelessd pair --seconds 120
sudo wirelessd close-pairing
```

`wirelessd status` 使用 D-Bus 查询后在终端输出 JSON：`manager` 是能力与管理属性，`services` 是各服务的属性及对象 `path`，`radio` 是 hostap 状态。JSON 仅为命令行输出格式。

Rust 注册示例（在 Tokio 异步入口内调用）：

```rust
let connection = zbus::Connection::system().await?;
let manager = zbus::Proxy::new(
    &connection,
    "org.wifiawared1",
    "/org/wifiawared1",
    "org.wifiawared1.Manager",
).await?;
let path: zbus::zvariant::OwnedObjectPath = manager
    .call("RegisterService", &("_device-control._tcp", "tcp", 7788u16))
    .await?;
```

调用方在注册前建立数据监听，持有 `connection` 至服务停止，并订阅服务的会话属性变化。可在同一连接再次调用以注册其他服务。单次 `busctl call RegisterService` 返回后进程立即退出，注册也会立即回收，不能用于运行常驻服务。

## 已知限制

当前支持 TCP 和 UDP Publisher 服务；上层主动发起 NDP、按服务申请独立频率或带宽尚未提供。多服务共用无线调度与 `ndi0`，IPv6 地址授权不提供按空口 NDP 区分同一对端 IP 数据包的隔离。能力属性描述实现支持范围，实际空口协商以 NDP、对端时段表和传输证据为准。

接口采用 [D-Bus 标准对象和属性机制](https://dbus.freedesktop.org/doc/dbus-specification.html)，由 zbus 接入 Tokio 运行时。

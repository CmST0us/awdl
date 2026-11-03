# BE200 原生 AWDL 后端

`be200-awdl` 复用已运行的 NAN 管理接口，通过 nl80211 收发 Apple AWDL Action 帧。`awdl0` 是同一 PHY 上的 `NAN_DATA` 接口，使用 NMI 地址；IP 数据由 mac80211 完成 AWDL 封装、解封装并交给内核 IPv6。控制协议来自公共 `components/awdl`。

本后端与 NAN 共用数据主信道：5745 MHz 对应 149，5805 MHz 对应 161，80 MHz 中心频率为 5775 MHz。协商支持 HE80 / VHT80；本地发现日程由 hostap 管理。控制状态机以 `phy_tx - target_tx` 补偿对端同步帧的发送排队时间，使用选定对端的同步时钟计算可用窗口，转换为 CLOCK_MONOTONIC 纳秒窗口，内核 hrtimer 唤醒发送队列，驱动在窗口内取包。`tx_drain_margin_tu` 指定对端切换前的固件排空余量，默认 3 TU；同信道的相邻时隙之间不扣除该余量。

## 资源生命周期

公共管理器调用 `be200-awdl phyN channel target local-name`，`channel` 为 149 或 161，并传入硬件策略中的 TCP 参数。`target` 为空时只发现，`*` 选择第一个可用对端，也可传入 MAC；标准输入的 `SELECT MAC` 选择已发现对端，`STOP` 请求释放。标准输出与公共管理器交换 `started`、`peer`、`netdev-ready`、`stopped` 等 JSON 事件。

创建接口时设置 nl80211 `SOCKET_OWNER`。AWDL 对端的 NMI station 归 `awdl0` 所有，接口停止先回收 NDI station，再释放其 NMI station；控制套接字关闭也触发接口销毁。正常停止等待 `DEL_INTERFACE` 确认。`be200-awdl phyN stop` 可回收同 PHY 上残留的原生 `awdl0`。

公共 `wirelessd.service` 在启动 hostap 后注册 `org.wirelessd1.Awdl`，与 Wi-Fi Aware Manager 共用设备锁和总线名。管理器在启动后端前记录所有权，只有释放完成后才清除；退出先停止 AWDL，再关闭 NAN。

## 驱动接口

有序补丁 `0011` 允许指定 AWDL BSSID 与 Apple Action 帧；`0012` 提供原生数据格式、站点所有权与窗口门控；`0013` 将 AWDL Action 帧送入所属 NAN_DATA 的组播队列，避免 NAN 发现窗口延迟同步帧的实际发送。`0014` 接入 AWDL 快速发送、A-MSDU、TSO 与校验和卸载；每个子帧保留独立 AWDL 序号，IP 偏移按实际封装计算，固件发送队列为 256 项。沿用 c108 / PNVM 固件。

Intel vendor command `0x001735 / 0x4157444c` 以 `awdl0` 的 ifindex 寻址：

| 数据 | 操作 |
| --- | --- |
| `01` | 接口启用前选择 AWDL 数据格式 |
| `02` + 6 字节 MAC | 建立归该接口所有的 NMI 对端；已有对端返回错误 |
| `03` + 两个 LE64 | 安装发送窗口，起止时间为 CLOCK_MONOTONIC 纳秒；全零关闭窗口 |

对端 HT/VHT/HE 能力与可用信道经标准 station 和 NAN peer schedule 命令提交。公共协议中的 AWDL HT TLV 转换为 IEEE HT capability 后交给 mac80211。

## TCP 策略

`hardware/be200/radio.toml` 的 `[awdl]` 定义拥塞控制算法、初始拥塞窗口、乱序阈值、接口发送队列长度和发送排空余量，字段必填。`wirelessd` 校验后通过 `AWARE_AWDL_TCP_CONGESTION`、`AWARE_AWDL_INITIAL_WINDOW`、`AWARE_AWDL_REORDERING`、`AWARE_AWDL_TX_QUEUE`、`AWARE_AWDL_TX_DRAIN_TU` 传给后端；这些进程环境不是第二份配置来源。后端为会话主机路由设置 TCP 策略，并使用 fq pacing。建立新链路时通过通用 Netlink 清除该本地地址与对端地址对应的旧 TCP 指标，使路由参数用于新连接；无缓存时正常继续。最小重传期限为 1100 ms，覆盖一个 1048.576 ms AWDL 周期；路由和队列随接口销毁，不改变系统全局 TCP 默认值。

## 限制

- 仅一个 AWDL 数据对端；使用所选的 149 或 161，不在两者之间跟随切换。
- 固件继续执行 NAN 本地日程；AWDL 发送窗口由主机控制，不是固件自主 AWDL/TSF 调度。
- 实测链路和 AirDrop 验收范围见 [验收记录](../../../docs/acceptance.md)。

# BE200 原生 AWDL 后端

`be200-awdl` 复用已运行的 NAN 管理接口，通过 nl80211 收发 Apple AWDL Action 帧。`awdl0` 是同一 PHY 上的 `NAN_DATA` 接口，使用 NMI 地址；IP 数据由 mac80211 完成 AWDL 封装、解封装并交给内核 IPv6。控制协议来自公共 `components/awdl`。

本后端固定主信道 149、中心信道 155、VHT80。NAN 数据主频率须为 5745 MHz；本地发现日程继续由 hostap 管理。控制状态机计算对端在 149 上的可用窗口，转换为 CLOCK_MONOTONIC 纳秒窗口，内核 hrtimer 唤醒发送队列，驱动在窗口内取包。

## 资源生命周期

命令为 `be200-awdl phyN 149 target local-name`。`target` 为空时只发现，`*` 选择第一个可用对端，也可传入 MAC；标准输入的 `SELECT MAC` 选择已发现对端，`STOP` 请求释放。标准输出与公共管理器交换 `started`、`peer`、`netdev-ready`、`stopped` 等 JSON 事件。

创建接口时设置 nl80211 `SOCKET_OWNER`。AWDL 对端的 NMI station 归 `awdl0` 所有，接口停止先回收 NDI station，再释放其 NMI station；控制套接字关闭也触发接口销毁。正常停止等待 `DEL_INTERFACE` 确认。`be200-awdl phyN stop` 可回收同 PHY 上残留的原生 `awdl0`。

公共 `wirelessd.service` 在启动 hostap 后注册 `org.wirelessd1.Awdl`，与 Wi-Fi Aware Manager 共用设备锁和总线名。管理器在启动后端前记录所有权，只有释放完成后才清除；退出先停止 AWDL，再关闭 NAN。

## 驱动接口

有序补丁 `0011` 允许指定 AWDL BSSID 与 Apple Action 帧；`0012` 提供原生数据格式、站点所有权与窗口门控；`0013` 将 AWDL Action 帧送入所属 NAN_DATA 的组播队列，避免 NAN 发现窗口延迟同步帧的实际发送。沿用 c108 / PNVM 固件。

Intel vendor command `0x001735 / 0x4157444c` 以 `awdl0` 的 ifindex 寻址：

| 数据 | 操作 |
| --- | --- |
| `01` | 接口启用前选择 AWDL 数据格式，禁用不识别该封装的校验和及分段卸载 |
| `02` + 6 字节 MAC | 建立归该接口所有的 NMI 对端；已有对端返回错误 |
| `03` + 两个 LE64 | 安装发送窗口，起止时间为 CLOCK_MONOTONIC 纳秒；全零关闭窗口 |

对端 HT/VHT 能力与可用信道经标准 station 和 NAN peer schedule 命令提交。公共协议中的 AWDL HT TLV 转换为 IEEE HT capability 后交给 mac80211。

## 限制

- 仅一个 AWDL 数据对端，固定 149；不跟随 161 或其他信道。
- 固件继续执行 NAN 本地日程；AWDL 发送窗口由主机控制，不是固件自主 AWDL/TSF 调度。
- 实测链路和 AirDrop 验收范围见 [验收记录](../../../docs/acceptance.md)。

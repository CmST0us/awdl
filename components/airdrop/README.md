# AirDrop

`airdrop` 提供常驻 D-Bus 管理接口和命令行客户端。文件传输使用固定 Owly `0f2ca4087e6d938d823e76c856fcd04d0a0162b6` 的 TLS、Discover、Ask、Upload 与接收归档实现；发送前查询对端 `_airdrop._tcp` 的 SRV 端口，并核实 TLS Discover 返回的系统设备名。

文件内容直接经过原生 AWDL IPv6 接口。发送和接收各持有一条独占 AWDL 链路；蓝牙只用于发送时唤醒附近的系统 AirDrop。每个操作有独立运行时，停止操作先关闭全部传输连接，再等待管理器释放原生接口。接收器完成一份文件后继续接收，链路断开后重新等待对端。

配置位于 `/etc/airdrop/config.toml`。默认手工接受接收请求；明确设置 `auto_accept = true` 时自动接受。接收内容写入 `directory`，既有文件保留。TLS 证书和服务实例身份仅用于当前接收会话，与 Wi-Fi Aware 持久配对身份独立。Owly 授权原文见 [LICENSE.owly](LICENSE.owly)。

构建、部署、命令及限制见 [AirDrop 使用说明](../../docs/airdrop.md)。

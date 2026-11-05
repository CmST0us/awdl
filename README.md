# Wireless Service

Linux 无线服务组件，Wi-Fi Aware 与原生 AWDL 接入 Intel BE200、RTL8852BE，AirDrop 使用公共 AWDL 接口。这些网卡共用 Rust `wirelessd`、hostap 2.12、Rust HTTP 文件服务和 iOS AwareClient。驱动、固件及无线接入差异位于 `hardware/`。

MT7925 / Dragon Q8B 已接入公共构建和安装入口，固定公开固件已通过 iPad Pro 系统发现；新配对仍在进入 PIN 前失败，详见 [MT7925 验证结论](hardware/mt7925/README.md)。

`wirelessd` 管理无线设备与协议生命周期：Wi-Fi Aware 提供配对、服务注册和安全 NDP 会话，AWDL 提供发现、对端选择和原生数据链路。应用通过 D-Bus 使用对应接口。`fileserver` 是使用该接口的文件应用，可独立安装。一套服务管理一块网卡，业务数据走原生 NDI 与内核 IPv6。

- [AirDrop 与 AWDL](docs/airdrop.md)：系统 AirDrop 发送接收、接受策略与硬件接入限制。
- [架构](docs/architecture.md)：组件、硬件边界与来源版本。
- [配置](docs/configuration.md)：运行默认值、无线策略、客户端契约、本机与安装选择。
- [构建安装](docs/build.md)：Linux、驱动、iOS 与暂存交付。
- [运行](docs/usage.md)：服务、配对和文件传输。
- [D-Bus 接口](docs/service-api.md)与[文件协议](docs/protocol.md)。
- [验证与已知限制](docs/acceptance.md)。

可编辑的本机配置放在 `config/local/`，示例位于 `config/examples/`。`.build/` 存放可重建产物，`.private/` 存放私有诊断材料；配对状态与共享文件保存在目标机器上。

仓库地址：`ssh://git@gitlab-r.eric3u.xyz:20022/oxygen/wireless/wireless-service.git`。现有 Wi-Fi Aware 配置内容与 iOS 安装身份保留。公共 D-Bus 命名空间为 `org.wirelessd1`。

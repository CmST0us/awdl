# Wi-Fi Aware

Linux Wi-Fi Aware 组件，支持 Intel BE200 与 RTL8852BE。两种网卡共用 Rust `wifiawared`、hostap 2.12、Rust HTTP 文件服务和 iOS AwareClient。驱动、固件及无线接入差异位于 `hardware/`。

`wifiawared` 提供配对、服务注册和安全 NDP 会话；应用通过 D-Bus 注册 TCP / UDP 服务。`fileserver` 是使用该接口的文件应用，可独立安装。一套服务管理一块网卡，业务数据走原生 NDI 与内核 IPv6。

- [架构](docs/architecture.md)：组件、硬件边界与来源版本。
- [配置](docs/configuration.md)：运行默认值、无线策略、客户端契约、本机与安装选择。
- [构建安装](docs/build.md)：Linux、驱动、iOS 与暂存交付。
- [运行](docs/usage.md)：服务、配对和文件传输。
- [D-Bus 接口](docs/service-api.md)与[文件协议](docs/protocol.md)。
- [验证与已知限制](docs/acceptance.md)。

可编辑的本机配置放在 `config/local/`，示例位于 `config/examples/`。`.build/` 存放可重建产物，`.private/` 存放私有诊断材料；配对状态与共享文件保存在目标机器上。

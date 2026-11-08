# Wireless Service

Linux 无线服务组件。Wi-Fi Aware 与原生 AWDL / AirDrop 接入 Intel BE200、RTL8852BE 与 MT7925。这些网卡共用 Rust `wirelessd`、hostap 2.12、Rust HTTP 文件服务和 iOS AwareClient。驱动、固件及无线接入差异位于 `hardware/`。

[BCM43752 A2 / AP6275P](hardware/bcm43752/README.md) 的驱动及固件补丁已接入同一份 Rust 服务和 hostap。真机已完成原生 NAN、iPad mini 首次 PIN 配对、缓存认证、安全 NDP 和双向文件内容校验。当前整套候选的 1 GiB 单／四连接下载为 36.90／41.29 MB/s，256 MiB 上传为 30.08 MB/s，内容校验通过；首轮单连接曾停滞，原因未确定。已补齐原生活动查询和空闲会话回收；吞吐仍低于 BE200，长期稳定性及完整固件能力尚未验收，保持实验接入。整套配置及成功／失败结果见[验证结论](docs/acceptance.md#bcm43752-整套候选真机验收)。

MT7925 / Dragon Q8B 已完成 iPad Pro PIN 配对；正式模块通过缓存身份认证、安全 NDP、双向文件和服务重启后重连。CN / 5 GHz / 80 MHz 性能验收为单连接下载 46.52 MB/s、四连接下载 43.44 MB/s、上传 30.69 MB/s；单连接已超过 BE200 已有参考值，四连接与上传仍低于对应参考值。固定固件的 DCEA 信标规避与认证限制见 [MT7925 验证结论](hardware/mt7925/README.md)。

MT7925 原生 AirDrop 已通过同一 iPad Pro 的 1 GiB 随机文件双向传输和往返内容校验：发送 37.72 MB/s，接收 35.87 MB/s；取消后重传接收为 39.33 MB/s。数据使用 149 / VHT80 双流及原生 NAN_DATA `awdl0`，完整配置和验收边界见 [MT7925 AirDrop](docs/acceptance.md#mt7925-原生-airdrop)。

`wirelessd` 管理无线设备与协议生命周期：Wi-Fi Aware 提供配对、服务注册和安全 NDP 会话，AWDL 提供发现、对端选择和原生数据链路。应用通过 D-Bus 使用对应接口。`fileserver` 是使用该接口的文件应用，可独立安装。一套服务管理一块网卡，业务数据走原生 NDI 与内核 IPv6。

- [AirDrop 与 AWDL](docs/airdrop.md)：系统 AirDrop 发送接收、接受策略与硬件接入限制。
- [架构](docs/architecture.md)：组件、硬件边界与来源版本。
- [配置](docs/configuration.md)：运行默认值、无线策略、客户端契约、本机与安装选择。
- [构建安装](docs/build.md)：Linux、驱动、iOS 与暂存交付。
- [运行](docs/usage.md)：服务、配对和文件传输。
- [D-Bus 接口](docs/service-api.md)与[文件协议](docs/protocol.md)。
- [验证与已知限制](docs/acceptance.md)。

可编辑的本机配置放在 `config/local/`，示例位于 `config/examples/`。`.build/` 存放可重建产物，`.private/` 仅保留必要的私有原始证据；三个目录均由 `.gitignore` 排除，不随源码提交。实现与使用结论维护在硬件说明和 `docs/`，不依赖私有临时脚本。重复校验文件、导出附件和已解包的传输包在确认原件保留后可删除；`config/local/` 是持久输入，不能按临时目录清理。配对状态与共享文件保存在目标机器上。

仓库地址：`ssh://git@gitlab-r.eric3u.xyz:20022/oxygen/wireless/wireless-service.git`。现有 Wi-Fi Aware 配置内容与 iOS 安装身份保留。公共 D-Bus 命名空间为 `org.wirelessd1`。

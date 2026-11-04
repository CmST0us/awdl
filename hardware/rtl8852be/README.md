# RTL8852BE

PCI `10ec:b852`，hostap 使用 rtl_nan。保留未关联的物理控制接口及原厂 p2p0；逻辑 nan0 由后端承载，业务流量使用原生持久 ndi0。无线策略由 radio.toml 提供，当前启动组合要求 dual_band=false、master_preference=200、发现信标间隔 100 TU。

`hostap` 包含 vendor netlink、NMI/NDI、调度与密钥适配；`driver` 包含固定 Radxa 来源、NAN 补丁及唯一公共 UAPI；`firmware` 包含 U2 基线和正式补丁链。公共 Rust 服务与 Intel 共用。

驱动只支持一个活动数据站点，活动站点不能在线更换为不同密钥。构建安装见[公共入口](../../docs/build.md)，固件能力见[能力说明](firmware/NAN-CAPABILITIES.md)。

`diagnostics/control.py` 负责依赖 RTL 内核事件的双端观测。`driver_events.py` 按同一 cookie 关联发送提交和完成记录，并依据实际帧长确认结果；服务名和配对信息的长度不固定。

`awdl` 提供公共 AWDL 状态机的原生硬件宿主。驱动补丁 0005–0021 增加原生 awdl0、BA64、HE80、发送窗口门控和真实 DMA 回收，与 NAN 独占。U2 镜像保持原版本，限制见 [AirDrop 与 AWDL](../../docs/airdrop.md)。

补丁 `0022` 将其他发送者发往本机的有效 AWDL 单播数据报告为 `WA_NAN_EVENT_AWDL_SENDER`，携带发送者地址。原生宿主将其转换为公共 `sender-requested` 事件，由接收服务在当前连接释放后重新选择对端；驱动不在接收路径直接更换站点，组播发现不触发切换。

# hostap 无线适配

公共 Rust Radio 通过注册的 `rtl_nan` 驱动使用 RTL vendor NAN 与持久 NDI。配置和 D-Bus 对象由公共服务管理；HTTP 与文件操作不进入此层。

## RTL 数据站点

`rtl_ndi.c` 直接接收 hostap 的 `hostapd_sta_add_params`、`wpa_driver_set_key_params` 和站点授权标志，使用上一级 vendor 后端调用原生 RTL NDI。`phy.c` 负责共同 HT/VHT/HE 能力协商，现有诊断程序与此适配共用实现。

- 创建持久 NDI 后，保存实际 ifindex。
- NMI 能力与协商信道生成数据站点 PHY 配置；数据站点绑定远端 NMI/NDI，不接收业务服务名或 NDP ID。
- 站点和密钥先进入未授权状态；设置 AUTHORIZED 时，通过 `NDI_STA_ADD` 和 `NDI_CONNECT` 安装原生资源，全部完成才返回授权成功。
- 相同密钥的重复安装保留 PN，不重装硬件；活动站点不接受原位更换密钥。
- 撤销授权、删除密钥或站点执行原生排空和删除。失败保留资源和密钥身份，并禁止再次授权；成功清零密钥并保留持久接口。
- 接口创建回复不确定时保留清理责任；明确的 EEXIST 不接管已有接口。

当前承载一个数据站点，不支持活跃站点重键。新会话须完成旧资源释放，并使用协议协商得到的新密钥。能力位不满足时拒绝创建，不降级到诊断 NDP 接口。

## RTL 管理接口

`rtl_nmi.c` 将 hostap peer、密钥与管理帧操作映射到原生 vendor 后端，并输出标准 `EVENT_RX_MGMT` / `EVENT_TX_STATUS`。

- 单个 NMI peer 的 CCMP 密钥绑定独立 cookie；重复安装保留 PN，清理失败保留资源责任并停止使用该密钥。
- IGTK/BIP 与诊断程序共用 `hardware/rtl8852be/hostap/igtk.c`、`group_protection.h`；接收校验 MIC 和重放计数，BIGTK 随原生 STOP 释放。
- 管理帧串行提交，下一帧由真实 TX 完成推进；未知 ACK 不报告成功。TX 状态保留 hostap 提交的原始长度，软件附加的 MMIE 不进入上层完成匹配。
- 受保护 RX 必须匹配当前 key cookie、地址与硬件验证标志；清理失败的密钥不再接收新帧。
- 5 秒发送兜底只处理缺失完成事件；到期停止新增发送，由成功的原生 STOP 释放资源。

## 构建

由根目录 scripts/build-hostap.sh 统一准备协议栈源码、编译本静态库并链接 wpa_supplicant。UP 和 BUILD 由构建工具按目标架构传入，CC / AR 使用对应目标工具链。组件验证见[验证记录](../../../../docs/acceptance.md)。

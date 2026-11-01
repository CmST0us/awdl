# hostap RTL 用户态适配

本组件承载 hostap 的 RTL 后端；当前链接目标为 `wpa_supplicant`。内核基线、补丁与 UAPI 保留在 `hardware/rtl8852be/driver/`。

`backend.c` / `backend.h` 实现 RTL vendor netlink 命令、能力读取、管理帧与完成事件。上层 hostap 协议代码通过该边界使用无线能力；适配器不监听业务端口、不转发文件数据。`phy.c` 提供共享的 PHY 能力协商；[hostap 适配](hostap/README.md)管理通用数据站点操作。唯一 UAPI 为 [`../driver/include/wa_nan_uapi.h`](../driver/include/wa_nan_uapi.h)。

## 持久 NDI

当 `GET_CAPS` 包含 `WA_NAN_FEAT_PERSISTENT_NDI` 时：

1. 原生 NAN 启动后，`NDI_IF_ADD` 创建 `ndi0` 并返回实际 ifindex，不要求 peer 或密钥。接口保持 carrier off，业务发送不准入。
2. 上层可配置 IPv6 地址并绑定监听。`NDI_ADD` 保留会话身份、站点与本地 GTK；`NDI_CONNECT` 在密钥、日程及硬件配置完成后开放数据。
3. `NDI_DEL` 排空在途引用、释放会话资源并关闭数据准入，保留持久接口及 ifindex。清理失败保留原所有权，不能开始下一次会话。
4. `NDI_IF_DEL` 仅删除无会话资源的接口；仍有会话时返回 `EBUSY`。原生 STOP 清理包括持久接口。

接口与 NDP 的生命周期独立，硬件仍只支持一个活动 NDP；该接口扩展不表示已经支持多 peer 或并行 NDP。每个新会话仍须使用新派生的 TK 和新生成的本地 GTK。未显式创建持久接口的现有诊断流程继续由 `NDI_ADD/DEL` 创建、删除 `aware*`。

新操作沿用 ABI4 的可扩展命令编号，并以能力位区分可用性，不改变现有命令的二进制布局。失败返回驱动实际错误；创建成功必须包含非零 ifindex。

## 验证

NDI 生命周期回归覆盖空接口数据拒绝、同 ifindex 重连、PN/BA 重置、在途排空失败、活动会话删除拒绝及 STOP；Linux netlink 回归覆盖命令编码、返回索引和驱动错误传播。相关 ARM64 对象编译通过。组件构建与双卡验收范围见[验证](../../../docs/acceptance.md)。

# BE200 驱动

基线为 Intel `iwlwifi-stack-public:release/core24.70:14867:362ada0a`，包含匹配的 cfg80211、mac80211、iwlwifi 和 iwlmld。已有真机基线为 ARM64 Linux 6.1.115；其他内核按固定来源重新构建，不能复用该内核的模块。`source.json` 固定上游仓库、原始发布包及 Ubuntu 补丁包；构建先应用 Ubuntu 补丁，再按 `patches/series` 应用项目补丁。完整源码只保存在 `.build/targets/<target>/drivers/be200/<kernel>/source`。

## 集成修复

- 非一致性 DMA 平台在读取 RX 描述符前同步 CPU 所有权。
- NAN robust unicast 按目标接口地址分发，NDI 的 Block Ack 不交给 NMI。
- 发现窗口使用实际配置的5 GHz频率。
- 单站点 NAN 数据站点的信道定义与 TLC 命令均未变化时不重发配置，对端时段只改变时间位图或序号时保留已学习速率；多站点组不去重，本地 PHY 链路增删、硬件重启或排队失败后重新下发。
- 对端时段命令提交成功后更新 TLC；提交失败时不留下新时段派生的 TLC 配置与缓存。
- NAN_DATA 使用 mac80211 快速发送路径，逐包读取当前 Cluster ID。
- NAN 聚合上限依据可用 LMAC FIFO，避免访问不存在的 NDI 信道上下文。
- NAN_DATA 的 TLC 聚合建议超过对端上限时，限制到对端及部署上限后继续应用。
- `nan_amsdu_limit` 用于对端大聚合互通限制；`be200.conf` 随每次安装将其设为7935字节。
- `platform_cn` 仅在平台 WRDD 缺失且显式启用时补充CN，沿正常BIOS MCC路径提交。
- TSO A-MSDU 按每个聚合实际引用的内存片段核对描述符预算。
- `IWL_ERR_LIMIT` 在频率限制允许时输出，超出时只写入 trace。
- 构建将 `IWLWIFI_FW_MISBEHAVE_NMI_DEFAULT` 设为 0，驱动一致性检查命中只记录错误，不向固件发 NMI。需要固件现场时，在 `/lib/firmware/iwl-dbg-cfg.ini` 首行写 `[IWL DEBUG CONFIG DATA]`、次行写 `FW_MISBEHAVE_NMI=1`，再完整重新加载 iwlwifi 及其 opmode 模块。

`be200-cn.conf` 只设置 `platform_cn`，由 `--cn` 安装到中国大陆的目标板，不带该参数时移除；其他地区使用当地平台配置。配置与固件共同决定信道权限。

构建入口：`scripts/build-driver.sh be200`。安装入口：`scripts/install-driver.sh be200`。

安装包含匹配的 `iwldvm`、`iwlmvm` 和 `iwlmld`，并在写入前核对全部模块的目标内核版本。Ubuntu Secure Boot、initramfs、其他网卡共存及硬件修订的边界见[构建说明](../../../docs/build.md#内核要求)。

## source.json 字段

`name` / `version` 为固定上游组件与包版本；`license` 为上游授权；`repository` 为来源仓库；`upstream_tracking` 为上游追踪标识；`archive` / `directory` 为源码归档与内部根目录；`packaging_archive` 为 Ubuntu 打包归档，`packaging_license` 为打包授权，`packaging_series` 为归档内的有序补丁列表。修改后重新准备和构建模块。

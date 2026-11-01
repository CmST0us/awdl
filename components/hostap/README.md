# hostap

hostap 2.12 的公共 NAN 协议栈，编入 nl80211 与 rtl_nan，程序名称为 wpa_supplicant。公共 `patches/series` 应用 12 个协议补丁，硬件目录的补丁序列提供驱动注册。协议补丁涵盖 NAN 互通、调度、对端能力事件、缓存认证的本地日程及 NDC 实际信道覆盖。RTL 原生后端、注册补丁和编译片段位于 `hardware/rtl8852be/hostap`。

`hostap.config` 提供公共协议及标准 nl80211 配置；构建时追加硬件后端的编译片段。每个配置项保留注释。展开源码与目标产物在 `.build/targets/<target>/hostap`，不进入 Git。编译入口为 `scripts/build-hostap.sh`，上游授权保留于 LICENSE.hostap。

`source.json` 字段：`name` 为上游组件名，`version` 为固定发布版本，`license` 为授权标识，`repository` 为上游仓库，`archive` 为固定归档地址，`directory` 为归档内的源码根目录。修改来源后重新准备和构建。

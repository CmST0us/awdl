# hostap

hostap 2.12 的公共 NAN 协议栈，编入 nl80211 与 rtl_nan，程序名称为 wpa_supplicant。按 `patches/series` 应用 13 个补丁：共享 NAN 互通和调度修复、对端能力事件、RTL 驱动注册、缓存认证的本地日程及 NDC 实际信道覆盖。RTL 原生后端位于 `hardware/rtl8852be/hostap`。

`hostap.config` 是带注释的上游编译配置。展开源码与目标产物在 `.build/targets/<target>/hostap`，不进入 Git。编译入口为 `scripts/build-hostap.sh`，上游授权保留于 LICENSE.hostap。

`source.json` 字段：`name` 为上游组件名，`version` 为固定发布版本，`license` 为授权标识，`repository` 为上游仓库，`archive` 为固定归档地址，`directory` 为归档内的源码根目录。修改来源后重新准备和构建。

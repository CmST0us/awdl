# Intel BE200

PCI `8086:272b`，hostap 使用 nl80211。运行时所选 PHY 只保留 NAN/NAN_DATA；启动 managed 接口在创建 NAN 后删除。无线策略由 radio.toml 提供，接管与接口约束由 radio.rs 实现。

驱动采用 Ubuntu backport 24.70-14867、项目补丁及 c108 / PNVM 固件。构建安装见[公共入口](../../docs/build.md)，芯片固件及授权位于 firmware。BE200 backport 会替换 cfg80211/mac80211；本组件一次运行一种硬件，不能据此声明与 RTL 驱动可同时运行。

## 已知限制

hostap 被强制终止后，cfg80211 异步销毁 NAN 接口，现有 iwlmld 的群组密钥清理可能在广播或管理站点已失效时触发 `iwl_mld_get_key_sta_mask` 警告。公共服务正常停止时等待 hostap 退出与内核接口释放；驱动异常退出路径的修复独立于该正常生命周期。

## 原生 AWDL

`wirelessd.service` 在现有 NAN 管理接口上承载 AWDL，使用独立 NAN_DATA `awdl0` 和原生内核数据路径。控制适配、驱动接口与资源生命周期见 [AWDL 后端](awdl/README.md)。

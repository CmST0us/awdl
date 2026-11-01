# Intel BE200

PCI `8086:272b`，hostap 使用 nl80211。运行时所选 PHY 只保留 NAN/NAN_DATA；启动 managed 接口在创建 NAN 后删除。无线策略由 radio.toml 提供，接管与接口约束由 radio.rs 实现。

驱动采用 Ubuntu backport 24.70-14867、项目补丁及 c108 / PNVM 固件。构建安装见[公共入口](../../docs/build.md)，芯片固件及授权位于 firmware。BE200 backport 会替换 cfg80211/mac80211；本组件一次运行一种硬件，不能据此声明与 RTL 驱动可同时运行。

# Wi-Fi Aware Publisher

Rust 守护进程通过 Unix 控制接口管理公共 hostap，提供配对窗口、NDP 会话和多个 TCP / UDP 服务注册。D-Bus 公共命名空间为 `org.wifiawared1`；接口见[服务 API](../../docs/service-api.md)。

`configuration.rs` 读取带注释的公共默认值及部署覆盖；`hardware/radio.rs` 通过共用硬件表选择后端并读取 radio.toml；`radio.rs` 管理进程、接口和本地日程；`publisher.rs` 串行管理认证、服务和会话；`api.rs` 发布状态快照。硬件接管和接口约束分别位于 `hardware/*/radio.rs`。

就绪、进程退出和接口变化由 inotify、子进程等待和 netlink 推进；配置期限仅用于失败兜底。缓存认证使用 hostap 已提交的本地日程，NDP 准入继续遵守对端能力。终止失败的会话保留清理责任，期限到达仍未完成时退出无线服务释放资源。

运行配置位于 `/etc/wifiawared/config.toml`，硬件策略安装到 `/usr/local/share/wifi-aware/hardware/<name>/radio.toml`。构建与部署见[构建文档](../../docs/build.md)，使用见[运行文档](../../docs/usage.md)。

# Rust 公共支持

`aware-common` 提供两个 Linux 服务共用的 D-Bus 名称、对象路径、接口名称和会话类型，以及禁用属性缓存的代理创建、五秒方法超时、进程退出信号、运行锁、随机数和单调时钟支持。

`bus.rs` 不持有无线会话状态。`unix.rs` 中的系统调用由 Rust 文件所有权约束资源生命周期，`unsafe` 限于 libc 接口。文件数据块统一为 256 KiB。

依赖在工作区 `Cargo.toml` 声明，实际版本与来源由 `Cargo.lock` 固定。两个服务通过 zbus 直接连接 system D-Bus。

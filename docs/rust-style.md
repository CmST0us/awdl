# Rust 编码约定

项目自有 Linux 服务和构建工具使用 Rust 2024 edition，最低工具链为 1.93，格式使用 `cargo fmt`。模块和函数使用 snake_case，类型使用 UpperCamelCase，常量使用 SCREAMING_SNAKE_CASE。

无线状态由 Publisher 任务串行修改；D-Bus 对象仅暴露快照和命令通道。文件服务的连接、上传临时文件、目录锁和子进程使用明确的所有者，任务结束时释放对应资源。锁只覆盖共享状态访问，不跨越异步 I/O。

错误在组件边界添加操作上下文，协议错误保持既有 D-Bus 名称和 HTTP 状态。`unsafe` 仅用于没有对应标准库接口的系统调用，并注明参数与资源的有效性。

第三方依赖由 Cargo.lock 固定，构建使用 `--locked`。驱动及 hostap 补丁保持上游风格，iOS 保持 Swift 风格。

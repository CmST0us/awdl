# AWDL 控制

公共 AWDL 状态机基于固定 Owly 提交 `0f2ca4087e6d938d823e76c856fcd04d0a0162b6`，维护发现、选主、同步、对端能力缓存和发送日程。原生接口及 vendor 命令由 `hardware/rtl8852be/awdl` 执行，IP 数据直接经过内核 `awdl0`，不经控制管道或 D-Bus。

控制库与硬件宿主由 `scripts/build-awdl.sh` 统一构建，依赖进入根 Cargo.lock。Owly 的 MIT 原文保留于 [LICENSE.owly](LICENSE.owly)。来源为 RTL 仓库 `awdl` 分支的 `b76e5bf`；驱动补丁 0005–0021 接在现有四个 NAN 补丁之后，U2 镜像保持原版本。

RTL 支持一个原生 AWDL 数据对端，在主信道 149 启动并跟随同一 80 MHz 块内的 149/161。HE 能力按 Apple Data State 与对端能力处理。发送由主机协议时钟、内核 hrtimer 和真实硬件完成事件推进；U2 不运行 AWDL 协议状态机。

# 构建

在仓库根目录使用公共入口：

```sh
scripts/build-driver.sh rtl8852be --platform rock5b \
  --kernel-release <目标内核版本> --headers /opt/kernel-headers/rock5b \
  --cross-compile aarch64-linux-gnu- --mips-cc clang
```

本机工具链、headers 与并行度可持久保存到 `config/local/build.toml`；字段说明见[构建与安装](../../../docs/build.md)。目标内核 headers 中的 fixdep、genksyms、modpost 必须能在构建环境执行。

## 固定输入与产物

`source.json` 固定 Radxa rkwifibt 提交 `b61a1e8499a4f1956ef417cbcfaa6c2cf805ce08` 的 `drivers/rtl8852be/` 子树。准备程序核对原有归档摘要，以零 fuzz 应用 `patches/series`，并核对内核与用户态 UAPI。可通过 `--archive /path/to/rkwifibt.tar.gz` 复用本地归档。

公共构建入口依次准备源码、重建完整 U2 镜像、嵌入数组、调用 Kbuild、核对最终模块中的固件字节和下载长度。U2 使用包含 MIPS16 后端的 Clang 21 与 GNU MIPS little-endian binutils。嵌入内容未变化时保留 C/H 修改时间，Kbuild 按实际依赖增量构建。

产物位于 `.build/targets/<Rust target>/drivers/rtl8852be/<kernel>/`：`source/8852be.ko` 为模块，`firmware/candidate.bin` 为嵌入镜像。改变源码清单或补丁后重新准备生成树；正式内核修改只维护在补丁中。

单独准备源码：

```sh
scripts/aware-build prepare-source rtl8852be --platform rock5b \
  --kernel-release <目标内核版本> --archive /path/to/rkwifibt.tar.gz
```

准备源码不替代固件重建与驱动构建。完整 NAN 能力依赖[组合固件](../firmware/NAN-CAPABILITIES.md)。

安装使用 `scripts/install-driver.sh rtl8852be --kernel-release <目标内核版本> --country <实际国家码>`；`--destdir` 可将模块、地区配置和授权暂存到独立目录。安装器检查 vermagic，启用 Secure Boot 时检查模块签名。

## 原生队列观测

NAN持有射频时，原生watchdog输出`WA_NAN_QUEUE`、`WA_NAN_DMA`和`WA_NAN_RXDMA`，与`ndi-fw-txsts`和`ndi-tx-phase`按内核时间关联。采样沿用watchdog周期，不增加数据路径轮询。

- `QUEUE`：每个MACID的TID0软件环游标和DMA通道。`core`与`phl`不同表示仍有请求待PHL消费。
- `DMA`：硬件TXBD主机/设备游标、可用描述符、WD空闲/忙/待提交数量，以及等待payload报告、成功、失败和回收失败计数。TXBD已消费不等于空口发送成功。
- `RXDMA`：数据与完成报告接收环的硬件游标及待收数量。与发送统计共同区分报告未处理和设备尚未完成发送。
- `RADIO`：端口0/2的TSF，以及8852B的信道报告编码（0x0734）、RF带宽（0x49c0）和主子信道（0x49c4）寄存器；与本轮端口分配和committed位图关联，不能以IQK校准表代替当前信道。
- `GATE`：每个活动软件环MACID对应的硬件sleep（0xc2c0组）与pause（0x9eb0组）字；使用MACID在字内的位索引解码，结合TSF判断当时是否应开放。

这些是各层即时快照，跨层读取期间队列仍可变化；用连续停顿样本定位，不以单个活跃样本推断丢包。

`ndi-tx-failure`保留每个失败MPDU的PN、802.11序号、硬件完成状态、完成TSF和等待时间，不受成功路径日志限频影响。`tsf_valid=1`时可与本轮日程边界关联；等待时间包含排队与关闭窗口，不能直接作为空口发送时长。

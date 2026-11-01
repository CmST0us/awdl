# 固件

RTL8852BE 的 U2 WiFi 固件内嵌在驱动模块里，不是 `/lib/firmware` 的独立文件。板端 `/lib/firmware` 只有 Bluetooth 镜像。

正式构建采用固定基线加有序补丁：`source.json` 固定原始 U2 镜像，`patches/series` 定义应用顺序，补丁描述固定构建参数。入口 `tools/build_nan.py` 在执行前校验基线，逐步编译和注入补丁，最后生成镜像；定义见[补丁序列](patches/README.md)。

## 内容

```
u2/
  rtl8852be_fw.bin      原始U2镜像，325304 B
  VERSION               原始镜像来源与校验
  rtl8852be_fw_nan.bin  原生NAN/数据预约镜像，341688 B
  VERSION.nan           NAN版本、校验与验证入口
```

## 来源

原始镜像从 `8852be.ko` 的 `array_8852b_u2_nicce` 符号只读提取：

| 项 | 值 |
| --- | --- |
| 模块 SHA-256 | `64a0658b48ec15ea315511d3ff9138e9035a5badf0d1e3e2dcfb6b6c6fa78203` |
| 固件 SHA-256 | `a310447813efc85a81dd2d88ac7f106b6c382acd759e5753984785f4d2db09a2` |
| 大小 | 325304 B |

## 如何迭代

固件是驱动源码的一部分：上游 `rtl8852b_fw.c` 里以数组形式定义。替换数组内容后重新构建驱动并加载，才能改变固件行为。

- 不能通过复制 `/lib/firmware` 单文件替换。
- 替换设备模块前保存现有模块和参数。
- 当前镜像没有版本字符串打印，dmesg 里的 `0xd9b744d8` 是 Bluetooth 固件版本，不是 WiFi。

修改代码或数据段后必须重算安全段的明文摘要，否则正常下载返回 `FWDL_SECURITY_FAIL`（73）。工具与证据见 [patches/u2-digest-bypass.md](patches/u2-digest-bypass.md)。

```sh
python3 tools/u2_digest_bypass.py u2/rtl8852be_fw.bin <候选.bin>
```

安全段 `0x3C0`–`0x7FF` 的空白区只能放数据，取指会触发SER。自定义MIPS16e代码使用代码段的已验证调试槽，或已验证的代码段扩展空间；构建入口与执行证据见 [DEVELOP.md](DEVELOP.md)。

## 开发

固件开发工具链见 [DEVELOP.md](DEVELOP.md)：镜像解析、字节修改、代码注入、模块构建与验证。

BE200 c108 到 RTL U2 的无线能力映射、完整候选组合与验证边界见 [原生 NAN 能力](NAN-CAPABILITIES.md)。

## 已确认能力

- `FWCAP` MAC dword0 `0x3db`，bit4 `nan=1`。
- 主机 ABI 只有 8 个 NAN H2C 与 9 个 C2H，没有 PASN/NIK/NDP/NDI 命令。
- 本机非安全芯片（`is_sec_ic=0`）的正常下载只比对明文摘要，不验证 MSS 签名；改代码或注入新代码都可加载。
- [原生启动、FAW 与完整 STOP](patches/u2-nan-stop.md) 已通过启动同步信标对齐、单板执行、窗口取消、最后 DEL 与 MACID 复用验证；iPad mini安全配对与数据验证见[组件验收](../../../docs/acceptance.md)。
- [原生信标逐帧取证](patches/u2-beacon-trace.md) 关联消费者、模板更新、门控和实际TX结果；同步端口512TU及TBTT窗口配置已通过板端验证。

## 约束

- 固件保持 Realtek 原生，禁止用 monitor 或普通网络方案替代验收路径。
- 替换前保留原镜像与原模块，便于回退。

## NAN数据版本

正式镜像按信道恢复双链 IQC、TSSI PA 对齐、TXAGC 及 CCK/OFDM DE；恢复在原生切信道完成后、FAW 发送开放前执行。配对前本地数据预约与认证使用独立上下文，数据站点接管后旧本地预约失效。MIPS 状态机回归和公共 hostap 原生双信道文件双向传输和生命周期验收通过；当前验收结果见[组件验收](../../../docs/acceptance.md)。

`u2/rtl8852be_fw_nan.bin`与正式`build_nan.py`输出一致，随本项目驱动和控制程序使用；不可单独加载到原驱动。固件支持原生信标保护、管理/数据独立FAW、每owner预约描述符、2.4/5GHz切换、本机接收驻留及数据预约优先。数据记录有效时保留 DW 同步与认证；发现信标在当前预约信道上继续发送，不抢占数据驻留。

iPad mini最终无探针实测每方向30MiB，下行4.880682、上行9.398577MB/s，TXerror/RXdrop/TXdrop均0，19.311秒完成原模块138参数和App恢复。共同数据窗口可从25%扩大到81.25%，对端也可能保持25%；吞吐及连接重复性仍有波动。硬件CCMP、原生队列及双向BlockAck/A-MPDU已接入，90MB/s及300秒稳定性未通过。全部短测与失败计数见[组件验收](../../../docs/acceptance.md)。

从仓库根目录重建、嵌入和核对：

```sh
python3 -B hardware/rtl8852be/firmware/tools/build_nan.py --output .private/nan-firmware
python3 -B hardware/rtl8852be/firmware/tools/u2fw.py embed hardware/rtl8852be/firmware/u2/rtl8852be_fw_nan.bin --array <生成树>/phl/hal_g6/mac/fw_ax/rtl8852b/hal8852b_fw.c
python3 -B hardware/rtl8852be/firmware/tools/u2fw.py verify <构建的8852be.ko> hardware/rtl8852be/firmware/u2/rtl8852be_fw_nan.bin
```

模块构建与恢复见[驱动构建](../driver/BUILD.md)及[构建安装](../../../docs/build.md)。

### 聚合重试策略写入跟踪

`python3 tools/u2_mmio_writer_trace.py u2/rtl8852be_fw_nan.bin <输出目录>` 生成独立诊断镜像，保留原生门控/信道跟踪，并在首次原生门控执行后安装 MMIO 网关。仅对 `0xc604` 的写入记录调用者返回地址和写入值；NAN 日志 marker 241/242/244 分别表示8/16/32位写入。记录发生在原写入之后，不改变原返回值；经其他直接MMIO路径的写入不在覆盖范围内。

该镜像仅用于完整模块加载与恢复窗口，模块重载重新初始化固件RAM；不能把STOP视为跟踪网关卸载。`python3 tools/test_u2_mmio_writer_trace.py <输出目录>` 在MIPS QEMU中验证原调用、返回值、过滤和重复安装，离线测试不替代板端验证。

`python3 tools/read_mmio_writer_trace.py <kernel.jsonl> <output.json>` 解码写入宽度、调用者和原始32位值，保留采集时间；返回地址标识网关的直接调用者，不等同于策略的上层发起者。

解码结果同时给出模式3（低两位为3）的已闭合区间和末尾未闭合区间。区间采用主机采集时间，表示已观测写入之间的间隔，不是固件精确执行耗时；重复写入3不会重置区间起点。

marker 243补充已确认setter与分发器调用链的上层返回地址及相同写入值。生成器核对两个原生栈帧入口，运行时核对两层保存的返回地址后才读取更高层；其他调用链不输出该marker。解码结果将其保存在`parent_events`，不计入实际写入次数。

### 重试模式消融

`python3 tools/u2_retry_mode_ablation.py <含写入跟踪的候选镜像> <输出目录>` 仅将原生setter的低两位掩码改为0，使其保持AGG重试模式。它保留对象状态、A-MPDU、速率控制和日程，用于判定模式3是否是停顿的必要条件；不是正式修复。实验必须结合写入跟踪与寄存器读回确认变量成立，结束后恢复原模块。

## source.json 字段

`name` 为固件标识；`file` 是相对此目录的原始镜像路径；`size` 为原始镜像字节数；`sha256` 是原项目的输入校验；`source_symbol` 为原模块中的数组符号；`source_module_sha256` 记录提取来源模块。构建只以原始镜像为输入，安全段摘要属于固件下载格式。

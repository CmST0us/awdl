# U2 固件开发

`hardware/rtl8852be/firmware/tools/u2fw.py` 负责镜像解析、修改、数组嵌入和最终模块验证。自定义代码写入可复用槽位，入口使用 MIPS16e `jal`，安全段摘要自动重算。驱动模块统一通过 [公共驱动构建入口](../driver/BUILD.md) 构建。

正式镜像由 `source.json` 与 `patches/series` 重建；功能变更修改对应补丁代码和参数，不直接修改交付镜像。构建中间产物放在 `.build/` 或 `.private/`。

## 工具链

| 命令 | 作用 |
| --- | --- |
| `u2fw info <image>` | 解析段布局、动态头、摘要一致性 |
| `u2fw slots` | 列出可复用代码槽及容量 |
| `u2fw patch <image> --offset 0x.. --hex .. -o out` | 按偏移改字节并重算摘要 |
| `u2fw inject <image> --slot <name> --code <bin> -o out` | 注入代码、生成 `jal`、重算摘要 |
| `u2fw embed <image> --array <c>` | 同步内嵌数组、头文件声明与下载长度 |
| `u2fw verify <ko> <image>` | 逐字节比对模块内唯一数组，核对独立下载长度 |
| `u2fw extend <image> --bytes 0x4000 -o out` | 扩大代码段填充并重算摘要，用于探测可用 RAM |

`embed` 和完整 NAN 组合工具共用同一嵌入函数；字节与长度均未变化时保留 C/H 修改时间。`verify` 的数组内容或下载长度任一不匹配均返回非零。完整 NAN 镜像使用 [能力映射中的重建入口](NAN-CAPABILITIES.md#候选重建)。

## 快速开始

```sh
# 1. 写 MIPS16e 代码
mkdir -p .private/u2-dev
cat > .private/u2-dev/hook.S <<'EOF'
	.set noreorder
	.set mips16
	.text
	.globl entry
entry:
	li	a0, 4095
	jr	ra
	nop
EOF
mipsel-linux-gnu-as -EL -mips32r2 -mabi=32 -o .private/u2-dev/hook.o .private/u2-dev/hook.S
mipsel-linux-gnu-objcopy -O binary -j .text .private/u2-dev/hook.o .private/u2-dev/hook.bin

# 2. 注入到可执行槽
python3 hardware/rtl8852be/firmware/tools/u2fw.py inject hardware/rtl8852be/firmware/u2/rtl8852be_fw.bin \
  --slot debug-handler --code .private/u2-dev/hook.bin -o .private/u2-dev/candidate.bin

# 3. 嵌入生成树，随后执行 Kbuild
python3 hardware/rtl8852be/firmware/tools/u2fw.py embed .private/u2-dev/candidate.bin \
  --array .private/nan-build/source/phl/hal_g6/mac/fw_ax/rtl8852b/hal8852b_fw.c

# 4. 验证内嵌镜像
python3 hardware/rtl8852be/firmware/tools/u2fw.py verify .private/nan-build/source/8852be.ko .private/u2-dev/candidate.bin
```

单次工具调用上限30秒；完整对象构建按真实 Kbuild 目标分批，聚合、modpost、链接分别执行。阶段成功后再推进，超时或失败不产出可上板结论。

## 可复用槽

| 槽 | 地址 | 容量 | 说明 |
| --- | --- | ---: | --- |
| `stub` | `0xb89a69fc` | 8 B | `NANPSTIMERTEST` 调试 stub，仅调试命令表引用 |
| `debug-handler` | `0xb89a84fc` | 284 B | `NANRCRECSYNC` 调试 handler，仅调试命令表引用 |
| `secure-gap` | `0xb8e11ce8` | 1088 B | 安全段空白区，**只能放数据**，放代码会取指异常 |

默认入口是代码段 `0x3B796` 的调度日志立即数（`0xb89ab6f6`），它在 NAN START 路径上，换成 `jal` 后每次调度都会调用注入代码。要换入口时用 `--hook-va`，该位置必须原本是 4 字节指令。

注入的代码必须 `jr ra` 返回；入口指令的延迟槽保持原样。

## 上板

工具链只产出离线镜像和模块。上板按[构建安装](../../../docs/build.md)安装：核对下载输入、实际 FWDL 结果和 INIT_RDY，完成实验后显式恢复原模块及全部参数。`WA_OBS download`只证明主机提供的输入；固件取指异常的 SER 与 `FW EPC/RA` 属于失败证据。

## 代码空间边界

镜像三个 section 原本填满，只剩下 `stub` 8 字节与 `debug-handler` 284 字节可复用。代码段可以向后扩展，但实测边界很近：

| 扩展 | 结果 |
| --- | --- |
| +4 KB | 通过：两次 `attempt-end result=0`、`Ready` |
| +16 KB | 通过：两次 `result=0`、`Ready`、首帧 |
| +64 KB | 失败：SER，无 `Ready` |

代码段扩展需要同步 section 0 长度、`hal8852b_fw.h` 的 `extern` 长度、`hal8852b_fw.c` 的数组及 `array_length_8852b_u2_nicce`。镜像修改命令更新段长度，`embed` 同步 C/H 和下载长度，最终 `verify` 从实际 ELF 核对。

16 KB 不足以容纳完整 NAN 协议栈（PASN/SAE 大整数运算、NIK/NPK、NDP/NDL）。要继续做固件侧协议栈，只能裁剪现有功能腾空间，或把协议栈留在主机、固件只加关键 hook。

## 限制

- **代码空间有限**：可复用区 292 字节；代码段扩展实测到 16 KB，64 KB 失败。
- **镜像几何固定**：段地址、长度与加载表都是硬编码，改段边界会与固件内指针冲突。
- **摘要必须重算**：改代码或数据后不重算安全段摘要，正常下载返回 `FWDL_SECURITY_FAIL(73)`。
- **仅限非安全芯片**：验证对象是 `is_sec_ic=0` 的板子；安全芯片与 `mss_idx>0` 分支未验证。
- **覆盖槽的副作用**：被覆盖的调试命令会失效；不要覆盖正常路径上的函数。

## 已验证示例

| 窗口 | 槽 | 结果 |
| --- | --- | --- |
| `20261023-STUB-02` | `stub`（8 B） | 注入 `li a0,4095; jr ra`，START 内打印 `line: 4095`（原 2953），首帧、STOP、恢复 PASS |
| `20261023-TOOLCHAIN-01` | `debug-handler`（284 B） | 同一流程由 `u2fw inject` 生成，`line: 4095` 执行，PASS |

证据在 `.private/artifacts/U2-FW-BYPASS-001/`。

# 原生信标逐帧取证

`u2_beacon_trace.py` 在含512TU DW相位修复的4 KiB NAN STOP代码扩展中加入496字节MIPS16代码，关联原生消费者、模板更新、端口门控与逐帧TX结果。取证镜像保持原生参数、返回值和发送处理。

从仓库根目录执行，输出目录须尚不存在：

```sh
mkdir -p .private/beacon-trace-input
python3 hardware/rtl8852be/firmware/tools/u2fw.py patch hardware/rtl8852be/firmware/u2/rtl8852be_fw.bin --offset 0x38458 --hex 00 -o .private/beacon-trace-input/phase.bin
python3 hardware/rtl8852be/firmware/tools/u2_nan_stop.py .private/beacon-trace-input/phase.bin .private/beacon-stop
python3 hardware/rtl8852be/firmware/tools/u2_beacon_trace.py .private/beacon-stop/candidate.bin .private/beacon-trace
```

输出镜像需替换驱动内嵌数组并重新构建模块。代码位于 `0xb89bb8a0`；工具要求该扩展区域为空，重算加载器要求的摘要。MIPS16调用的延迟槽使用单个16位指令。

## 记录格式

诊断复用日志ID857的三个整数，格式中的 `FAIL` 是既有格式名。除消费者返回外，`ret`均为端口0硬件TSF的低32位，记录的是取证点时间。启动/STOP可能重置TSF；双端合簇还可能调整时钟，排序必须限定在同一时钟连续区间。

| func | 取证点 | line编码 |
| --- | --- | --- |
| 248 | `0xb89a6c80` 门控调用返回 | port \| enable<<8 \| band<<24 |
| 250 | `0xb89a41d8` 模板更新调用返回 | port \| offset<<8 \| length<<16 \| band<<24 |
| 251 | `0xb8992e32` 已识别的逐帧beacon TX report | port \| failure<<8 \| mbssid<<16 \| band<<24 |
| 252 | `0xb89a4818` 消费者进入 | port \| mbssid<<8 \| band<<16 |
| 253 | 消费者返回，ret为原生返回值 | 同252 |

251来自原生RX report type6/body4，failure=0为发送成功。248/250保留原调用返回值，但日志未包含该返回值，不能单独证明目标寄存器或模板的最终内容。未启用端口也会进入消费者，不能将所有252计为发送。

驱动同时记录 `FW_INFO/BCN_CNT`。U2有效载荷为7字节：byte0为端口/MBSSID与band，byte1–5为失败计数，byte6低四位为成功计数、高四位为总数；累计10次报告一次。主解析器与事件解析器均按7字节读取，短包在分发前拒绝。

## 验证与边界

同步端口使用512TU硬件TBTT，并通过原生 `MAC_AX_PCFG_BCN_PRCT` 启用TBTT信标窗口（TBTT_PROHIB和BRK_SETUP）；发现端口使用100TU。组合候选取得同步3次、发现9次成功TX，消费者到报告约5.9毫秒，STOP及原模块140项参数恢复通过。原始证据为 `.private/artifacts/M5-BCN-ORDER-04/`，计数见 [组件验收](../../../../docs/acceptance.md)。

[原生启动补丁](u2-nan-stop.md)同时复位TSF与信标计时器，已验证首个及稳态同步发送与DW窗口对应。当前单板记录只观察到初始化时的模板字段更新，后续合簇或角色变化时的覆盖字段稳定性尚待验证。该工具不提供BIGTK保护；TX报告不代替空口MIC/IPN及iPad配对验收。

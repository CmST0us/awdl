# DW/FAW 管理 MACID 门控取证

`u2_nan_gate_trace.py` 在完整 NAN 候选上增加带序号诊断代码。原生 `0xb8979c0c` 完成写入后，读取该 MACID 对应的硬件 sleep（`0xc2c0`）和 pause（`0x9eb0`）位，保留原始参数、返回值与窗口策略。仅记录 reason35（DW）和36（FAW）；不输出密钥或帧内容。

```sh
python3 -B hardware/rtl8852be/firmware/tools/u2_nan_gate_trace.py \
  hardware/rtl8852be/firmware/u2/rtl8852be_fw_nan.bin .private/nan-gate-trace --sequenced
```

将输出镜像通过 `u2fw.py embed` 嵌入驱动并重编对应对象，使用 `u2fw.py verify` 核对实际模块数组与下载长度。此诊断入口独立于 `build_nan.py`，不改变正式候选组合。

带序号门控代码位于 `0xb89bdc00`，切信道观测位于 `0xb89bde00`，原生入口的8字节栈前导由 trampoline 执行，再返回 `0xb8979c14` 的原始函数体。工具核对入口指令和空闲代码区，并重新计算加载器要求的摘要。

记录复用固件日志ID857，`func=249`，其中 `ret` 为取样时端口0 TSF低32位，`line` 编码如下：

| 位 | 内容 |
| --- | --- |
| 0–7 | MACID |
| 8 / 9 | 请求的 pause / sleep |
| 10 / 11 | 硬件读回的 sleep / pause |
| 12 | reason35为0，reason36为1 |
| 13–31 | 全部MACID共用的19位递增序号 |

同一时钟连续区间内可与原生 DW/FAW 和接收计数器关联。记录证明写入后该次取样的硬件值，不代表两次取样之间没有其他写入；TSF采用或复位前后不能直接比较低32位时间。

原生`b8979c0c`参数顺序为`(macid, pause, sleep, reason)`。解码器将请求与读回统一为低位sleep、高位pause后比较；不对原始位8/9直接比较。

计数使用BIGTK的1088字节数据分配末尾4字节（`0xb8e12124`），不占用密钥、日程或射频状态；常量布局保留此位置，生成器核对镜像中的初始值为零。固件加载时初始化；每次被观测的原生门控调用在日志提交前递增，19位字段自然回绕。分析时必须包含其他MACID的序号，不能在筛选数据MACID后直接判断是否丢事件。重复日志不补足缺失序号；回退、重置或中断导致的日志次序异常保留为未知。

`summarize_nan_gate_duty.py`只对序号完整区间累计开闭时长，同时报告`unknown_us`与`sequence_coverage_percent`。快照矛盾时不输出开放比例。序号连续只证明所观测调用的事件连续，不能排除其他写寄存器路径，也不证明射频驻留。

默认不传`--sequenced`时保留marker246及240字节布局，用于既有MMIO安装与迁移工具的参考代码核对。旧日志仍可解码，但没有连续性证据的区间不计入开放比例。

原生调试查询会把异步日志带入命令回复；取证时同时保存内核JSONL与`runtime-reads.jsonl`、`timing-reads.jsonl`。回复事件先使用前后内核TSF锚点合并，再裁剪NDI生命周期；清理阶段锚点可定位传输尾部事件，但不跨下一次NDI或TSF复位。

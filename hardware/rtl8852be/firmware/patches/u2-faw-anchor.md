# 原生FAW时钟锚点更新

U2在集群合并后采用对端TSF。日程记录重算若继续使用旧双域锚点，候选计算会用32位周期乘积追赶新的64位TSF，无法收敛，阻塞后续无线事件和STOP。

`u2_faw_anchor.S`在记录重算入口`0xb89ad6c0`调用原生时钟转换函数`0xb89aabac`，刷新`0xb8e16de4`的端口TSF与free-running锚点，再进入原生位图计算。使用band0、当前NAN端口、mode0，不添加18TU偏移；保留记录表、记录键、返回地址及原生112字节栈帧。调用者随后重建候选表。

补丁80字节，位于12KiB代码扩展的`0xb89bd800`，输入为[原生BIGTK](u2-bigtk.md)镜像。原生日程位图、周期和窗口选择保持不变。支持管理与数据独立记录、2.4/5GHz信道及512TU周期。

## 构建与取证

```sh
python3 hardware/rtl8852be/firmware/tools/u2_faw_clock_trace.py .private/bigtk-fw/candidate.bin .private/faw-clock-trace
python3 hardware/rtl8852be/firmware/tools/u2_faw_anchor.py .private/faw-clock-trace/candidate.bin .private/faw-anchor
```

生成的`candidate.bin`嵌入驱动模块，使用`verify_u2_module.py`核对嵌入字节。每个输出目录须不存在，MIPS16链接使用2字节对齐。锚点修复也可直接应用于BIGTK镜像。

取证补丁64字节，入口`0xb89ad4ec`，代码位于`0xb89bcac0`。`0xb8e12080`的24字节记录依次为表地址、记录索引、当前TSF低/高32位、调用者、进入次数；不包含密钥。通过既有AXIDMA读回入口读取。

## 验证

故障输入为TSF `0x00686941e2d3ca0c`与零端口锚点，原生周期计数已达4230007。刷新至当前8192TU边界后，同一输入只需11次512TU推进。

双端验证取得连续10次FAW开启/关闭、BIGTK IPN79及持续iPad订阅；后续主机组合取得34次开启/关闭，并完成bootstrap与认证首帧响应。两轮均完成原生STOP、连续内核采集和原模块140项参数恢复。固件候选为`6C03EF36713908E9BC5D905`；原始证据见`.private/artifacts/M5-SECURE-PAIR-06/`至`M5-SECURE-PAIR-08/`。

## 已知限制

首次双端验证的窗口起点为周期内16414–16856µs、终点32624–33072µs，其中一次结束晚于宣告32TU边界304µs。时钟停滞已修复，严格窗口边界仍须收紧。完整组合镜像支持安全配对，组件验证范围见[组件验收](../../../../docs/acceptance.md)。

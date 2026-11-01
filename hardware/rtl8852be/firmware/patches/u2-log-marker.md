# U2 原生调度日志准入候选

基线为 `hardware/rtl8852be/firmware/u2/rtl8852be_fw.bin`，325304字节。工具 `hardware/rtl8852be/firmware/tools/u2_log_marker.py <基线> <独立输出>` 生成候选；输出不得覆盖基线。

| 项目 | 基线 | 候选 |
| --- | --- | --- |
| 指令地址 | `b89ab6f6` | 同左 |
| 文件指令范围 | `3b796…3b79a` | 同左 |
| 小端字节 | `81 f3 09 6c` | `81 f3 1b 6c` |
| MIPS16 指令 | `li a0,2953` | `li a0,2971` |
| 唯一变更 | 文件 `3b798 = 09` | 文件 `3b798 = 1b` |

函数 `b89ab69c` 在模块分配位非零时打印调度状态。该立即数随后由 `b89ab702: sw a0,24(sp)` 写入日志line实参；`b89ab706` 已重载a0，`b89ab71c` 经槽 `b8e098f0` 调用日志入口。func实参仍为255，ret实参仍为 `b8e16dc8` 的状态字节。指令长度、寄存器选择、栈布局、调用目标和控制流不变。

固件事件ID857对应原驱动格式 `[NAN] FAIL in func: %d, line: %d, ret = %d`。候选预期输出 `C2H log: [NAN] FAIL in func: 255, line: 2971, ret = <原状态>`；这条既有格式的FAIL不表示H2C拒绝。不能把其他未解释异常归入此状态日志。

RAM代码section的单字节变化不改变header、三个section长度和地址、security section及两份MSS签名。正式验证保留正常下载与ready检查，以init-ready、该固件marker、完整恢复分别判定。没有marker时不能报告修改点执行通过。

## 已知边界

该候选只检验当前设备正常加载路径对这一修改的准入与执行。它不证明任意固件修改、扩展空间、逐帧回调、BIGTK或配对数据链路可用。原镜像与原模块独立保留；实验由设备持有会话按批准窗口执行。

## 构建与模块确认

在独立冻结驱动树中，用 `u2_log_marker.py` 的 `--array-input <基线hal8852b_fw.c> --array-output <独立hal8852b_fw.c>` 生成内嵌数组源码。工具要求原数组与基线镜像逐字节一致；将输出用于独立构建树，按真实Kbuild依赖重建模块。

`python3 hardware/rtl8852be/firmware/tools/verify_u2_module.py <8852be.ko> <候选bin> --extract <独立提取bin>` 从AArch64 ELF符号 `array_8852b_u2_nicce` 提取并直接比较全部字节。等长文件或srcversion变化不能替代这一对应关系。

`u2_marker_session.py` 复用冻结board/publication工具进行一次启停，`u2_marker_host.py` 控制16秒SSH失败兜底。所需输入及基线由实验manifest给出，运行需设备窗口。`test_u2_marker.py --board-tools <冻结工具目录>` 验证来源边界和失败后的恢复决策。

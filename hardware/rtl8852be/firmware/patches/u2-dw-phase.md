# U2 2.4 GHz DW 起点消融

`u2_dw_2g_probe.py` 从基线模块生成同尺寸实验候选，仅将 `0xb89a83b8` 的 `sltiu a0,128` 改为 `sltiu a0,0`，并重算加载器要求的两份摘要。驱动代码、固件数据和 ELF 布局保持。

```sh
python3 hardware/rtl8852be/firmware/tools/u2_dw_2g_probe.py baseline.ko .private/dw-phase-probe.ko
```

原始 `0xb89a8338` 初始窗口计算：TSF 模 512TU 小于 128TU 时，目标选下一个 128TU 边界，否则选下一个 512TU 边界；准备时刻比目标提前 2TU。`0xb89a826c` 与 `0xb89a7f70` 在单频配置下按 512TU 延续初始相位。实验候选强制使用原有的下一 512TU 分支。

无 iPad 的单板对照中，原件的三个 DW 准备相位为 126.027–126.028TU；候选为 510.027–510.029TU，周期 512TU、持续量 16TU 保持。两个端口时钟没有 128TU 偏差。板端启停与原模块恢复均通过。证据：`.private/artifacts/M5-NAN-TIMING-01/`、`.private/artifacts/M5-DW-PHASE-02/`。

双端观察中，该候选曾使真实 bootstrap 回复发送成功，并触发 iPad 的 PIN 输入回调及代码页；另一轮仍重试耗尽。两轮接收同步信标的时刻均在本地 DW 之外，尚未证实同簇同步。配对与失败计数见 [组件验收](../../../../docs/acceptance.md)。

## 已知限制

- 此候选仅用于当前 2.4 GHz 单频实验，不能作为 5 GHz/双频固件交付。
- 模块 `srcversion` 保持原值；候选身份使用现有固件下载取证及运行时相位确认。
- 本地起点对齐不能替代集群同步、真实 ACK、密钥保护或系统安全配对验收。

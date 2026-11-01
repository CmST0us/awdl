# 原生FAW候选刷新

原生func7在运行中加入日程记录后，不改动已预约的下一个无线事件。若新增FAW早于该事件，选窗器`0xb89ab278`随后仍可选中已错过的起点，使整个窗口在宣告时间之外执行。

`u2_faw_refresh.S`在既有NAN临界区内读取free-running时钟，与module2首候选的64位起点比较。错过起点时，对有效记录调用原生`0xb89ad6c0`重新计算，再用`0xb89ad420`重建三个候选，随后进入原生选窗器。未来候选保持原有路径。日程位图、周期、MACID与集群时钟映射均沿用原生记录。

补丁占240字节，位于4 KiB代码扩展的`0xb89bbca0`，可与[完整STOP](u2-nan-stop.md)和[逐帧取证](u2-beacon-trace.md)组合。从仓库根目录执行，输入为已生成的取证镜像，输出目录须不存在：

```sh
python3 hardware/rtl8852be/firmware/tools/u2_faw_refresh.py .private/beacon-trace/candidate.bin .private/faw-refresh
```

输出`candidate.bin`需嵌入驱动并构建模块。日志ID857的`func=247`记录刷新事件，`line`是旧候选起点低32位，`ret`是选窗时free-running时钟低32位；判定本身使用完整64位。

## 验证

单板实际命中过期分支，旧起点与当前时钟相差86008µs。刷新后两个FAW起点为512TU周期内16417/16420µs，终点32626/32629µs。管理MACID2的一帧165字节Publish经正式TX接口发送，提交前与硬件完成后的DW端口TSF为周期内18680/29674µs，整个区间位于共同窗口。实际DW端口为1；STOP、原模块及140项参数恢复通过。证据：`.private/artifacts/M5-FAW-REFRESH-01/`。

该结果验证本地NMI日程和MGQ窗口内发送；广播完成不提供对端ACK或安全配对证明。

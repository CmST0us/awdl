# U2 原生启动、日程与完整 STOP

`u2_nan_stop.S` 对齐启动时的TSF与硬件信标计时器，在完整 STOP 中解除管理 MACID 的固件 sleep/pause 门控，并使 DW/FAW 回调的有效性检查与 STOP 使用同一临界区。主机仍复用原生 module2 和 func7 日程，不接管无线定时器。

## 固件边界

- 原生NAN启动的端口复位在 `b89a6b7e` 保存PORT_CFG，清PORT_FUNC_EN/BRK_SETUP后复位TSF、清BCN_CNT_TMR，再恢复配置。保留TBTT_UPD_SHIFT_SEL；首个比较点与后续512TU周期使用同一时钟。端口地址由原生分支提供，不固定物理port。

- 完整 STOP 在原 func2 覆盖 G+122 前保存运行管理 MACID。原生成功值按 `b8e1454c` 判断，保留 dispatcher 的 H2C 状态转换。
- 原 STOP 取消定时器、释放 module0/1/2、清表成功后，调用 `b8979c0c(macid,0,0,36)`，只清固件贡献，保留 HOST 的 OR 贡献。
- DW release/end/initial、FAW begin/end、warmup 六个入口在同一原生临界区内检查模块有效性。定时器注册与取消仍使用原函数地址。
- DW release完成放行后，以module0当前端口调用原生 `b89a766c` TSF通知构造器，产生C2H7。通知与窗口回调、模块有效性和STOP共用临界区；主机据此持续发布服务。
- 准备及执行的记录键清零，MACID 设为 `0xffff`。清理完成后才返回原生成功状态。

临界区复用 `b89ba75c/b89ba7a0`，当前运行分支进入 ROM 的嵌套中断临界区。同步边界与跳转表已通过板端只读取证确认。

补丁代码 848 字节，放入代码段扩展的 4 KiB；镜像总长 329400 字节。原代码、数据段虚拟地址保持，加载摘要随新段长重算。

## 生成

需要 GNU MIPS binutils。仓库根目录执行，输出目录必须尚不存在：

```sh
python3 hardware/rtl8852be/firmware/tools/u2_nan_stop.py hardware/rtl8852be/firmware/u2/rtl8852be_fw.bin .private/nan-stop-build
```

当前 channel 6 验证组合还包含 [512 TU 起点修正](u2-dw-phase.md)：

```sh
mkdir -p .private/nan-stop
python3 hardware/rtl8852be/firmware/tools/u2fw.py patch hardware/rtl8852be/firmware/u2/rtl8852be_fw.bin \
  --offset 0x38458 --hex 00 -o .private/nan-stop/phase.bin
python3 hardware/rtl8852be/firmware/tools/u2_nan_stop.py .private/nan-stop/phase.bin .private/nan-stop/build
```

`candidate.bin` 是组合镜像；`patch.asm`、各入口跳板与 `build.json` 保存离线地址和长度。替换驱动内嵌数组时，同时将 `array_length_8852b_u2_nicce` 与头文件 extern 长度设为 329400。日程 HAL 的编译入口为 `hal_com_i.c`；按 [驱动构建](../../driver/BUILD.md) 重编实际依赖、刷新模块元信息并链接，最后反提取固件逐字节比对。

## 验证

启动对齐原型在port1、正式补丁在port0各取得连续3条同步TX，均在原生DW门控区间内；发现TX、STOP和140项原参数恢复通过。正式组合用时1.901秒，证据 `.private/artifacts/M5-BCN-START-05/`。

固定记录为 key 1、channel 6/20 MHz、512 TU 周期、16 TU 位时长、bitmap `02`，使用实际管理 MACID。INIT 依据 module0/1、空 module2/日程表及 G+122 归属准入，允许同一 owner 的 DW 正常持有门控。

| 场景 | 结果 |
| --- | --- |
| DW 开放并结束后 STOP | 模块、表、执行身份及固件/硬件门控清理通过 |
| 两次 FAW 开始/结束后 STOP | 起点相差 524288 µs，开放 16210/16211 µs；清理通过 |
| ADD 后、首次 FAW 前 STOP | 无 FAW 开始事件，清理通过 |
| FAW 开始后 STOP | 结束定时器取消，清理通过 |
| 最后一条 DEL 后 STOP | 清理通过 |
| 连续启停复用 MACID 2 | 三个场景通过 |

FAW 起点为周期内 16426 µs。板端单场景用时 2.668 秒，三个交错场景合计 5.740 秒；每轮原模块及 138 参数恢复一致。原始证据在 `.private/artifacts/M5-SCHED-CLEAN-04/`、`.private/artifacts/M5-SCHED-FAW-02/`、`.private/artifacts/M5-SCHED-STOP-CASES-01/`。

## 已知限制

- 仅覆盖单管理 MACID、单记录、2.4 GHz 的当前路径。多 MACID 日程需要独立的所有权与清理设计。
- 最后一条 DEL 只删除记录；完整撤销仍以 STOP 为边界。
- FAW 是实际本地日程原语。主机接入、对端共同窗口、BIGTK、系统安全配对和 NDI 数据验收独立完成。

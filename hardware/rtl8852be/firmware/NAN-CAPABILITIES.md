# 原生 NAN 无线能力映射

参考输入是 BE200 工程的 c108（108.45105872.0）及配套 `iwlwifi-stack-public release/core24.70:14867:362ada0a` 驱动。下表中的 Intel 行为来自驱动调用与 `fw/api/mac-cfg.h`、`mld/nan.c`、`mld/key.c` 的 ABI；RTL 行为来自本仓库正式源码与 U2 补丁。ABI 和本地测试不代表本轮空口执行。

| 能力 | BE200 c108 接口及驱动 | RTL8852BE 对应实现 | 验证边界 |
| --- | --- | --- | --- |
| 同步、DW、集群 | `NAN_CFG_CMD`、cluster/DW 通知，专用管理与广播 station | func0/1/2 双端口、原生合簇/C2H6、512TU 同步端口、DW 起点及启动 TSF 对齐 | iPad mini原生发现、缓存认证与数据连接通过 |
| 本机可用时隙 | `NAN_SCHEDULE_CMD` 将 availability bitmap 与 PHY/link 绑定；deferred 更新有完成通知 | func7/module2认证与数据独立记录，错过窗口重算、TSF跳变刷新双域锚点 | 512TU周期，认证16–32TU；数据提议30/32时隙，保留DW同步，暂停冲突发现信标 |
| 射频信道校准 | 信道切换前后保持与当前 PHY 配置匹配的校准状态 | 数据日程前对目标和认证信道执行完整RFK，class10/func2发送双槽双链路中心信道映射并用NAN完成ACK定序；U2原生FAW切换恢复对应IQK页，STOP清缓存 | 本地RFK/页映射/H2C顺序回归通过；5GHz/80MHz真机`ready=1`、IQK页6/155、双向2MiB业务通过 |
| peer/NDI 日程 | `NAN_PEER_CMD` 将 NMI station、committed DW、PHY/link/map 与 peer 位图关联 | 主机保留宽本地窗口，TX取对端committed交集；func8登记MACID，每owner保存预约上下文，切信道前暂停旧队列；认证结束后驻留数据频道接收 | 多段门控215次及数据预约优先203次读回一致；25%至81.25%交集与5GHz业务通过 |
| 管理 TX/RX | NAN station/queue；robust unicast 按接收接口地址分流 | PASN 与 Action 的 PHL 管理队列、cookie/WP 完成；软件 CCMP 校验与 vendor RX | 首次系统配对及缓存Auth3通过，管理MACID2与数据MACID3分离已验 |
| NDI 数据 | `NAN_DATA`、station/key、mac80211 数据路径；快速发送逐包使用当前 Cluster ID | `aware%d`、PHL DATA/TID0/PCIe 队列、三地址 QoS MPDU、硬件CCMP；验证后送 Linux 网络栈 | 单peer/NDP、64帧在途、HT/VHT/HE、硬件解密与双向BA协商通过；大聚合与BA恢复见[组件验证](../../../docs/acceptance.md) |
| GTK 与 RX | NAN_DATA 的 multicast station/key 归属 | GTK TX PN 由 `wa_ndi_send` 分配，owner3 查询同一计数器；RX 使用 peer GTK、KeyID、MIC 和17个重放域 | NAN filter 保留 multicast 接纳；未建立 NDI 不交付组数据 |
| IGTK/BIGTK | key 命令携带 TX 序号；NAN 管理/广播 station 区分 key owner | 本机 IGTK 主机 BIP；BIGTK 在 U2 原生信标准备时签名，双端口共用 IPN；owner2 查询实际固件快照 | MIPS16 签名/PN/完成/清理测试通过；对端信标先行接纳限制见下文 |
| STOP | flush station TX queues，删除 NAN station 与配置 | 原生临界区取消 DW/FAW、解除管理和附属门控；完成后清 BIGTK owner；NDI 先停止准入、排空引用再删除 | STOP 失败保留资源归属；不得以命令提交替代完成 |

## 接收与首包

NAN RX filter 使用正常接收模式，保留 multicast、broadcast 和本机单播接纳，关闭普通 STA 的 CAM 匹配前置条件与信标规则。NDI 入口验证 peer NDI、当前 Cluster ID、TK/GTK、KeyID、MIC 和接收 PN 后才交付网络栈。PASN 入站日志 `pasn-rx` 位于 vendor 分发之前，记录硬件 freerun counter、port、MACID、CAM 有效位、A1 命中、MAC 序号与认证 transaction/status，不记录认证载荷。

Auth3 观察需关联同轮对端发送、`pasn-rx`、vendor RX 与主机协议结果。没有 `pasn-rx` 只能说明该采集点未见帧；还需连续内核采集和其他接收帧证明采集有效。若已见 transaction3 而主机未接纳，检查地址和协议上下文；若对端重复发送但本端无帧，结合实际 DW/FAW、端口与 MACID 门控区分接收窗口和硬件准入。multicast 接纳变化不解释单播 Auth3。

首包通过必须同时具备安全 NDP 完成、对应 NDI/key/FAW 归属、驱动 TX 完成/RX、iPad实际WiFiAware路径及双方 TCP7777 HELLO/ACK。GTK PN 的零值仅在该 owner 尚未消费组数据 PN 时有效；首个组帧消耗1，查询必须随实际发送推进。BIGTK 查询取共享 latest-consumed IPN，包含已分配但尚未完成的信标。

## 候选重建

在仓库根目录运行，输出目录须不存在。工具链与单步期限见 [开发说明](DEVELOP.md)。`build_nan.py` 依次组合 DW 起点、完整 STOP、原生信标入口、FAW refresh、BIGTK/成员门控和 FAW anchor，输出341688字节镜像。正式镜像保留信标签名、门控与完成回调，省去逐信标诊断日志调用；`u2_beacon_trace.py`独立构建保留诊断输出。

```sh
python3 hardware/rtl8852be/firmware/tools/build_nan.py --output .build/u2-firmware
```

公共[驱动构建入口](../driver/BUILD.md)完成组合、嵌入、Kbuild 以及模块验证。嵌入同步更新数组、头文件声明和下载长度，内容相等时保持 C/H 修改时间。模块验证直接比较所有固件字节，并要求下载长度与镜像长度一致。

## 已知限制

- 对端 BIGTK 的主机验证尚未门控 U2 先行处理的集群信标；完整信标接纳保护未闭合。
- 当前单peer、双频道FAW不等价于Intel多PHY/link、deferred schedule或ULW；动态交集和日程切换仍影响有效吞吐。
- 5GHz HT/VHT/HE、自动速率与BlockAck/A-MPDU已接入，16MiB双向正文已通过；持续传输仍有停顿，90MB/s与300秒稳定性未验收。速率、恢复及失败计数见[组件验证](../../../docs/acceptance.md)。

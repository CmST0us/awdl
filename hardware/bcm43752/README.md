# BCM43752 A2 / AP6275P

当前实现包括固定来源的 DHD 驱动修复、NAN 编译接入、只读分析工具，以及 MAC、信标、主机日程、NDI、邻居和 NDL/NDP 生命周期的离线固件补丁，**已通过 iPad mini 首次 PIN 配对、缓存身份认证和安全 NDP，1 GiB 单／四连接下载与 256 MiB 上传已通过内容校验，长期稳定性和性能目标仍未通过**。`hardware/catalog.rs` 已加入设备匹配，公共服务选择 `bcm_nan`；构建入口已接入，正式驱动安装仍拒绝未经实机验收与授权核对的交付。公共 Rust 服务、hostap、文件服务和 iOS 客户端保持单份。

## 硬件与固件

已确认的设备为 PCI `14e4:449d`、芯片 `0xaae8` revision 2、模组 AP6275P；板级内核与恢复约束见 [RK3568 平台](../../platforms/pro-rk3568/README.md)。其固件内部型号为 `4362a2`，不能与 `4362b0` 或 BCM4389 混用。

| 输入 | 固定标识 |
| --- | --- |
| 原装 RAM 固件 | `fw_bcm43752a2_pcie_ag.bin`，936,074 字节 |
| 版本 | `18.35.387.23.146 (g412cc5ec)`，FWID `01-93c53be6` |
| 固件 SHA-256 | `6a2dbe01e72221defba91a52e158768d973a3c85ca2d881c924379e35ad36b23` |
| CLM SHA-256 | `d81ba8adc8b3bc0a5589146b31bc318122693d8a116a5028d3fe826556c5a92a` |
| AP6275P NVRAM SHA-256 | `b4780f7b86a5680dbe496815dbe07aa22010701de2d8c60570b8274a42db215b` |
| DHD | `101.10.591.52.27 (20240409-1)(20240411-2)` |

目标板原装固件未修改，镜像也未作为源码或交付包提交。补丁候选在 `.build` 生成，并从独立固件目录临时装载。原装固件、CLM、板级 NVRAM 与驱动必须一起保留可恢复副本；后续交付仍须保留原始固件授权，不能把驱动的 GPL 授权用于固件。参考固件和展开源码仅放在 `.build`，运行时 ROM/RAM、设备标识与日志放在 `.private`。

## 驱动实现

来源为 [Rockchip Linux kernel](https://github.com/rockchip-linux/kernel/tree/77168c8d5ab82399f65a80e9f807b50ba37cf483/drivers/net/wireless/rockchip_wlan/rkwifi/bcmdhd)，固定提交与路径由 `driver/source.json` 管理。`prepare.py` 从该提交导出源码，再按 `driver/patches/series` 应用补丁，不使用工作树的未提交内容。

| 补丁 | 行为 |
| --- | --- |
| `0001` | 修正外置模块使用绝对 `M=` 路径时的头文件路径。 |
| `0002` | 加载时完成 PCIe 上电与固件初始化；向模块加载器返回实际初始化错误，避免失败清理后仍报告成功而再次卸载。 |
| `0003` | 只向 BCM43752 A2 开放每次 1–4,096 字节的 ROM/RAM 读取；拒绝 SET、短参数、非法范围与其他芯片。 |
| `0004` | NMI 地址改用 `DEV_ADDR_SET`，维护内核地址索引；PCIe PHY 绑定真实 PCI 父设备。 |
| `0005` | 编入项目自有 `wl_nan_host.c`：专用 vendor 命令转发原生 NAN GET／SET，启用 158／159 事件掩码并封装真实 NAN 事件，拒绝 Android 自主 NAN 控制。 |
| `0006` | 编入 `wl_nan_ndi.c`：按 MAC、固件 ifidx／bssidx 匹配 NDI 事件；承载原生以太网数据接口，排空 PCIe 发送环，保留失败和迟到完成的资源记录。 |
| `0007` | BCM43752 A2 使用 NAN 专用命令入口；禁用 STA/AP/P2P/monitor 和额外接口，主控制节点不承载数据。 |
| `0008` | 保留 firmware-class 相对目录，避免同名原装固件覆盖候选选择；不编入原厂 ESCAN、AP+STA 与 NAN+P2P 扩展。 |
| `0009` | 原生 NDI 保留 TCP socket 发送记账直到 PCIe TX 完成，排除发送入口及总线提交前的提前 orphan；采用约 8 ms 的无线 pacing 额度。 |

`CONFIG_BCMDHD_NAN=y` 可编译现有 `wl_cfgnan` 的发现、NDP 及数据接口代码。新增补丁拒绝 Android vendor NAN 命令；项目 hostap 后端已注册。`0007` 不创建原厂 `aware_nmi0`、P2P 或静态附加接口；仅保留物理 vendor 命令节点。该节点继承 Linux managed 类型，但无扫描、关联、AP、P2P、monitor 操作，主接口数据发送及其他无线 IOCTL 被拒绝；私有 IOCTL 仅保留有权限检查和地址边界的只读 ROM/RAM 诊断。NMI／NDI 的真实角色由固件持有。不能仅修改接口名称、类型或能力位来声称已完成接入。

`wl_nan_host.c/h`、`wl_nan_ndi.c/h` 与 `include/nan_vendor_abi.h` 是项目自有源码，由准备工具复制到展开驱动；`nan_host_abi.h` 同时供固件和驱动使用，不手工维护副本。Linux 通道使用 Broadcom OUI `0x001018`、subcommand `0x5741`。GET／SET 每次只提交一条原生 version-2 命令，响应保留实际 IOVAR 错误；SET 不回显输入，临时密钥缓冲区清零。WLC critical／non-critical 事件保留 type、reason、status、接口索引和原始载荷；reason 128 额外检查主机帧格式，实际 TX 完成与命令接受分离。该通道已完成实机初始化、启停、NDI 及管理帧往返；iPad mini 安全 NDP 和双向业务数据内容校验已有实机证据，完整稳定性仍未通过。

NDI 操作使用同一通道的 operation 3，提供 CREATE、DELETE、QUERY、QUIESCE、RESUME；字段、单位与结果语义以唯一的 [ABI 头文件](include/nan_vendor_abi.h) 为准。两个所有者记录随 wiphy 存活，hostap 退出和命令超时不清空记录。创建响应不能替代 IF_ADD；只有匹配的原生 IF_ADD 才允许注册 Linux 接口，IF_DEL 和实际主机释放共同决定删除完成。匹配事件唤醒工作线程执行注册／注销并发送完成事件，QUERY 仅读取状态；RTNL 占用、在途发送和实际错误均保留所有者。

沿用现有 RTL 后端的接口结构：NMI 由原生 NAN BSS 持有，NDI 为不带 `wireless_dev` 的以太网接口，复用 DHD PCIe 收发路径。Linux 6.1 的 NAN wdev 不承载 netdev，不能把 NDI 伪装为这种接口。NDI 不经过 STA 的 MAC 改写、扫描扩展、关联控制或 AP／P2P 配置；公共无线后端要求唯一控制节点；hostap 核对实际 wiphy 的模式、命令和唯一 vendor 入口后才允许初始化。

关闭数据路径先 QUIESCE：停止新发送，分别等待主机 TX 和固件发送环完成，每阶段上限 5 秒且仅作为失败兜底。创建中的发送环在真实创建完成后继续删除；删除提交失败或固件拒绝保留活跃环，允许显式重试。之后由 hostap 释放站点、密钥和 NDP，再 DELETE 原生 NDI。PCIe 创建／删除响应另发完成事件，供主机处理迟到释放。NDI 注册失败不会让内核析构回调提前释放仍被 DHD 引用的内存；实际注销走以太网路径，避开空 `wireless_dev` 解引用。固件重启后应重建驱动实例，不能沿用旧索引记录。

`source.json` 的字段均为必填：`repository` 是上游 Git URL；`commit` 是完整提交 ID；`directory` 是该提交内的源码相对目录；`version` 为驱动版本标识；`license` 说明源码原始授权及保留文件声明的要求；`patch_series` 相对清单目录定位有序补丁列表；`archive_sha256` 固定该提交及子目录的 `git archive --format=tar` 字节。该文件变更在下次展开源码时生效。

准备源码需要 Python 3.12+、Git，以及包含固定提交的本地上游仓库：

```sh
python3 hardware/bcm43752/prepare.py \
  --source-checkout .build/bcm43752/rockchip-kernel \
  --output .build/bcm43752/driver
```

`--output` 必填且须在本项目 `.build` 内；只复用输入一致的已登记产物，输入变化时重建本工具产物，拒绝替换未登记目录。`--source-checkout` 与 `--archive` 互斥，分别读取固定 Git 提交或匹配摘要的 tar；都省略时读取 `.build/downloads/bcm43752-dhd-77168c8d.tar`。工具不安装或加载驱动。公共入口为 `prepare-source bcm43752`／`build-driver bcm43752 --platform pro-rk3568`，目标内核版本与头文件必须匹配设备。

## 公共 hostap 的硬件接入

`hostap/` 实现专用 vendor netlink 通道、实际 wiphy 能力与监管信息读取、A2 原生事件解析、管理帧与完成 cookie、NMI／NDI 站点及密钥所有权、NDI 状态核对，以及 8192 TU／16 TU 调度格式转换。公共补丁 `0015` 传递协商得到的 NDP ID，硬件后端不另造编号。保护帧使用固件的 CCMP／GCMP／BIP 实现。重复密钥安装不重置 PN，发送超时不释放仍由固件持有的 cookie，命令错误保留实际原生返回值。

11 个模块已在目标 ARM64 系统编译、链接到同一份公共 hostap，包含 `bcm_nan`、`rtl_nan` 和 `nl80211`。启动按真实事件推进 NMI、双频发现、管理帧完成及 NDI 生命周期；调度和密钥连接公共协议栈。频率为零的组播发现沿用当前 DW；异步单播配对／NDP 应答按对端最近实际定向请求的发现信道发送，避免被其他频段的 DW／发送完成事件改道。定向信道提示最多保存 16 个对端，停止时清空，既不改变日程也不创建额外无线模式。实机双频 DW 通知、管理帧完成、配对及安全 NDP 已验证；已取得大文件双向传输及速度结果，稳定性和性能目标仍未通过。

创建数据站点前等待同对端管理帧的真实发送完成，避免 NDP 完成与定向发现响应竞态。取消返回在途或记录已完成时仍保留主机 cookie，只有 TX 完成事件释放它；未到事件则报告超时。失败 ADD 的 REMOVE 只有在实际 SCB／NDP 表证明没有对应资源时才返回成功，已有资源仍沿原生释放流程处理。同 NMI 再认证的新 NM-TK 在管理帧排空后移除旧管理密钥并安装，独立 NDI 密钥保留；完全重复安装不重置 PN，同密钥材料而 PN 改变仍拒绝。

持久 NDI 在清理前 QUIESCE，站点和密钥释放成功后 RESUME；仅在实际发送与环资源已排空、接口身份未改变时恢复。停止失败保留资源并报告错误，不把命令接受当作完成。调度目前接受立即生效的单无线电日程；活动 NDP 的 deferred 本地变更及非空 ULW 尚未实现。

NDI 的 `get_inact_sec` 经固件 GET 查询真实 SCB 最近收包时间，接入公共 hostap 的 NDL 空闲回收策略；查询错误不会伪造超时。删除日程前排空该对端管理帧，站点、密钥和日程仍按实际释放结果更新所有权，持久配对文件不随空闲会话清理。

可在 Linux 下执行 `make -C hardware/bcm43752/hostap UP=<公共hostap源码绝对路径> BUILD=<.build内输出绝对路径>`。`UP` 指已应用公共补丁序列的唯一 hostap 源码，默认 `.build/targets/aarch64-unknown-linux-gnu/hostap`；`BUILD` 为可重建产物目录，默认 `.build/bcm43752/hostap`。`CC`／`AR` 可指定目标编译器与归档器，省略时使用 Make 默认值；公共源码与编译器必须属于同一目标架构。

## 固件缺口

2026-11-07 的原装及启用 NAN 编译的驱动均读到同一固件版本。`cap` 没有 `nan`，按真实 batch ABI 构造的 `nan version` GET 返回 `EOPNOTSUPP`（errno 95）。这不是裸 iovar 参数格式错误，也不能通过主机侧编译开关补齐。

A2 实机 ROM 保留 NAN 状态机定时器、可用时隙和密钥派生代码。ROM 中 `nan_fsm_timer` 位于 `0x1575fa`，NAN 密钥派生字符串位于 `0x1577c6`、`0x1577d3`。原装 RAM 固件的 333 条 Flashpatch 记录位于 `[0x253d24, 0x25478c)`，每条为 8 字节目标地址/补丁数据地址；所有记录均与实机 ROM 对应字节吻合。

但 ROM 代码依赖的多处 RAM 回调没有实现。例如 `0x1f1e4` 调用 ROM 跳板 `0x151080`，跳板读取 RAM 槽 `0x1812f0`，其值为 `0x1a5689`。该 Thumb 入口打印 `UNIMPL: ra=%p` 并进入断言路径。分析工具对 52 个具体调用位置、50 个原装 UNIMPL 槽核对了同类调用链；这不是完整依赖图。ROM 中保留 NAN 代码并不表示完整 NAN 功能可直接启用。

[nexmon](https://github.com/seemoo-lab/nexmon/tree/68720406f54ff16cad28fd3c51f2392aefad9fb7) 的 Flashpatch 与 RAM hook 方法可供移植参考，但该固定来源未提供 BCM43752 A2 的现成端口。参考用的 Pixel Tablet `4362b0` Android 固件和符号表提供 NAN 初始化及注册结构；其初始化依赖 13 组子模块，地址、回调表和结构布局必须与 A2 逐项对应。该 B0 镜像未加载到设备，不能替换 A2 镜像或直接沿用其地址。

补丁实现帧上下文、核心及主机命令、MAC／信标、BSSCFG、可用时隙、NDI／邻居／站点、密钥和管理帧完成路径。公共 hostap 独占发现服务、配对及 NDL/NDP 协商，固件自动响应、自动确认和自动删除邻居保持关闭；邻居容量为 1–32，运行中禁止修改容量。初始化失败、在途发送、实际 SCB 引用及未完成注销均保留资源所有权，不能用清空记录替代回收。

新增候选补齐八个 NDPE 回调槽及模块初始化／回收，以及十九个 DAM 社会信道列表／信道检查／NDC／时隙／提议响应、激活及对端 CRB 回调和接收解析、周期遍历、提议生成、网格扩容、反提议入口。NDPE 包含帧事件／属性映射、结果可变区、状态处理表和只读实现描述符；DAM 复用 A2 的 CLM、禁止信道列表、NDC 编码与释放，补齐列表容量错误处理、多个对端共享 NDC 的引用管理、512 槽引用更新、提议与响应选择、NDC 去重、日程扩容、外层提议／请求／响应、主机所有权互斥、日程激活／取消、引用释放和部分失败重试。52 KiB 补丁保留区的原生堆与 MPU 划分、ARM 构建及实际 A2 ROM 的离线调用通过；同镜像主机模式已完成启动、缓存认证、双向内容校验和活动 NDP 停止验收，不据此公告自主协议能力。仍有 3 个已检查的基础设施／STA 共存槽未恢复；自主 NDP 结束调用者的错误传播也未齐套，公共 hostap 的结束路径不经过该入口；固件自主协商和 STA 共存未启用，详见 [NDPE](../../docs/acceptance.md#bcm43752-ndpe-注册候选)与 [DAM 验证范围](../../docs/acceptance.md#bcm43752-dam-信道与-ndc-候选)。

固定镜像的离线执行与真机验收分别记录于[验收文档](../../docs/acceptance.md#bcm43752-a2-运行接入)，实现细节见[固件补丁](firmware/PATCHING.md)。iPad mini 已通过 PIN、缓存认证和安全 NDP；NDI 的 80 MHz BSS 激活、PCIe 发送环创建和正常停止已有实机证据。同 MAC 的 NMI／NDI 站点选择及 CCMP 密钥冲突已有修复。当前整套候选的 1 GiB 单／四连接下载为 36.90／41.29 MB/s，256 MiB 上传为 30.08 MB/s，三项内容哈希均一致；首轮单连接曾停滞，原因未确定。原生活动查询已接入公共 NDL 空闲回收，空闲后 SCB、NDP、对端日程和对端组密钥实际释放，无须重启服务。吞吐仍低于 BE200，不能认定长期稳定性或性能目标完成。固件自主 FSM／DAM 的剩余回调尚未全部实现，当前没有可交付的完整 NAN 固件。精确产物及失败边界见[当前验收](../../docs/acceptance.md#bcm43752-整套候选真机验收)。

## 性能配置

采用 BE200 已有方案，在一次验收前同时启用 149 主信道／80 MHz、完整 512 槽数据可用日程、原生对端能力交集及速率适配、原生聚合、NDI 硬件 CCMP、PCIe 数据环与内核 GRO。NMI 管理帧的软件密码处理不用于数据载荷。公共文件服务和 iPad 的 Bulk 传输逻辑沿用已维护的同一份实现。

原生 NDI 的发送 skb 保留 TCP 内存记账，直到实际 PCIe TX 完成或真实错误释放；原发送入口和总线投递前的提前 `skb_orphan` 会使 TCP Small Queues 看不到在途积压。发送采用 `sk_pacing_shift=7`（约 8 ms 额度），为聚合保留空间。平台为新建接口选择 `fq`，按 TCP 提供的节奏出队。

RK3568 平台同时安装 CPU performance 和 TCP 收发自动调节 32 MiB 上限。实际 PCIe 为 2.5 GT/s ×1，与设备上报能力一致；驱动未支持的 IPv6 checksum／TSO／scatter-gather 不强行打开。单连接、四连接和上传使用同一配置统一验收，测试中不按速率结果逐项调参。本地策略的 32 槽按 512 TU 模式重复到完整 8192 TU 周期，对端日程保留全周期，避免不同 DW 的可用窗口被短周期交集丢弃。配置齐套不表示已达到芯片吞吐上限，实测速率和已知限制见验收文档。

## 只读分析工具

ROM 可读区间为 `[0, 0x160000)`，RAM 为 `[0x170000, 0x2b0000)`，均以字节计。**禁止读取 `[0x160000, 0x170000)`**：初版快照包含该未映射区，随后实机报告 AXI decode/AER 错误并触发固件 TRAP。最终驱动和读取工具均排除此区间。读内存需要特权，不是生产数据通道。

在已取得共享设备租约、停止接口管理者且该 PHY 全部接口 DOWN 后，以 root 运行：

```sh
python3 hardware/bcm43752/firmware/read_memory.py \
  --interface wlan0 \
  --firmware /lib/firmware/fw_bcm43752a2_pcie_ag.bin \
  --output /var/lib/wifi-aware-adaptation/bcm43752/snapshot \
  --lease-fd 9
```

`--interface` 是现有接口名，限 1–15 个 ASCII 字节；`--firmware` 为固定原装固件文件，用 SHA-256 拒绝其他版本；`--output` 为尚不存在的私有目录，父目录必须已存在。这三个参数必填。`--lease-fd` 为可选的、已继承共享租约的文件描述符；省略时工具非阻塞获取 `/run/lock/wifi-aware-device.lock`，占用即失败。工具不启动或切换任何无线模式，只读取版本、能力、NAN version 和内存，输出权限为 0700/0600。

离线验证：

```sh
python3 hardware/bcm43752/firmware/inspect_firmware.py \
  .build/bcm43752/original/fw_bcm43752a2_pcie_ag.bin \
  --low-memory .private/new-radio/final-evidence/final-snapshot/low-memory.bin
```

固件路径必填。`--low-memory` 可省略；省略时仅检查固定固件及补丁表，提供时核对全部 Flashpatch、ROM 标记和未实现回调。旧 0x170000 字节快照只允许离线分析，读取工具不会重新读取其中的空洞。工具不改写固件。

## 验收边界

四个补丁已从固定提交干净重放；启用 NAN 的模块在 RK3568 对应内核头文件下原生构建通过。最终模块的加载、卸载、PCI 父设备绑定和受限快照结果按 [验收文档](../../docs/acceptance.md#bcm43752-a2-前置验证) 记录。编译、静态分析与原生 Wi-Fi Aware 验收是不同结论。

当前驱动包含九个有序补丁，DHD／hostap 控制通道已完成板端构建和实机初始化。已修复 NMI 空密钥槽清理、vendor 回复长度及末尾 netlink 对齐处理。原装启动文件保持不变；候选固件从独立目录临时装载。固件已完成 NAN／NDI 启动、正常停止、双频发现调度及信标提交；iPad mini 系统选择器已发现目标并完成 SAE／CCMP PIN 认证，缓存身份认证和安全 NDP 已通过，已取得大文件双向传输及速度结果，稳定性和性能目标仍未通过。当前镜像、构建与实机证据见[验收文档](../../docs/acceptance.md#bcm43752-a2-运行接入)。

正式功能验收使用已连接的 iPad mini（A17 Pro），保留其安装身份、配对记录和共享文件。固件与完整后端完成后，统一验证发现、首次配对、缓存身份、安全 NDP、双向内容校验、取消和重连，再做最终速度测试；不建立逐级性能基线。整个验收期间芯片必须独占原生 NAN，数据走原生 NDI，禁止 monitor、STA/AP/P2P 并发。

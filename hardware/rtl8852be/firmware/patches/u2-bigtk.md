# U2原生BIGTK

固件在原生信标准备入口生成81字节BIP-CMAC-128帧，保留原生63字节模板供集群字段更新。两个动态NAN端口使用独立签名副本，共享本地NMI的一把BIGTK及48位IPN。固件在队列锁内切换WD引用与长度，原生TX报告释放待发所有权。

同一时刻只允许一个端口持有待发帧，以保持共享IPN的发送顺序。另一个端口的准备事件会跳过；现有单板验证未发生跳过。没有待发签名时，原生门控不能重新使能旧帧。完整STOP完成后清除密钥上下文、IPN、缓冲区与公开快照。

## 固件与命令

`u2_bigtk.py`在FAW刷新候选上接入NAN dispatcher、原生准备、门控与TX完成入口。与FAW锚点组合使用16KiB代码扩展。状态784字节位于`b8e11ce8`，296字节只读表位于`b8e11ff8`。代码扩展区通过取指执行，数据常量放入数据区；编译工具将紧凑栈操作和返回指令展开，并将地址常量转换为立即数。

NAN H2C func8接收32字节：`op, KeyID, NMI[6], initialIPN[6], ports[2], key[16]`。op1安装KeyID6/7，同一次START内拒绝重复安装；op2按NMI/KeyID查询原子公开快照。查询时IPN/key字段不参与安装。op3以payload8的数量和payload9/10的MACID登记最多两个数据成员，payload11为允许尾部预算的数据MACID加1，0表示关闭；仅NDI数据密钥安装后设置。op4取得成员快照，op5探测该扩展能力。新成员保持关闭，已有成员保留实际门控；各自FAW记录驱动开启/关闭。结束timer携带该记录的MACID，避免下一窗口覆盖全局执行身份；切信道前暂停其他owner，目标在原生begin中开启。管理owner的相邻窗口按计数取并集。完整func2 STOP清除owner、计数和全部成员门控。

FAW切信道描述符位于`b8e16aac`，byte0/1/2为主信道、中心信道和带宽，byte6为RF频段。NAN hook在调用原生`b89a2c44`前按主信道设置2.4GHz或5GHz；原生BB函数`b899b76c`将该值用于模式、频偏和信道报告编码。认证窗口返回channel6时同步恢复band0。真机已验证161/155/80MHz协商、5GHz接收、双向业务和完整STOP。

FAW开始timer按管理/数据owner保存独立的信道、MACID、时长和记录键，回调执行前恢复对应描述符。同一owner的新预约注册成功后使旧代次失效；注册拒绝保留原预约；其他owner的准备不会覆盖当前回调。回调地址保持原生入口，使STOP沿用原生取消路径。

本机接收驻留与对端发送窗口分开执行。认证FAW结束后，按当前有效数据记录切到5GHz；数据MACID仍由共同位图控制。有效数据预约期间，周期性发现信标保持当前频道并继续签名发送，使新peer可发现并同步；DW同步端口继续运行。删除数据记录后恢复原生发现频道选择。切信道和门控均由固件事件推进。

公开快照204字节：前16字节为H2C序号、KeyID、NMI、IPN、pending端口加一、active；随后五个LE32为prepared、completed、skipped、error、CMAC耗时；末尾是两份84字节槽，各包含81字节签名信标。快照不含密钥，查询完成后保持到下一次查询或STOP。

驱动`mac wa_bigtk`调试命令接收八个十六进制LE32字，等待匹配的func8固件完成。func8载荷不写入H2C观察日志。生产vendor密钥接口通过`LOCAL_BIGTK_TX`能力接入本机发送owner。

## 构建与验证

```sh
python3 hardware/rtl8852be/firmware/tools/u2_bigtk.py <FAW刷新候选.bin> .private/bigtk-fw
python3 hardware/rtl8852be/firmware/tools/test_u2_bigtk.py .private/bigtk-owner-test \
  --beacon <原生63字节信标.bin> \
  --openssl /opt/homebrew/opt/openssl@3/bin/openssl
```

双端集群同步组合在此镜像上继续应用[FAW时钟锚点更新](u2-faw-anchor.md)，避免TSF跳变后的候选计算停滞。

目标指令测试覆盖共享IPN进位、独立OpenSSL MIC、重复安装拒绝、在途缓冲区保留、错误完成释放、队列失败、模板更新、PN耗尽、NDI能力探测、FAW成员绑定/解绑、相邻窗口begin/begin/end/end交错、开始回调描述符与过期代次、结束回调归属、本机接收/对端发送分离、发现信标与数据预约仲裁、切信道前暂停、5GHz/2.4GHz频段往返与STOP清钥。

板端使用候选模块与ARM64 `schedule-once`，持有设备独占权后运行：

```sh
sudo python3 test_u2_bigtk_board.py --output /tmp/bigtk-check \
  --program /tmp/schedule-once
```

工具使用公开RFC测试密钥；两个端口各取得两次原生TX后读取快照，立即STOP并检查密钥上下文及owner清零。活动失败兜底5秒，清理3秒；不涉及iPad。完成后沿模块装卸入口恢复原模块及参数。主机独立验证：

```sh
python3 hardware/rtl8852be/firmware/tools/verify_u2_bigtk_snapshot.py <snapshot.bin> \
  --openssl /opt/homebrew/opt/openssl@3/bin/openssl
```

实际结果为12次签名、12次原生完成，最新两端口IPN12/11，两个MIC均通过，计算355µs；STOP清钥、连续内核采集、原模块140参数恢复通过。证据`.private/artifacts/M5-BIGTK-INTEGRATION-04/`；累计失败记录见[组件验收](../../../../docs/acceptance.md)。

## 已知限制

当前只支持band0、mbssid0、两份原生63字节NAN信标及一把本地发送BIGTK。生产安装与GET_SEQ回调已接入；快照返回同一分配域的latest-consumed IPN，后续新签名严格大于该值。在途旧信标可能被新接收关联丢弃，不回退或重复使用IPN。对端BIGTK主机校验已接入，固件原生集群信标接纳尚未由该校验门控；密钥轮换待接入。

生产接口使用func8/op0探测固件能力，op1安装后以op2读回owner；STOP后驱动检查owner与密钥清零。`LOCAL_BIGTK_TX`只表示本机原生信标签名发送。正式主机启动验证见`.private/artifacts/M5-BIGTK-API-01/`。

## 日程取证

`u2_nan_gate_trace.py`生成单独的诊断镜像：marker249带序号模式记录MACID门控写入及硬件读回，marker245记录原生切频道调用的参数、返回值与TSF低32位。正式镜像不含这些探针。TSF为有符号日志字段，解析时恢复为32位无符号值；门控位0表示开放、3表示暂停；数据门控和切频道调用分开核对，不以TX门控开放代替射频驻留证明。

```sh
python3 hardware/rtl8852be/firmware/tools/u2_nan_gate_trace.py hardware/rtl8852be/firmware/u2/rtl8852be_fw_nan.bin .private/faw-trace --sequenced
python3 hardware/rtl8852be/firmware/tools/read_nan_schedule_trace.py <kernel.jsonl> .private/faw-trace/events.json
```

## 双信道 IQC 恢复

func8/op7 在主机RFK前请求无线占用。已有在途保护信标时阻止新准备，保留已有发送，待匹配完成报告后授予；无在途时直接授予。授予时关闭NAN发送门控并清除两组IQC有效位，通过NAN C2H func9发送四字节正文（请求H2C序号、状态、两个保留字节）。主机同时取得命令ACK及匹配授予后开始校准，1秒仅作授予失败兜底；占用期间原生NAN定时状态继续推进，但切信道、FAW发送开放与信标准备暂停。主机恢复源信道并提交IQC后用op8释放；校准或提交失败也执行释放，STOP清空占用状态。op6 的32字节载荷为：byte0=6，byte1=记录索引0/1，byte2=中心信道，byte3=带宽0/1/2，byte4=IQC页0/1，byte5–7保留；byte8起依次为两链使能、两链TX结果、两链RX结果，共六个LE32。两组均有效后才恢复；STOP清除两组记录，提交不完整不能激活恢复。

驱动分别采集目标与源 RFK 的实际寄存器结果，在 MCC 页映射通知后提交两组状态。固件按切信道描述符的中心信道和带宽匹配，原生切换返回后恢复 `0x8104` 页位、`0x8138/0x813c` 结果和 `0x8124` 使能，再由原生 FAW 开放发送。NAN调度中已定位的原生切换调用点使用该恢复；校准时暂停恢复，避免覆盖主机正在生成的结果。RF58、VBUF与LUT未由该 IQC 接口写入。

MIPS16测试覆盖半组禁用、信道/带宽匹配、双向切换、暂停及STOP；Pro窗口19组5GHz读回均匹配同轮目标校准，96MiB内容与精确恢复通过，持续性能验收待完成。

RFK占用op7在同一临界区更新公开快照的active与pending字节，再检查准入。驱动在固件拒绝后、op8释放前读取这两个字节，用对应命令ACK关联拒绝原因；未收到执行ACK时不能将快照视为本次结果。op2仍更新完整公开快照，ABI与状态大小保持不变。

op8同时取消尚未授予的排空请求；STOP清空排空与占用状态，迟到的信标完成报告不能重新授予。RFK等待状态和请求序号保存在状态尾部。C2H正文由原生内存池分配，发送器接管所有权；主机按adapter和命令序号接纳授予，保留早于ACK到达的事件。

## TSSI 双信道恢复

func8/op9，每个IQC记录分别携带两链TSSI校准状态。32字节正文的byte0–3为操作9、记录索引、中心信道、带宽；byte4–7为链、使能、TXAGC偏移、保留；byte8–23为四个PA对齐寄存器LE32；byte24–27为CCK DE、byte28–31为OFDM DE，均为已右对齐的10位值。原厂`halrf_tssi_set_efuse_to_de_8852b`将同一CCK值写入长/短字段，同一OFDM值写入六种带宽字段，因此两字完整表达该校准结果。

op9须在RFK占用期间匹配已安装的IQC信道记录。双链记录完整后，切信道恢复PA对齐、TSSI使能和TXAGC，再以掩码`003ff000`恢复两链CCK及OFDM DE；链地址步长`2000`。主机在目标/源RFK完成处采集，沿用原命令ACK和释放流程。

状态784字节、常量296字节，占用1080/1088；常量起点为状态基址加`310`，公开快照及FAW读回偏移保持。私有尾部记录切信道代次、延后开门次数、切换中/就绪状态、目标中心信道/宽度及待生效owner位图。BIGTK保留16字节原始密钥，在签名前展开轮密钥。正式镜像不含gate/channel/MMIO观测探针；历史TSSI候选完成1GiB43.919848MB/s及双信道校准读回，新门控提交边界待真机验证。

## 扩展代码与观测槽

当前带序号观测使用`u2_nan_gate_trace.py --sequenced`，门控256字节位于`b89be600`，切信道144字节位于`b89be700`；生成时拒绝覆盖已有代码。

BIGTK代码占用原门控/切信道观测槽时，可用`u2_relocate_nan_observation.py`保留成功基线的观测执行路径。该工具将门控移到`b89bdf00`、切信道观测移到`b89bde00`，保留`b89bdb00`处的528字节MMIO探针；参考镜像必须采用既有240字节门控安装代码和相同切信道观测代码。生成前检查完整目标槽为空，并以原地址重新汇编核对参考代码，再由链接器处理迁移后的内部调用。正式FAW锚点使用`b89be800`。

```sh
python3 hardware/rtl8852be/firmware/tools/u2_relocate_nan_observation.py \
  <待加观测的完整候选.bin> <成功观测基线.bin> <新输出目录>
```

此产物用于消融与时序取证。观测影响连接或吞吐时，须继续定位实际完成条件，不能把日志耗时作为正式时序机制。

## 同信道复用与窗口尾部预算

完整15字节信道描述符相同、双槽有效且上次原生调用成功时，恢复前经原厂BB读取入口`0xb8999f41`读回两链有效TSSI对齐、DE和使能状态。匹配则保留实时TSSI偏移和收敛状态，跳过34次重复TSSI写入；不匹配恢复记录。IQC页、使能及TX/RX仍恢复，原生切换调用、门控保护和完成回调保持，RFK及记录更新使复用失效。

已安装NDI数据密钥的FAW沿原生free-running结束期限注册尾部事件。正常窗口保持原厂`C610[31:24]`聚合时长；末2048µs按实际剩余时间向下取32µs单位、且不超过原值，短窗口立即应用剩余预算。原生end关闭门控，在途请求仍按原厂完成路径回收；最后一个重叠窗口结束时恢复原值。每个MACID独立计数，旧窗口end不能关闭仍有效的新窗口；预算沿用相交窗口中最晚截止时间。成员表和预算owner不变时不修改当前窗口；成员变更、RFK、STOP使旧回调失效。原生timer非零表示接纳，拒绝必须返回0；注册失败恢复原值。

原生切信道返回失败时，不发布就绪状态、不应用目标校准、不开放门控。成功后的校准恢复与owner提交保持串行。

STOP前固件marker250的line/ret分别报告寄存器匹配复用次数和尾部事件执行次数。公开快照偏移保持；清理保留私有尾部代次以拒绝迟到事件，其余状态清空。

构建时可设置`U2_BUILD_CONTAINER=<已有容器名>`复用已挂载仓库到`/work`且包含clang的构建容器；默认仍使用`flange-build:latest`逐次运行，命令结果记录实际编译入口。

尾部预算消融使用[`experiments/tail-budget-disabled.patch`](experiments/tail-budget-disabled.patch)，仅取消尾部注册，保留TSSI及窗口状态机。比较结果须同时给出原生门控开放比例与对端committed bitmap；不将不同时隙条件的吞吐差解释为策略净收益。

## 预约 owner 交接

FAW begin 的当前代次回调若因本地预约转交 NDI 或成员删除而失效，消费该 token 并沿原生选窗、预约入口继续事件链；不恢复失效描述符、不打开旧 MACID。过时代次和 STOP 后回调保持无副作用。日志 ID857 的 func249 携带 token 与继续结果，MIPS 回归覆盖交接、重复回调及 STOP。

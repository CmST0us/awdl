# NAN vendor 接口

唯一 ABI 定义是 [`include/wa_nan_uapi.h`](include/wa_nan_uapi.h)。命令使用 `NL80211_CMD_VENDOR`，属性置于 `NL80211_ATTR_VENDOR_DATA`；控制程序直接包含该头。当前ABI4，主机与驱动须同步更新。扩展属性和能力在现有编号之后追加。

`HE_CAP`返回21字节的5GHz HE SU能力：6字节MAC、11字节PHY、4字节≤80MHz RX/TX MCS映射，最多两条空间流。能力来自芯片原生cfg80211能力，NAN只发布已接入的SU数据能力；接收要求固定16µs包尾扩展。`NDI_CONFIG`增加HE开关、共同TX MCS映射、LDPC、GI/LTF选项及20/40/80MHz包尾扩展。主机检查对端可变长MCS/PPE字段，驱动先更新CMAC编码，再注册固件RA。TWT、触发发送、波束成形和160MHz未接入。

| 操作 | 当前行为 |
| --- | --- |
| GET_CAPS | 返回ABI、帧能力、NMI、原生当前cluster、当前频率及事件丢弃计数 |
| START | 接收上层完整 SDF，使用其A3作为初始cluster，建立原生 NAN 上下文，等待启用完成和首帧 TX completion |
| STOP | 关闭新TX入口，等待在途回调完成，完成固件停用与资源清理，恢复原角色/过滤/信道；失败保留残留并返回错误 |
| TX | 接收 Public NAN action/PASN，或显式请求单播 CCMP128 的 Protected Dual SDF/NAF 明文；异步返回同 cookie 的 TX_DONE |
| PEER_ADD | 创建单个远端 NMI 的 MACID/地址 CAM，保持 NAN 角色，等待对应固件完成 |
| PEER_DEL | 清理该 peer 的密钥及硬件角色，成功后回收软件 MACID |
| KEY_ADD / KEY_DEL | 缺省owner为pairwise CCMP-128；支持本机IGTK CAM与独立LOCAL_NMI_BIGTK原生签名owner |

peer 操作要求 START 已建立上下文，以及精确 6 字节合法单播 PEER_ADDR。该地址不能等于本机 NMI。同一已就绪 peer 重复 ADD 返回 0；不同的第二个 peer 返回 `ENOSPC`。没有 peer 时 DEL 返回 0；已存在其他 peer 时，错误地址的 DEL 返回 `ENOENT`。硬件创建失败保留分配，同地址再次 ADD 返回 `EIO`，由 DEL 执行清理。

缺省或 `WA_NAN_KEY_OWNER_PAIRWISE=0` 的密钥操作要求匹配的 peer 已创建，cipher `0x000fac04`、index 0。安装需要精确16字节 KEY_DATA，删除禁止 KEY_DATA。KEY_ADD 成功表示通用 SEC_CAM 与地址 CAM 操作完成；重复安装返回 `EEXIST`。安装结果不确定时再次安装返回 `EIO`，避免隐式重装密钥。KEY_DEL 在没有安装尝试时返回 0；失败保留待清理状态，peer 删除也必须先成功清理密钥。临时密钥副本在调用后清零，事件和日志不携带密钥。

本机 IGTK 使用 `KEY_OWNER=WA_NAN_KEY_OWNER_LOCAL_NMI_IGTK`（1），不接受 PEER_ADDR。KEY_CIPHER 必须为 CMAC-128 `0x000fac06`，KEY_INDEX 为4或5；ADD需要精确16字节KEY_DATA，DEL禁止KEY_DATA。KEY_OWNER属性号23，GET_CAPS能力 `WA_NAN_FEAT_LOCAL_IGTK_CAM` 为bit5，只表示CAM生命周期，不表示PN或管理帧保护。GET_CAPS无需START；本机IGTK操作需要已启用且就绪的NAN管理MACID，独立于远端peer。

最多一把本机IGTK活跃或待清理。同index重复ADD返回EEXIST；该index安装未完成时返回EIO；另一index的ADD返回ENOSPC、DEL返回ENOENT。无安装尝试的DEL返回0。held期间ADD返回EUCLEAN，但允许显式DEL。IGTK CAM入口不接收BIGTK或初始PN。原生BIGTK使用独立LOCAL_NMI_BIGTK owner和TX_PN分配下界，契约见[无线适配设计](../hostap/hostap/README.md)。IGTK安装不消费或回显初始PN；其软件BIP发送序号由主机唯一持有。

共享SEC_CAM路径在ADDR_CAM提交前保留全局条目占用，传播每次真实更新错误。删除时向硬件发送解除关联的映像，软件仍保留原key ID和SEC_CAM条目，直到ADDR_CAM解除与SEC_CAM清除均成功才释放。失败后显式DEL向同一身份重发幂等解除/清除；期间条目不可重用。ADD尚未获得关联槽即失败时，DEL不需要硬件清理。STOP先停发送/固件，再清理本机IGTK及peer密钥；任一删除失败保留管理MACID、角色和射频占用，返回非零/held。

NAN 角色和密钥操作按适配器、H2C 类别、功能及实际序号匹配固件 done ACK。命令直接提交，单ACK最多1秒，同一生命周期调用的固件等待共享2秒失败界限；固件拒绝或未完成不能报告成功。

## 单播管理 CCMP128

能力 `WA_NAN_FEAT_MGMT_PAIRWISE_CCMP128` 为 bit6。单 peer NMI、TK index0、CCMP128；此管理能力不表示NDI或旧TK的PN恢复。无 KEY_COOKIE 的既有 pairwise 安装仍为 CAM 原语。

KEY_ADD 附非零 `KEY_COOKIE`（属性25，u64）启用管理保护。调用方每次安装新协商 TK 时生成新 cookie，并保留至清理成功；不能以重新安装同一 TK 重置 PN。TX/RX PN 初值为0，首个发送消费1。KEY_DEL必须携带该实例cookie，错误cookie返回ESTALE；在途TX/RX引用返回EBUSY，不解除CAM。peerDEL传播同一错误。硬件删除失败保留身份与PN并禁止继续保护收发，显式DEL重试成功后清零密钥副本。

TX附 `TX_PROTECTION=WA_NAN_TX_PAIRWISE_CCMP128`（属性24，u32值1）、PEER_ADDR、KEY_INDEX=0和KEY_COOKIE。FRAME是24字节三地址管理头及category9 NAN SDF/NAF明文，输入Protected位为0、无CCMP头/MIC；必须未分片、源为本机NMI、目标为该peer。驱动设置Protected位、插入主机分配的8字节CCMP头，软件生成密文及8字节MIC，实际提交长度为输入+16，完整MPDU不得超过2304字节。PN在构帧/真实提交前原子消费，后续prepare、入队、通知或完成失败均不回滚；48位耗尽返回EOVERFLOW。同一MPDU硬件重传保持原PN。描述符绑定peer MACID，禁用硬件加密、自动IV与header转换。未显式请求保护的category9 TX返回EACCES；Public SDF/PASN不自动加密。

可信RX保留线上Protected位，去除CCMP头和MIC，body为验证后的明文；RX_FLAGS包含PROTECTED与CCMP_VERIFIED（bit6），清除RAW。事件必须携带PEER_ADDR、KEY_INDEX=0、KEY_COOKIE和 `RX_PN`（属性26，精确6字节小端）。控制端核对本机/peer地址及活跃key实例后才能交协议栈，不能通过清Protected位或HW_DEC标志建立信任。

RX在HAL DMA解析后、PHL station/core处理前验证。密文复用驱动AES-CCM解密与MIC比较；硬件已解密的payload仍独立重算加密MIC并与保留尾MIC比较。只有合法Protected Dual NAN SDF/NAF、匹配peer/key、MIC正确且PN严格大于已接收值才更新replay下界。并发验证在提交PN时再次比较。STOP在清理前关闭新TX和新保护RX，未完成引用不强制释放。

失败RX仅保留RAW诊断事件，不携带可信key元数据、不设置CCMP_VERIFIED；限频`ccmp-rx`记录counter、长度、原flags、结果、A2及可用key cookie，不记录密钥或正文。成功TX的netlink cookie与key cookie相互独立，TX_DONE仍使用原发送cookie。

## 生命周期

START 仅在启用命令按实际序号完成且首帧TX completion成功后返回0；首次管理帧使用内部cookie，`host-start`与`sdf-done`记录该cookie。发现信道在启用前设置为channel6，GET_CAPS的会话前频率不是日程证明。

初始cluster的范围为 `50:6f:9a:01:00:00` 至 `50:6f:9a:01:ff:ff`。主机每次START随机选择末两字节，驱动将同一A3写入两条原生模板与功能配置。独立时钟使用相同固定ID会绕过异簇合并；合簇由U2采用对端cluster/TSF。同步Publish和Follow-up响应使用请求A3，U2合簇完成产生C2H6，驱动以 `WA_NAN_EVENT_CLUSTER`（3）携带6字节 `CLUSTER_ID` 上报，主机更新hostap的PASN/NAF集群状态；STOP后忽略该事件。

原生SDF窗口放行后，固件通过既有C2H7通知DW时钟。`WA_NAN_EVENT_DW`（4）携带两个精确4字节属性：`DW_START` 为原生DW起点的TSF低32位，`RX_COUNTER` 为同一通知的free-run低32位。主机按counter去重，完整启动且发送队列空闲时发布当前服务；握手在途时跳过该窗口，不积压Publish。STOP后忽略窗口事件。

同一物理设备的普通扫描与 NAN 互斥。cfg80211 扫描/P2P ROC 准备及 PHL 已接纳请求（包括 PENDING）占用射频时，START 返回 `EBUSY`，不取消扫描、不等待。NAN 准备、活跃及 held 期间，新扫描/ROC 返回忙；内部扫描共用 PHL 接纳条件。扫描占用在真实完成/取消完成回调返回后释放，NAN 占用仅在完整清理后释放，不使用固定 scan-deny timer。该规则覆盖同设备的 wlan0/p2p0，不阻止另一物理设备独立扫描。

STOP 使用U2 func2的 `options=0` 完整停用分支；对应done ACK成功后才删除peer/key、管理MACID并释放额外端口，恢复原接收过滤、角色和信道。新TX在清理前关闭；已有请求等待回调释放所有权，最多1秒失败兜底，不强制回收仍在途的请求。空闲STOP返回0。

START失败调用相同清理流程。全部清理完成时返回原始失败码，允许重新START；残留未清完返回 `EUCLEAN`，保持占用并拒绝新的START与peer/key新增，STOP/显式删除仍可报告实际清理结果。固件只有一部分日程时，func2停用的双日程门槛可能拒绝；该情况保持held，不以释放主机标志或模块卸载报告STOP成功。`host-stop`记录返回值及残留资源，`host-start`记录原始结果、cookie和清理结果。

## 已知限制

- 当前为单peer NMI的CCMP128管理保护、本机IGTK CAM与原生BIGTK签名；NDI、真实发送PN查询与主机对端组管理校验已接入；旧TK的PN恢复未支持。
- CCMP管理收发已通过本地函数、OpenSSL整帧对照及真实iPad NIK交换；缺失字段或MIC不符时拒绝可信交付。
- 本机IGTK CAM能力不能解除完整组保护门控；原生BIGTK保护已接入，peer组钥由主机BIP维护，NDI GTK由数据owner维护。
- TX命令的netlink ACK仅表示接收提交。单播Action/PASN的TX_DONE事件按原驱动WP契约上报ACK_YES或ACK_NO；组地址与无ACK帧保持ACK_UNKNOWN。RX_RAW的HW_DEC不代表MIC、防重放或BIP验证通过。
- SET_SCHEDULE支持channel6/20MHz、512TU周期：认证记录固定16–32TU，数据记录承载双方共同的其余16TU时隙。附带peer时同步其NMI/NDI的FAW门控，相邻窗口按并集保持开放。 已创建peer及本机BIGTK就绪后可在pairwise密钥安装前配置该记录，供公开认证帧收发；这不放宽NDI创建或受保护管理帧的密钥要求。配置cluster不等于实际加入证明。
- 软件编译和模块链接不能证明硬件 peer 匹配、密钥使用或安全配对通过，仍需串行设备验证。

## 原生 NDI

`NDI`能力bit8要求固件func8/op5通过。`NDI_ADD`保留地址、GTK发送owner与独立`aware%d`网卡，`NDI_CONNECT`安装NDP TK及对端GTK/RSC后开放数据准入，两个命令回复真实`NDI_IFINDEX`。`NDI_CONFIG`为97字节八位组结构；包含HT/VHT交集、主/中心信道与带宽，末2字节为对端A-MPDU最小间距和最大长度指数，定义见`wa_nan_uapi.h`。`NDI_DEL`先关闭准入并排空工作/完成引用，再删除密钥及网卡；清理失败保留所有权并阻止卸载。

`GET_SEQ`要求`KEY_OWNER`、`KEY_INDEX`，回复6字节`TX_PN`。本机BIGTK owner2查询固件latest-consumed；NDI GTK owner3另带`NDI_ADDR`，读取驱动维护的实际GTK发送PN计数器。分配后失败不回退，未发送的NDI GTK序号为0。

本机NDI复用NMI地址。对端同址时复用peer MACID并使用最新NDP TK，不同址时建立独立数据peer。数据TX使用硬件CCMP及常规PHL/PCIe队列，最多64个MPDU在途；RX核对硬件MIC状态、密钥归属、逐TID PN与NDP地址关系。原始密文仍须通过软件MIC验证。单播按协商HT/VHT能力注册固件RA，收到对端有效数据后进行受保护BlockAck协商；A-MPDU遵循对端窗口、长度和密度。A-MSDU最多合并两个MSDU，接收端验证全部子帧后推进PN并交付。详细生命周期及芯片接收匹配见[无线适配设计](../hostap/hostap/README.md)。

`GET_CAPS`返回2.4/5GHz HT、5GHz VHT及当前监管域允许主动使用的20/40/80MHz组合。主机按共同日程构造Element Container，PHY信息参与既有NDP MIC计算；HE尚未启用。

## hostap 数据站点入口

能力 `WA_NAN_FEAT_NDI_STA`（bit10）表示支持 `NDI_STA_ADD`。该操作要求原生 NAN、已认证 NMI peer、管理密钥、日程及持久 NDI 就绪，接收现有 `NDI_CONFIG` 布局，但 `ndp_id` 和 `initiator` 必须全零；NDP 协议身份由 hostap 持有。站点资源、本地 GTK、CONNECT 的实际硬件配置和 DEL 的排空/回收语义与原 NDI 路径一致。

旧 `NDI_ADD` 仍要求有效 NDP ID 和发起方地址。`NDI_CONNECT` 必须匹配 ADD 或 STA_ADD 已保留的身份及本地 GTK；不能通过另一次 CONNECT 更换身份。新操作只追加命令和能力位，ABI4 的结构大小和原命令编号保持。

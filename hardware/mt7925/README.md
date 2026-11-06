# MT7925 原生 NAN 接入

MT7925 PCIe（`14c3:7925`）接入公共 Rust `wirelessd`、hostap 的 `nl80211` 后端和公共文件服务，不复制用户态协议栈。已完成 Q8B 的模块构建、安装、加载及 iPad Pro PIN 配对；正式模块与正常公共 hostap 已通过缓存身份认证、安全 NDP、IPv6 双向文件和服务重启后重连。

## 固定来源

| 输入 | 固定版本 | 用途 |
| --- | --- | --- |
| linux-firmware | `5a3f625f2e9d9f70134af2340d82af3f19ae6891` | WM `20261016233029`、MCU patch `20261016232915a`、原始授权与 WHENCE |
| wireless-next | `8db0381351fe6af82f81693d5085790e291b2f17`，Linux 7.3.0 | 同源 cfg80211、mac80211、MT76，包含首批 NAN 及 15 个运行／NDP 修复 |
| MT76 安全补丁 | `20260927210306.737669-1-sean.wang@kernel.org` | 应用其中 22 个补丁，保留 NAN 密钥及事件处理；DW-WTBL 路径由项目补丁 `0026` 修正 |
| 项目接收修复 | `hardware/be200/driver/patches/0003-mac80211-nan-address.patch` | 按接收地址分发 NAN 单播管理帧，复用既有实现 |
| Linux 7.0 适配 | `driver/backport/patches/series` | AES-CMAC / FILS 使用原内核 shash API，移除新内核专有的转发路径字段 |

安全补丁来自[作者原始邮件](https://lore.kernel.org/all/20260927210306.737669-1-sean.wang@kernel.org/)。补丁 18 仅刷新空行上下文。补丁 11 不在 series 中：固定基线的 `mt76_dma_cleanup()` 不负责提前禁用 RX NAPI，移除原有禁用会在卸载时触发 NAPI / page-pool 警告；保留原有禁用后，实机卸载无新增该警告。补丁 20 保留按实际分配检查 NAPI 的修复。

固定 WM 原始能力字节为 `0x80`（CNM），未声明 NAN。该公开镜像实机支持主机 NAN 命令、真实 DW_START / DW_END 和 iPad 服务发现；项目补丁 `0024` 仅在 MT7925 文件路径、1,390,712 字节大小、构建时间 `20261016233029` 和原始能力字节全部匹配时补全驱动 NAN 能力。准备与安装另核对固定 SHA-256，固件二进制不修改，其他版本不自动套用该修正。`inspect.py` 仍如实报告原始 `nan: false`。

补丁 `0025` 在 ENABLE 请求中传递初始 NMI，保持固件信标地址和 mac80211 管理地址一致。单独的 NMI setter 用于运行中更新，在 ENABLE 前调用会被此固件拒绝。未加入周期性伪造发现窗口、无条件能力覆盖或未验证的固件属性命令。

补丁 `0026` 修正未建立对端日程时的管理帧发送。固定固件下，DW-WTBL 路径的配对引导响应未获确认；改用接口 WTBL 后，iPad 收到响应并完成 PIN 输入和 PASN Auth2 校验。没有已提交日程的未加密管理帧使用接口 WTBL；已有固件日程的对端保留其 WTBL 和时隙约束，加密帧保留密钥对应的 WTBL。无用途的 DW 专用站点、固件索引命令和专用 TX 状态期限一并移除。该路径与下述 R4 信标规避共同完成 iPad 配对及数据链路接入。

补丁 `0027` 在 NAN ENABLE 前通过既有 CHIP_CONFIG 命令提交 `NanEnableR4Ie 0`，仅匹配 WM 版本 `____000000-20261016233029`。此固件自主生成的信标原本携带零 DCEA，与 hostap 服务发现帧中的配对／缓存能力冲突；关闭固件 R4 IE 后，信标保留 Master / Cluster 属性，公共 hostap 的服务发现帧继续提供 DCEA 和 NIRA。提交失败会中止 NAN 启动，命令的实际效果另经空口确认。固件文件不修改，无额外运行配置和公共协议栈分支。

这是对固定固件的规避：没有实现信标中 DCEA / NIRA 的动态写入，也没有禁止信标。原有主机发现窗口、认证、加密及 NDP 日程仍由实际事件推进。

补丁 `0028` 将 NAN_DATA 接入既有硬件 A-MSDU 配置入口。PCIe 声明硬件聚合后禁用了软件 A-MSDU，但原入口只接受 STA / AP，导致 NAN 数据缺少该聚合。修复沿用 mac80211 从 NMI 对端复制的聚合长度限制，并保留 ADDBA 拒绝 A-MSDU 时的关闭逻辑；不放大对端能力，不修改管理帧。当前 NAN PHY 使用 HT / VHT，不能将网卡的 HE / EHT 常规联网能力直接视为 NAN 已支持。

## 构建与安装

需要 Python 3.12+、curl、patch、Linux 内核构建依赖及与运行内核一致的 headers / 配置；检查压缩固件另需 zstd。展开前至少保留 8 GiB 可用空间。板端构建选择见 [Dragon Q8B](../../platforms/dragon-q8b/README.md)。

```sh
python3 hardware/mt7925/prepare.py
scripts/aware-build build-driver mt7925 --platform dragon-q8b \
  --kernel-release 7.0.11+ --headers /usr/src/linux-headers-7.0.11+
scripts/aware-build build-wirelessd
scripts/aware-build build-fileserver
scripts/aware-build install-driver mt7925 --country CN --destdir "$PWD/.build/stage-mt7925"
scripts/aware-build install-wirelessd --hardware mt7925 --pci 0006:01:00.0 \
  --destdir "$PWD/.build/stage-mt7925"
scripts/aware-build install-fileserver --destdir "$PWD/.build/stage-mt7925"
```

公共 hostap 继续按[公共构建入口](../../docs/build.md)构建。`prepare.py --modules-to` 是构建器使用的模块源码生成入口，路径必须位于 `.build`；无该参数时只准备固定输入。下载、源码和模块产物均为生成物，输入摘要变化时拒绝复用旧目录。保留诊断材料后，只移开对应可重建目录再准备。

安装交付七个模块：`cfg80211`、`mac80211`、`mt76`、`mt76-connac-lib`、`mt792x-lib`、`mt7925-common`、`mt7925e`。它们必须成套加载，不能与发行版旧无线栈混用。适配入口限定 Linux 7.0，实机范围为 Q8B `7.0.11+`；其他内核和其他无线驱动组合未验收。模块使用目标 headers 构建，未修改网络核心结构、板级驱动、设备树或启动镜像。

固件装到 `/lib/firmware/updates/mediatek/mt7925/`，保留发行版原文件；原始固件授权、WHENCE 及内核授权装到 `/usr/local/share/licenses/wifi-aware/mt7925/`。实际安装省略 `--destdir`，操作前须取得设备租约、备份程序和配置。安装本身不代表固件互通通过。

安装时必须按实际地区提供 `[mt7925].country` 或 `--country`，上述 CN 仅适用于中国。Q8B 的 CPU 与 TCP 性能策略使用 `install-platform --platform dragon-q8b` 独立安装。

`radio.rs` 只接管空闲 managed 接口，进入 NAN 后删除引导接口，运行时仅允许所选 PHY 的 NAN / NAN_DATA。`radio.toml` 选择 2437 MHz 单频发现，数据频率由公共运行配置决定。实际地区确认为中国后，安装输入使用 CN，通过服务启动前的 `iw reg set CN` 提交监管请求；`/etc/wirelessd/config.toml` 使用 `frequency = 5745`，在允许的 149 信道选择 80 MHz。MT7925 未实现 AWDL / AirDrop，相关申请返回明确错误。

## 真机验证与限制

2026-11-06，Q8B `7.0.11+` / GCC 13.3 完成包含 `0027` 的固定补丁重放、七模块编译、modpost、链接、暂存安装及实机加载。运行 GNU build-id 与安装文件一致，安装文件与暂存模块逐字一致；`mt7925_common` 的 build-id 为 `3686db7e58a0e87da7cd918b2111d329b1340c82`。固定 WM / MCU patch 的原始摘要不变。公共 Rust 服务 ARM64 Release、公共 hostap、已签名 AwareClient 和 AwareLab 的构建已通过。

测试设备为 iPad Pro 12.9 英寸第六代、iPadOS 27.0.1。首次 PIN 配对使用 R4 IE 关闭诊断版，系统显示“配件已配对”，完成 PASN、密钥安装、NIK 交换及持久授权，客户端保存 `MT7925 Q8B · ID 2`。随后恢复正常 hostap、正常 NMI 和正式七模块，保留同一配对完成下表验收。系统发现能将正式 NMI 识别为已有 ID 2；未删除既有记录重做首次 PIN。

| 验证 | 结果 |
| --- | --- |
| NAN / 发现窗口 | 合簇、真实 DW_START / DW_END、日程、NDI / IPv6 及 `_fileserver._tcp` 注册完成 |
| 缓存身份认证 | 配对窗口关闭，NMI 变化后仍使用 ID 2 完成 SAE 缓存认证；PASN Auth2 获 ACK，Auth3 验证通过 |
| 安全 NDP | 双端确认对端 NDI、IPv6 link-local 地址与 TCP 7777；板端会话映射完成 |
| 板端 → iPad | `alignment.txt`，1,048,576 字节，客户端显示完成；从 App 容器读回后 SHA-256 与板端一致 |
| iPad → 板端 | `00-upload-probe-merge.txt`，114,688 字节，客户端显示上传完成；板端 SHA-256 与 App 源文件一致 |
| 数据通道 | `ndi0` 抓到实际 TCP 7777 请求、文件正文、ACK 与 FIN，未使用以太网／AP 文件通道 |
| 客户端断开重连 | 重新连接并读取文件列表通过；App 退出后 NDP 会话实际释放 |
| 服务生命周期 | 停止后 NAN、NDI、hostap 与运行目录全部释放；正常重启后原有配对再次下载 1 MiB 文件成功 |
| 最终运行 | 正常 hostap，无私有 RAM 诊断接口；wirelessd / fileserver active，配对窗口关闭，保持原有 disabled 开机状态 |

下载 SHA-256 为 `8451de9f35d3a274d6c5a8d16a5022ad4c595db278e058590048b374d0adfabc`，上传 SHA-256 为 `d2785ea8554a930f3f5d9e42f6b4dbd2f5725cb0274f2cd2168d181ebacc5385`。这些是完整文件校验，不作为吞吐基准。

正式版本的 2437 MHz 空口样本包含 74 个发现信标与 14 个同步信标，全部仅有 Master / Cluster 属性，没有错误 DCEA；14 个服务发现帧保留 DCEA 和 NIRA。配对窗口关闭时 DCEA 原始两字节为 `02 00`（缓存能力）。对照版本的信标中该字段为 `00 00`。正式抓包 SHA-256 为 `549abc3907b0c2345a53aa4721b5439e6effcca5d5272d5653bcdab8d819ccfe`。

连续操作期间有一次缓存认证收到 Auth1 后未完成，服务报告认证期限失败。后续不重启服务的复测成功，Auth2 获 ACK、Auth3 验证完成；现有证据不能确定该单次超时的原因，不能报告长时间重复连接可靠性已通过。成功配对与正式传输样本没有再出现 DCEA 缓存位缺失或 iPad 对端缓存满错误。正式加载及验收区间没有内核崩溃或固件复位。驱动仍对缺少 NDC ID 的 TX GTK 安装返回 `EOPNOTSUPP` 并记录警告；本轮验证的是安全单播 TCP，未验收组播及其组密钥路径。

配对及小文件验收使用 2437 MHz；同一固定固件的 5 GHz 性能验收见下节。长时间可靠性与多对端并发尚未验收。原有 iPad 配对、板端身份和共享文件保留，以太网 SSH 可用；诊断源码、密钥和原始证据不进入 Git。

## 5 GHz 性能验收

2026-11-06，固定补丁链加入 `0028` 后，七模块完整重建、暂存与实机加载通过。运行和安装的 `mt7925_common` build-id 为 `5d1d1c3b1438b246a625edb94f739332b222b5d6`，其余六模块与前述版本一致。固定固件摘要不变。ARM64 安装工具和签名 AwareLab 验收程序构建通过；公共 fileserver、hostap 和 AwareClient 传输实现保持一致。

设备位于中国，global 与 MT7925 自管理监管域均为 CN，使用 5745 MHz 主信道 149、80 MHz、VHT 双流。板端提供完整 32 数据槽，iPad 日志确认 149 / 80 MHz 日程及 Infra 0（未关联普通 AP）。双方建立 TX / RX BA，硬件计数器确认 2–8 MSDU 的 A-MSDU 聚合。CPU 两个 policy 实测为 2.4384 / 2.9952 GHz，TCP 自动发送／接收上限均为 32 MiB。

下表三项验收使用同一优化包与相同运行配置，不包含 2.4 GHz 性能对照。速率取 App 原始端到端计时，十进制 MB/s；计入 TCP 建立、HTTP 与文件写入，上传包含完成回执，排除初始配对、发现、目录读取和完成后的内容比较。客户端使用 Bulk、256 KiB 读取及既有自适应 Range 分块。

| 方向 | 字节数 | TCP 连接数 | 秒 | MT7925 MB/s | BE200 已有 MB/s |
| --- | ---: | ---: | ---: | ---: | ---: |
| Q8B → iPad | 1,073,741,824 | 1 | 542.56 | 1.98 | 43.43 |
| Q8B → iPad | 1,073,741,824 | 4 | 335.73 | 3.20 | 49.37 |
| iPad → Q8B | 67,108,864 | 1 | 2.22 | 30.22 | 31.85 |

BE200 对照来自其已验收的 Rust Wi-Fi Aware 记录（原仓库 `docs/acceptance.md` 的“Rust 真机吞吐”），同一 iPad Pro、149 / 80 MHz、Bulk、相同文件大小与计时口径；不是 AWDL / AirDrop 成绩，也不是本轮重新测量。板卡与底层 PHY 不同，不能据此单独归因硬件性能差异。

两轮下载均从 iPad App 容器读回完整文件，与源文件逐字节相同，SHA-256 均为 `49bc20df15e412a64472421e13fe86ff1c5165e18b2afccf160d4dc19fe68a14`。上传在板端逐字节比较相同，SHA-256 为 `3b6a07d0d404fab4e23b6d34bc6696a6a312dd92821332385e5af7c01c421351`。单连接的首次 UI 观察器超过原有 90 秒期限；同一 App 传输和 TCP 连接保持运行，完成观察使用其原始计时，没有重启下载。四连接及上传的 UI 验收均通过。

上传达到 BE200 参考值约 95%，下载性能仍未达标。站点最高采样速率为 866.7 Mbit/s；单连接下载的拥塞窗口中位数为 8 MSS、末段采样累计 TCP 重传约 17.57 MB，四连接的窗口中位数为 3 MSS、末段采样合计重传约 73.62 MB。硬件聚合已工作，高 PHY 速率未转化为下载吞吐；重传的驱动／固件／对端根因尚未确定，不能将本轮报告为最优性能或达到 BE200 水平。验收期间没有新增固件复位、断言、Oops 或内核 WARNING。

客户端退出后 NDP 数据站点释放；wirelessd / fileserver 保持 active、开机状态仍为 disabled，配对窗口关闭，原有配对及共享文件保留。长期可靠性、AP 并发及多对端传输不在本轮验收范围。

## 来源清单字段

`driver/source.json` 的 `repository`、`commit` 固定仓库及提交，`version` 是内核版本，`file` 是归档名，`url` 是固定下载地址，`sha256` 是摘要，`directory` 是归档顶层目录，`license` 描述授权，`patch_series` 相对于 driver 目录并按非空、非注释行有序应用。字段必填，无自动替代。

`firmware/source.json` 的 `repository`、`commit` 固定仓库及提交；`version` 为 WM 构建时间；`required_features` 是无单位能力掩码整数，当前 128（要求原始 CNM 位；NAN 版本修正规则见上文）；`license` 指原始授权。`files` 每项的 `file`、`upstream_path`、`sha256` 分别是文件名、提交内相对路径和摘要。字段必填，不以最新版本补缺。

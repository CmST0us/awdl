# MT7925 原生 NAN 接入

MT7925 PCIe（`14c3:7925`）接入公共 Rust `wirelessd`、hostap 的 `nl80211` 后端和公共文件服务，不复制用户态协议栈。已完成 Q8B 的模块构建、安装、加载及 iPad Pro 系统发现；**新配对仍在进入 PIN 前失败，安全 NDP 与数据传输尚未通过**。

## 固定来源

| 输入 | 固定版本 | 用途 |
| --- | --- | --- |
| linux-firmware | `5a3f625f2e9d9f70134af2340d82af3f19ae6891` | WM `20261016233029`、MCU patch `20261016232915a`、原始授权与 WHENCE |
| wireless-next | `8db0381351fe6af82f81693d5085790e291b2f17`，Linux 7.3.0 | 同源 cfg80211、mac80211、MT76，包含首批 NAN 及 15 个运行／NDP 修复 |
| MT76 安全补丁 | `20260927210306.737669-1-sean.wang@kernel.org` | 应用其中 22 个补丁，保留 NAN 密钥、管理帧时隙及事件处理 |
| 项目接收修复 | `hardware/be200/driver/patches/0003-mac80211-nan-address.patch` | 按接收地址分发 NAN 单播管理帧，复用既有实现 |
| Linux 7.0 适配 | `driver/backport/patches/series` | AES-CMAC / FILS 使用原内核 shash API，移除新内核专有的转发路径字段 |

安全补丁来自[作者原始邮件](https://lore.kernel.org/all/20260927210306.737669-1-sean.wang@kernel.org/)。补丁 18 仅刷新空行上下文。补丁 11 不在 series 中：固定基线的 `mt76_dma_cleanup()` 不负责提前禁用 RX NAPI，移除原有禁用会在卸载时触发 NAPI / page-pool 警告；保留原有禁用后，实机卸载无新增该警告。补丁 20 保留按实际分配检查 NAPI 的修复。

固定 WM 原始能力字节为 `0x80`（CNM），未声明 NAN。该公开镜像实机支持主机 NAN 命令、真实 DW_START / DW_END 和 iPad 服务发现；项目补丁 `0024` 仅在 MT7925 文件路径、1,390,712 字节大小、构建时间 `20261016233029` 和原始能力字节全部匹配时补全驱动 NAN 能力。准备与安装另核对固定 SHA-256，固件二进制不修改，其他版本不自动套用该修正。`inspect.py` 仍如实报告原始 `nan: false`。

补丁 `0025` 在 ENABLE 请求中传递初始 NMI，保持固件信标地址和 mac80211 管理地址一致。单独的 NMI setter 用于运行中更新，在 ENABLE 前调用会被此固件拒绝。未加入周期性伪造发现窗口、无条件能力覆盖或未验证的固件属性命令。

## 构建与安装

需要 Python 3.12+、curl、patch、Linux 内核构建依赖及与运行内核一致的 headers / 配置；检查压缩固件另需 zstd。展开前至少保留 8 GiB 可用空间。板端构建选择见 [Dragon Q8B](../../platforms/dragon-q8b/README.md)。

```sh
python3 hardware/mt7925/prepare.py
scripts/aware-build build-driver mt7925 --platform dragon-q8b \
  --kernel-release 7.0.11+ --headers /usr/src/linux-headers-7.0.11+
scripts/aware-build build-wirelessd
scripts/aware-build build-fileserver
scripts/aware-build install-driver mt7925 --destdir "$PWD/.build/stage-mt7925"
scripts/aware-build install-wirelessd --hardware mt7925 --pci 0006:01:00.0 \
  --destdir "$PWD/.build/stage-mt7925"
scripts/aware-build install-fileserver --destdir "$PWD/.build/stage-mt7925"
```

公共 hostap 继续按[公共构建入口](../../docs/build.md)构建。`prepare.py --modules-to` 是构建器使用的模块源码生成入口，路径必须位于 `.build`；无该参数时只准备固定输入。下载、源码和模块产物均为生成物，输入摘要变化时拒绝复用旧目录。保留诊断材料后，只移开对应可重建目录再准备。

安装交付七个模块：`cfg80211`、`mac80211`、`mt76`、`mt76-connac-lib`、`mt792x-lib`、`mt7925-common`、`mt7925e`。它们必须成套加载，不能与发行版旧无线栈混用。适配入口限定 Linux 7.0，实机范围为 Q8B `7.0.11+`；其他内核和其他无线驱动组合未验收。模块使用目标 headers 构建，未修改网络核心结构、板级驱动、设备树或启动镜像。

固件装到 `/lib/firmware/updates/mediatek/mt7925/`，保留发行版原文件；原始固件授权、WHENCE 及内核授权装到 `/usr/local/share/licenses/wifi-aware/mt7925/`。实际安装省略 `--destdir`，操作前须取得设备租约、备份程序和配置。安装本身不代表固件互通通过。

`radio.rs` 只接管空闲 managed 接口，进入 NAN 后删除引导接口，运行时仅允许所选 PHY 的 NAN / NAN_DATA。`radio.toml` 选择 2437 MHz 单频发现，数据频率由公共运行配置决定。Q8B 当前世界监管域的 5 GHz 信道有 NO_IR，诊断使用 `/etc/wirelessd/config.toml` 中的 `frequency = 2437`，没有推定实际国家码。MT7925 未实现 AWDL / AirDrop，相关申请返回明确错误。

## 真机验证与限制

2026-11-06，Q8B `7.0.11+` / GCC 13.3 完成上述七模块的编译、modpost、链接、暂存安装及实机加载，vermagic 与运行内核一致；运行模块的 GNU build-id 与安装文件一致，安装文件与暂存模块逐字一致。内核确认加载固定 WM / MCU patch，NAN 与 NAN_DATA 能力可见。公共 wirelessd、fileserver 的 ARM64 Release 构建与暂存安装通过，公共 hostap 原生构建通过。原有以太网 SSH 保持可用。

| 验证 | 结果 |
| --- | --- |
| NAN_START / 公共服务 | 合簇、日程提交、NDI、IPv6 地址及 `_fileserver._tcp` 注册完成 |
| 发现窗口 | 固件产生真实 DW_START（59）与 DW_END（60），hostap 据此发送同步服务发现帧 |
| iPad Pro 真机 | 12.9 英寸第六代，iPadOS 27.0.1；WiFiAware 能力检查、公共客户端签名构建与安装通过 |
| iPad 系统发现 | 系统选择器显示 `MT7925 Q8B` 和“设置”；双端日志确认对应 NMI 与服务名 |
| 新配对 | 点击“设置”后、进入 PIN 前失败；iPad 的 wifip2pd 返回错误 12，报告 DCEA 未启用配对缓存 |
| 安全 NDP / 文件传输 | 未进入，不能报告成功或吞吐 |

hostap 发出的服务发现帧含 DCEA 配对／缓存位和 NIRA；iPad 的首次发现结果也报告 `pairSetup=YES`、`caching=YES`，但创建配对会话时仍报告缓存位缺失。驱动没有收到对应的单播引导或 PASN 帧。固件信标属性与服务发现属性的一致性尚待确认，不能把推测写成已定位根因。公开厂商参考中的 UPDATE_ATTR 与 VENDOR_PAYLOAD 诊断请求返回成功，但未消除该错误，未纳入正式补丁。

正式七模块的运行 build-id、安装文件和暂存文件一致。正常停止后 NAN、NDI、hostap 及运行目录均释放；随后恢复正常 hostap 路径并重启，wirelessd / fileserver 为 active，配对窗口关闭，保持原有 disabled 开机状态。本次正式加载区间无新增内核警告、Oops 或固件异常。

早期 `20260414` 固件虽然声明 `0xa0`，仍未产生主机发现窗口；当前来源已替换为上述实际通过发现的公开版本。新配对、缓存身份重连、安全 NDP、IPv6 双向文件及 NDP 会话释放仍需继续验收。原有 iPad 配对记录、板端身份与共享文件保留。

## 来源清单字段

`driver/source.json` 的 `repository`、`commit` 固定仓库及提交，`version` 是内核版本，`file` 是归档名，`url` 是固定下载地址，`sha256` 是摘要，`directory` 是归档顶层目录，`license` 描述授权，`patch_series` 相对于 driver 目录并按非空、非注释行有序应用。字段必填，无自动替代。

`firmware/source.json` 的 `repository`、`commit` 固定仓库及提交；`version` 为 WM 构建时间；`required_features` 是无单位能力掩码整数，当前 128（要求原始 CNM 位；NAN 版本修正规则见上文）；`license` 指原始授权。`files` 每项的 `file`、`upstream_path`、`sha256` 分别是文件名、提交内相对路径和摘要。字段必填，不以最新版本补缺。

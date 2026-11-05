# MT7925 NAN 准备输入

MT7925 PCIe（`14c3:7925`）使用上游 MT76 / mac80211 / cfg80211 的原生 NAN 路径。此目录准备固件、完整无线内核源码及安全补丁，尚未注册为公共服务支持的硬件，也没有驱动安装入口。

## 固定来源

| 输入 | 固定版本 | 用途 |
| --- | --- | --- |
| linux-firmware | `b981cd3786e8275c6954f880e95ad54cacc5ccb8` | WM `20260414153105`、MCU patch `20260414153001a`、原始授权与 WHENCE |
| wireless-next | `8db0381351fe6af82f81693d5085790e291b2f17`，Linux 7.3.0 | 同一源码树内的 cfg80211、mac80211、MT76；包含首批 NAN 和 15 个运行／NDP 修复 |
| MT76 安全补丁 | `20260927210306.737669-1-sean.wang@kernel.org`，23 个有序补丁 | NAN 密钥、管理帧时隙、合簇事件、停止和资源生命周期 |
| 项目接收修复 | `hardware/be200/driver/patches/0003-mac80211-nan-address.patch` | 按接收地址分发 NAN 单播管理帧，复用既有实现 |

安全补丁来自[作者发布的原始邮件](https://lore.kernel.org/all/20260927210306.737669-1-sean.wang@kernel.org/)。补丁 18 仅刷新空行上下文；既有接收修复仅缩小上下文以适配不同内核中的相邻代码，功能内容不变。

固件必须同时声明 `MT792x_FW_CAP_NAN`（bit 5）与 `MT792x_FW_CAP_CNM`（bit 7）。固定候选的能力字节为 `0xa0`；不能修改二进制或驱动条件伪造这些能力。

2026-11-05 核查官方仓库的 17 个 MT7925 WM 历史版本，只有 `20260414153105` 声明上述两个能力。板上 `20260605184805` 和公开最新的 `20261016233029` 均为 `0x80`，只声明 CNM。该结论限定于这组已核查的固件，能力声明不等于启动或互通验收通过。

## 准备与检查

需要 Python 3.12 或更新版本、curl、patch；检查 `.zst` 固件另需 zstd。下载包与展开源码只进入 `.build/dependencies/mt7925/`，展开前至少保留 8 GiB 可用空间。

```sh
python3 hardware/mt7925/prepare.py
python3 hardware/mt7925/firmware/inspect.py \
  .build/dependencies/mt7925/firmware/WIFI_RAM_CODE_MT7925_1_1.bin
```

准备入口校验每个下载文件的 SHA-256、WM 构建时间及能力位，按 `driver/patches/series` 应用补丁并禁止模糊上下文匹配，保留原始固件授权。`source/wireless-next` 为加补丁的完整内核；`source/radxa-kernel` 和 `source/linux-qcom` 为独立的 Q8B 平台参考，不会自动合并。`.prepared.json` 记录对应源码的归档与补丁输入摘要；展开源码是生成物，修改来源清单或补丁才是持久输入。

输入变化或准备失败时保留诊断材料，只移开或清理对应的可重建源码目录后重新准备。该入口不写 `/lib/modules`、`/lib/firmware`、`/boot`，也不加载模块或重启服务。

### 来源清单字段

`driver/source.json` 的 `repository` 与 `commit` 固定源码仓库及完整提交；`version` 是内核版本；`file` 是下载包文件名；`url` 是固定提交的归档地址；`sha256` 是归档摘要；`directory` 是归档顶层目录；`license` 描述授权，具体原文保留在源码内；`patch_series` 相对于 driver 目录，按非空、非注释行顺序应用。字段均必填，无省略默认值。

`firmware/source.json` 的 `repository`、`commit` 固定固件仓库及提交；`version` 为 WM 构建时间字符串；`required_features` 为无单位的能力位掩码整数，当前为 160；`license` 指向随交付保留的原始授权。`files` 每项的 `file` 为本地文件名、`upstream_path` 为提交中的相对路径、`sha256` 为文件摘要。字段均必填，不以最新分支或其他版本补缺。

## 验收边界

2026-11-05，本机与 Q8B 原生准备均通过：归档和固件摘要一致，23 个上游补丁及项目接收修复均以零模糊上下文匹配完成应用。Q8B 使用 GCC 13.3.0、运行配置派生的独立构建目录，完成候选源码的 `olddefconfig`、`prepare`、`modules_prepare`，以及 `mt7925-common.o`、`mt7925e.o`、`net/mac80211/rx.o`、`net/wireless/nl80211.o` 原生对象编译。仅编译对象，没有生成可交付的完整内核模块集或启动镜像。

固件来源、摘要、能力声明及补丁准备检查通过，只证明依赖可重现。Q8B 平台参考和运行环境见 [Dragon Q8B](../../platforms/dragon-q8b/README.md)。

仍需完成 Q8B 平台与新无线内核的集成、相关构建与暂存安装、可恢复部署，以及真实 NAN 启动、iPad 配对、安全 NDP、IPv6 双向数据和退出回收验收。现有 7.0.11 headers 缺少所需 NAN_DATA 与日程接口，不能只替换 `mt7925e.ko`。不能将这些准备输入当作可启动的 Q8B 内核或已支持 Wi-Fi Aware 的硬件交付。

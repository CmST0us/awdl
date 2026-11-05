# Dragon Q8B 构建输入

目标为 Radxa Dragon Q8B（SC8280XP）、ARM64 Ubuntu，网卡为 PCIe MT7925。`build.toml` 只选择目标架构，不配置无线能力。

`source.json` 固定 Radxa `linux-qcom` 打包仓库与其中的内核子模块提交，供后续保留板级驱动、设备树和启动方式。该参考源码为 Linux 7.0.11；它没有自动合并到 MT7925 的无线内核，也未证明与板上自建的 `7.0.11+` Image 逐字对应。

2026-11-05 板端核查：运行内核为 `7.0.11+`，构建编译器为 GCC 10.5.0；`/proc/config.gz` 与 `/usr/src/linux-headers-7.0.11+/.config` 逐字一致，cfg80211、mac80211、MT7925E 均为模块。板端原生 GCC 为 13.3.0，尚不能据此复用不同内核的模块或 Module.symvers。

内核构建依赖包括 C 工具链、make、bc、bison、flex、libelf-dev、libssl-dev、cpio、kmod、Python、xz-utils、zstd、git、curl、patch、rsync；公共 hostap 另需 pkg-config、libnl-3-dev、libnl-genl-3-dev。板端已安装这些依赖，libnl 为 3.7.0，OpenSSL 为 3.0.13，libelf 为 0.190；补齐 wireless-regdb 的数据库与签名文件。未重新加载运行中的 cfg80211，新的监管数据库是否被内核接受尚未验证。

公共 Rust 服务使用固定的 Rust 1.93.0 ARM64 GNU 工具链。`toolchain.json` 记录官方发布清单地址及 rustc、标准库、Cargo 的下载地址和摘要；安装包放在 `.build/dependencies/mt7925/`，工具链使用独立用户目录，不覆盖系统编译器。公共服务的依赖继续由根目录 `Cargo.lock` 管理，hostap 继续使用 `components/hostap` 的固定来源与配置。

板端准备目录为 `~/wifi-aware`，工具链安装到 `~/.local/opt/rust-1.93.0`；rustc、Cargo 均已在 ARM64 主机运行并确认版本。持久构建选择保存在 `config/local/build.toml`，工具链搜索路径保存在 `config/local/mt7925-toolchain.env`，不进入 Git。使用该 shell 环境构建公共用户态：

```sh
cd ~/wifi-aware
source config/local/mt7925-toolchain.env
scripts/build-hostap.sh --archive "$PWD/.build/dependencies/mt7925/wpa_supplicant-2.12.tar.gz"
```

2026-11-05，板端 `cargo fetch --locked --offline --target aarch64-unknown-linux-gnu` 通过，依赖缓存包括 Cargo.lock 固定的注册表与 Git 来源；公共 hostap 使用固定 2.12 归档在 Q8B 原生构建通过。上述命令只构建公共 hostap；不表示公共服务已接入 MT7925，也不会部署无线内核。

## 清单字段

`source.json` 的 `kernel` 和 `packaging` 分别描述板级内核及打包输入。每项 `repository` 是来源仓库、`commit` 是完整提交、`file` 是下载包名、`url` 是固定提交的归档地址、`sha256` 是归档摘要、`directory` 是归档顶层目录。`kernel.version` 是内核版本，`kernel.license` 描述授权并保留源码中的原文。字段均必填，无自动选择或省略行为。

`toolchain.json` 的 `version` 为工具链版本、`target` 为 Rust triple、`manifest` 为官方发布清单 URL；`files` 中的 `file`、`url`、`sha256` 分别为安装包名、固定下载地址与摘要。均为必填字符串，不允许以其他版本自动替代。

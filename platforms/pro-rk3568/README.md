# pro-rk3568-h / AP6275P

`--platform pro-rk3568` 提供 ARM64 构建默认值，适用于 BCM43752 A2 的驱动及公共服务构建；正式固件交付仍待验收。板级安装提供 CPU performance、TCP 32 MiB 自动调节上限与 fq 默认队列，保留内核热限制；这项策略增加空闲功耗。硬件实现与固件限制见 [BCM43752](../../hardware/bcm43752/README.md)。

已核对的目标系统为 Ubuntu 24.04.4、ARM64、内核 `6.1.115-gd2eccb6c9eff-dirty`，Wi-Fi PCI 为 `0001:11:00.0`。管理链路走 `eth0`，不依赖无线接口。

## 原生构建

所需工具为 GCC、Make、Git、Python 3.12+、匹配的内核头文件与 `Module.symvers`；后续公共服务构建另需 OpenSSL、libnl、D-Bus 与 libcap 开发包。已验证的头文件目录为 `/usr/src/linux-headers-6.1.115-gd2eccb6c9eff-dirty`，`Module.symvers` SHA-256 为 `7ad515cb7aaec9dc9688028d66b5b935aff2e823e61d430b96ea8db411afc815`。

将 `hardware/bcm43752/prepare.py` 的展开结果放到板端 `.build` 后，在板上构建：

```sh
make -C /usr/src/linux-headers-$(uname -r) \
  M="$PWD/.build/bcm43752/driver" ARCH=arm64 \
  CONFIG_AP6XXX=m CONFIG_BCMDHD_PCIE=y \
  CONFIG_BCMDHD_SDIO= CONFIG_BCMDHD_USB= \
  CONFIG_BCMDHD_MULTIPLE_DRIVER=y CONFIG_BCMDHD_NAN=y -j4 modules
```

`M` 是展开驱动的绝对路径，必须对应当前构建；`ARCH=arm64` 对应目标架构；`CONFIG_AP6XXX=m` 生成模块；PCIe 启用、SDIO/USB 为空分别选择或排除总线；`MULTIPLE_DRIVER=y` 使用独立模块名 `dhdpcie`；`NAN=y` 编译驱动已有 NAN 代码；`-j4` 是四个编译任务。以上参数不应省略或自动推断为其他板卡配置。

GCC 13.3 构建通过，内核头文件记录的 GCC 为 10.5，因此 Kbuild 有工具链版本提示；模块 vermagic 匹配实际内核。构建产物只作临时诊断加载，不能覆盖原装开机模块或视为 NAN 固件已实现。

## 设备所有权与恢复

任何模块加载、卸载、读取或恢复操作都先取得 `/run/lock/wifi-aware-device.lock` 的独占租约。更换驱动前保留原模块、固件、CLM、NVRAM 和 NetworkManager 配置；原装模块位于当前内核的 `updates/bcmdhd_pcie.ko`。本次未更换该开机模块和固件文件。

NetworkManager 的 `/etc/NetworkManager/conf.d` 配置可覆盖 `/run` 的同类项；仅写 `/run` 排除规则不能保证不被接管。诊断加载前应核对 `NetworkManager --print-config`，排除 `wlan0`、`wlan1` 与 `aware_nmi0`，重载配置后确认全部接口 DOWN。正式 NAN 接管还须由硬件后端验证整块 PHY 的模式及真实资源释放。

操作者已授权使用小电拼 8509 的 **C1（端口索引 2）** 给本板断电重启。设备卡死且软件恢复失败时，在持有租约的操作范围内核对端口映射和当前状态，只关闭 C1、确认关闭后再开启并核对恢复；不得操作其他端口。SSH、租约记录和控制器原始结果不进入 Git。

临时验收单元与正式 `wirelessd.service` 一样使用 `KillMode=mixed` 和 `TimeoutStopSec=90` 秒，覆盖 AWDL 正常停止、异常进程恢复清理及 hostap 退出。先由主进程注销服务、结束 NDP，再通知 hostap 清理；期限只作为失败兜底。默认 `control-group` 会同时终止两者，无法验证正常退出顺序。

## AirDrop 蓝牙依赖

本板设备树将 AP6275P 蓝牙接到 `/dev/ttyS8`，RTS/CTS 使用 UART8 引脚，电源由 `wireless-bluetooth` 的 `rfkill_bt` 驱动管理。内核已包含 H4、BCM 与 LE 支持；仅有 Wi-Fi 驱动并不会创建蓝牙 HCI。

安装发行版 `bluez` 包以取得 `/usr/bin/hciattach`，并执行 `systemctl disable --now bluetooth.service`，防止 HCI 出现时被 BlueZ 自动接管。`install-platform` 安装 `wifi-aware-bluetooth.service` 与 AirDrop 依赖项；只有启动 AirDrop 时才启动 UART 初始化。服务与 `bluetooth.service` 互斥，为公共 AirDrop 保留关闭状态的专用 `hci0`，由应用在操作期间取得 HCI user channel。不要同时启动 BlueZ 管理守护进程。

所需 `/lib/firmware/BCM4362A2.hcd` 来自与 Wi-Fi 固件相同的固定 [rkwifibt 来源](https://github.com/radxa/rkwifibt/tree/b61a1e8499a4f1956ef417cbcfaa6c2cf805ce08/firmware/broadcom/AP6275_PCIE/bt)，SHA-256 为 `3a0a926fa800c121f850ed4f961a1e058a993666d9ee565c1e46dfd596e25567`，原始授权为 `hardware/bcm43752/firmware/LICENSE.rkwifibt`。目标系统已带此文件；平台安装器不下载或替换它，启动准备检查摘要，不匹配即失败。

UART 初始速率为 115200 bit/s、下载和运行速率为 1500000 bit/s，使用硬件流控。[BlueZ 5.72 hciattach](https://github.com/bluez/bluez/blob/5.72/tools/hciattach.c) 在初始化成功后才 fork，systemd 据父进程完成判定就绪；30 秒初始化期限仅作失败兜底。停止时关闭 UART line discipline，rfkill 保持上电；下次启动重新执行本板复位与固件下载。已有 `hci0` 时拒绝复位，不接管其他控制器。已在本板完成固件下载与 HCI 初始化，`wifi-aware-bluetooth.service`、`wirelessd.service` 和 `airdrop.service` 均启动成功；系统 BlueZ 守护进程保持停用。该结果不代表系统 AirDrop 双向文件验收通过。

当前板载内核的 `/proc/bluetooth/sleep/btwrite` 读取处理会直接访问用户态缓冲区，实测触发 `bluesleep_read_proc_btwrite` Oops；不得用 `cat` 等方式查询该节点。上游同源 `lpm` 读取处理也使用同样写法。启动准备仅写入 `btwrite`，通过 rfkill、HCI 和 systemd 状态观测结果；本项目未修改板载内核的该读取缺陷。

## 性能策略

`install-platform --platform pro-rk3568` 安装 `wifi-aware-performance.service`、无线服务依赖及 TCP 配置。服务按当前 CPU policy 选择 `performance`，TCP 发送／接收自动调节上限均为 32 MiB；不强制每条连接预分配该容量。新建网络接口使用 `fq` 默认队列，需要内核 `CONFIG_NET_SCH_FQ`；内建或模块均可。该默认值作用于之后新建的所有接口，既有接口的队列保持原状。原生 NDI 的驱动发送路径保留 socket 记账至 TX 完成，使 TCP Small Queues 与 pacing 对实际队列生效。原参数应在首次部署前保存，停止无线服务不会自动撤销这项板级策略。

公共构建工具构建、暂存安装及 RK3568 上的 systemd 启动已通过；这些检查只验证策略可部署，不代表吞吐验收。性能验证采用 BE200 已有经验一次性完成配置后统一测试，不另建逐级基线。

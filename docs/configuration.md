# 配置架构

配置是部署者或组件维护者交给实现的决策输入。一个值能在现有实现支持的范围内独立改变，就应由对应配置拥有；设备当前能力、接口名称和协议协商结果由运行时取得；协议编码、状态转换与资源所有权由代码负责。

文件按决策者、消费者和生效时机拆分。同一个默认值只有一份可编辑来源，生成文件由来源产生，已有机器的本地配置独立保留。

本文定义组件的配置文件、所有权与加载规则，组件关系见[组件架构](architecture.md)。

## 配置的所有权

| 类别 | 决策者与消费者 | 典型内容 | 生效时机 |
| --- | --- | --- | --- |
| 无线服务配置 | 部署者 → wifiawared | 选定 PCI、数据频率、展示信息、状态目录、控制阶段期限 | 服务重启 |
| 文件应用配置 | 部署者 → fileserver | 共享目录、监听端口、I/O 空闲期限 | 文件服务重启 |
| 无线适配配置 | 硬件维护者 → 公共 Radio 与对应后端 | NAN 启动参数、已验证的本地时隙策略 | 无线服务重启 |
| 双端服务契约 | 应用维护者 → Linux 发布端与 iOS 发现端 | 服务发现名称 | 重新构建客户端并部署对应服务配置 |
| 构建与平台配置 | 维护者、构建者 → aware-build / 原生构建工具 | 固定来源、补丁顺序、目标架构、工具链、内核输入、板级选项 | 重新构建或安装 |
| 系统集成配置 | 部署者 → systemd、NetworkManager、内核模块 | 进程依赖、服务重启、设备管理权、地区相关模块参数 | 对应系统配置重载、模块重载或重启 |
| 持久状态与临时数据 | hostap、服务、内核写入 | 配对身份、密钥、控制套接字、会话、实际协商参数 | 由所属状态机管理 |

## 文件布局

```text
wifi-aware/
├── config/
│   ├── build.toml                      # 公共构建工具默认值
│   ├── examples/                       # 带注释的本机与部署示例
│   │   ├── build.toml
│   │   ├── install-rock5b-rtl8852be.toml # ROCK 5B / RTL8852BE
│   │   └── install-rock5b-be200.toml    # ROCK 5B / BE200
│   ├── wifiawared.toml                 # 公共运行默认值
│   ├── fileserver.toml                 # Linux 文件应用运行默认值
│   ├── fileserver-discovery.toml       # Linux / iOS 共用的发现契约
│   └── local/                          # 本机持久配置，不入 Git
│       ├── build.toml                   # 本机工具链、headers 等路径
│       └── install.toml                 # 硬件、PCI 与地区安装选择
├── components/
│   ├── hostap/
│   │   ├── source.json
│   │   ├── hostap.config                # 上游原生编译配置
│   │   └── patches/series
│   ├── wifiawared/
│   │   ├── wifiawared.service           # 原生 systemd 单元
│   │   └── org.wifiawared.conf          # 原生 D-Bus 策略
│   └── fileserver/
│       └── fileserver.service
├── hardware/
│   ├── be200/
│   │   ├── radio.toml                   # 已验证的 BE200 无线运行策略
│   │   └── driver/
│   │       ├── source.json
│   │       ├── be200.defconfig          # 驱动编译输入
│   │       ├── be200.conf               # 通用模块参数
│   │       └── be200-cn.conf            # 明确选择的地区 / 平台参数
│   └── rtl8852be/
│       ├── radio.toml
│       ├── driver/                     # 来源、补丁及模块配置输入
│       └── firmware/
│           ├── source.json
│           └── patches/                # series 与每步 JSON 参数
├── platforms/
│   └── rock5b/
│       ├── build.toml                   # ARM64 / ROCKCHIP 构建选项
│       └── usbmoded-shutdown.conf       # 板级系统配置
├── ios/
│   ├── AwareClient/Client.plist         # 客户端连接及 I/O 期限
│   └── Configurations/                 # bundle、显示名、签名构建设置
├── .private/                           # 私有证据与临时材料
└── .build/
    └── config/                         # 工具生成的配置产物，可删除重建
```

运行默认值由根目录 `config` 管理；组件目录保留其原生系统配置。固件 JSON、Kconfig、Cargo.lock、plist、xcconfig 和 systemd 文件继续使用各自工具的格式。

`config/local` 是手工维护的持久输入，Git 仅忽略其版本管理，构建清理保留该目录。`.build/config` 是从配置源生成的安装文件、iOS 资源和设备规则，只由工具写入，删除后能够从原输入重建。目标机器上的 `/etc` 配置属于持久部署数据，独立于工作区构建目录。

## 配置注释

每个手工维护的配置项都在字段旁说明用途；数值注明单位与有效范围，路径注明所属机器与是否要求绝对路径，可选项注明省略或空值的含义。文件或配置段统一说明生效方式，存在双端一致性或硬件约束的字段就地注明关联条件。

默认配置中的值就是默认值，部署示例中的机器路径、PCI 和地区只代表该示例。注释解释含义与约束，不重复维护另一份默认值表。

TOML、systemd 和 shell 风格配置使用 `#`，XML plist 使用 XML 注释，xcconfig 使用 `//`。安装到 `/etc` 的可编辑配置保留字段注释。JSON 等不支持注释的原生格式在同目录说明文件中逐字段解释，保持原生语法。

## 无线服务配置

源文件为 `config/wifiawared.toml`，安装到 `/etc/wifiawared/config.toml`。保留现有 `pci`、`frequency`、`state`、`runtime`、`supplicant` 键，增加职责明确的配置段。

```toml
# 无线服务运行默认值；修改后重启 wifiawared 生效。

# 完整 PCI 地址，如 0000:03:00.0；空字符串选择唯一受支持设备。
# 存在多个候选时必须指定，接口名称由该设备自动解析。
pci = ""
# 期望的数据主信道频率，单位 MHz；实际可用性由所选驱动确认。
frequency = 5745
# 目标机器上保存配对身份和密钥的绝对路径；升级保留目录内容。
state = "/var/lib/wifiawared"
# 目标机器上控制套接字等临时运行数据的绝对路径。
runtime = "/run/wifiawared"
# 随本组件安装的 hostap 可执行文件，由无线服务启动。
supplicant = "/usr/local/libexec/wifiawared/wpa_supplicant"
# 可选：指定匹配所选硬件的完整无线策略文件，路径位于目标机器。
# 省略时读取该硬件随组件安装的 radio.toml。
# radio_profile = "/etc/wifiawared/radio.toml"

[identity]
# 配对发现信息中的厂商展示名，由部署产品决定。
vendor_name = "Wi-Fi Aware"
# 配对发现信息中的产品型号展示名，与所用网卡型号独立。
model_name = "Wi-Fi Aware Device"
# 系统配对界面中的设备展示名，不作为配对密钥或设备唯一标识。
pairing_name = "Aware Device"

# 各阶段失败期限均为正整数秒；收到完成事件立即推进。
[timeouts]
# 等待一条 hostap 控制命令回复的最长时间。
control_reply_seconds = 3
# hostap 子进程启动后，等待控制接口就绪的最长时间。
hostap_ready_seconds = 5
# NAN 启动后，等待集群建立的最长时间。
cluster_join_seconds = 10
# 单次配对或缓存身份认证的最长时间，与配对窗口时长独立。
authentication_seconds = 30
# 单次 NDP 建链的最长时间，不限制已建立会话的持续时间。
ndp_setup_seconds = 30
# 终止 NDP 时保留清理责任并等待结束的最长时间。
ndp_end_seconds = 10
# 请求 hostap 正常退出后，等待子进程及其 NAN 接口释放的总期限。
hostap_stop_seconds = 5

[retention]
# 无活动会话的内存 peer 记录保留时间，正整数秒；不删除配对凭据。
inactive_peer_seconds = 600
```

`pci` 为空时选择唯一受支持设备，多候选要求明确指定。后端由实际设备确定，接口名从所选 PHY 取得。`frequency` 表示期望的数据主信道；真正可用的信道、带宽和监管限制由所选驱动确认。

`identity` 是设备展示信息，由部署产品决定。它不参与 NIK/NPK 身份生成，也不随网卡更换自动变成另一厂商名称。

控制期限按阶段分别命名。默认值继承 BE200 Rust 基线，两个后端共用阶段期限。阶段收到完成事件立即继续，期限只决定缺少完成时何时失败。认证能力轮询的 50 ms tick、接口盘点周期及固定 sleep 不成为配置项。

`inactive_peer_seconds` 只控制无活动会话的内存 peer 记录回收，不控制配对凭据有效期。配对窗口时长仍由 `OpenPairing(seconds)` 的单次调用决定。

路径字段由守护进程负责读取并创建所属目录。标准 systemd 单元管理默认的 StateDirectory / RuntimeDirectory；部署选择其他目录时，安装结果须与配置一致，由服务按既有权限约定创建实际目录。

## 无线适配配置

`hardware/<name>/radio.toml` 与对应后端、驱动、固件共同版本化。用户态安装将受支持后端的文件放入 `/usr/local/share/wifi-aware/hardware/<name>/radio.toml`；服务在完成设备选择后读取对应文件。

BE200 的运行策略：

```toml
# BE200 已验证的无线策略；修改后重启无线服务生效。
# 配置必须与实际硬件匹配，不能通过改名选择另一种网卡能力。
hardware = "be200"

[nan]
# NAN 发现主信道频率，单位 MHz；须由后端支持。
discovery_frequency_mhz = 2437
# hostap 的 NAN 双频配置；数据主信道仍由运行配置 frequency 指定。
dual_band = true
# NAN 主设备选择优先级，使用本后端已验收的取值。
master_preference = 100
# 发现信标间隔，单位 TU；受后端支持的启动组合约束。
discovery_beacon_interval_tu = 100

[schedule]
# 发现主信道的可用槽号，整数 0–31；每槽 16 TU，不替代协议 DW。
discovery_slots = [1]
# 数据主信道的可用槽号，整数 0–31；不同信道的两组槽不能重叠。
data_slots = [0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
              16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31]
```

RTL8852BE 的运行策略：

```toml
# RTL8852BE 已验证的无线策略；修改后重启无线服务生效。
# 配置必须与实际硬件匹配，不能通过改名选择另一种网卡能力。
hardware = "rtl8852be"

[nan]
# NAN 发现主信道频率，单位 MHz；当前 RTL 后端使用 2437。
discovery_frequency_mhz = 2437
# hostap 的 NAN 双频配置；当前 RTL 后端要求 false。
# 该值不关闭独立配置的 5 GHz 数据主信道。
dual_band = false
# NAN 主设备选择优先级；当前 RTL 后端要求 200。
master_preference = 200
# 发现信标间隔，单位 TU；当前 RTL 后端要求 100。
discovery_beacon_interval_tu = 100

[schedule]
# 发现主信道的可用槽号，整数 0–31；每槽 16 TU，不替代协议 DW。
discovery_slots = [0, 1, 2, 3, 8, 10, 11]
# 数据主信道的可用槽号，整数 0–31；不同信道的两组槽不能重叠。
data_slots = [4, 5, 6, 7, 9, 12, 13, 14, 15, 16, 17, 18, 19, 20,
              21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31]
```

`discovery_slots` 表示发现主信道上的本地可用时隙，不替代协议 DW 定义。当前后端采用 512 TU 周期、16 TU 时隙，槽号为 0–31；这些单位由后端支持的调度格式定义。公共编码将槽号转换成 hostap 位图，产生当前 BE200 的 `fdffffff/02000000` 和 RTL 的 `f0f2ffff/0f0d0000`。

数据主信道从运行配置的 `frequency` 取得。同一信道时使用已支持的单信道全周期策略；不同信道时两组槽不能重叠。对端协商和交集计算仍由 hostap 与后端执行。

设备能力约束继续由实现检查。RTL 当前只接受指定的 master preference、dual_band 和 beacon interval；修改配置不会扩展固件能力。配置描述已支持的选择，驱动描述支持范围。

工程调试可通过 `wifiawared.toml` 的可选 `radio_profile` 指定一份完整的本地策略文件。文件必须匹配实际硬件；它整体替代该硬件的默认策略，不进行字段级叠加。正式配置固定本轮实际使用的文件，验收结论记录对应策略内容。

设备 PCI ID、hostap driver 名称、NMI/NDI 操作及资源释放流程继续属于后端实现。它们决定如何操作硬件，不作为用户选择另一行为的配置项。

## 文件应用与双端服务契约

`fileserver.toml` 由部署者调整 Linux 服务端行为；`fileserver-discovery.toml` 由应用维护者定义双端一致的发现名称。共享目录、端口和 I/O 期限改变时只需重启服务端；发现名称改变时需要同步部署服务端并重新构建客户端。两份来源在安装时合成为一个 `/etc/fileserver/config.toml`。

### 发现名称

`config/fileserver-discovery.toml` 是文件应用发现名称的唯一默认来源：

```toml
# Linux 发布端与 iOS 发现端共同使用的服务名，文件服务须以 ._tcp 结尾。
# 修改后需生成匹配的两端配置，重新构建客户端并部署服务配置。
service = "_rtl8852be-test._tcp"
```

构建工具读取它，为 Linux 文件服务提供默认服务名，同时生成 iOS 的 `WiFiAwareServices` 声明。Swift 从生成的应用资源读取服务名。脚本构建与 Xcode 直接构建使用同一生成步骤及依赖输入；清洁构建无需提前手工生成文件。

该文件不规定设备厂商、网卡型号、共享目录或监听端口。客户端从服务连接取得端口，端口变化只需重启并重新注册服务。

`fileserver` 支持 `service` 配置及 `--service` 覆盖。修改实际服务名时，客户端必须声明并发现同一名称；使用项目 AwareClient 时需要用对应契约重新构建和签名。其他应用使用自己声明的服务名。

### 服务端部署

`config/fileserver.toml` 保存运行默认值：

```toml
# 文件服务运行默认值；修改后重启 fileserver 生效。

# 目标机器上的共享目录，必须为绝对路径。
files = "/srv/fileserver"
# 在 NDI 上监听的 TCP 端口，范围 1–65535，由服务注册通告给客户端。
port = 7777
# 连接 I/O 空闲期限，正整数秒；不限制整个文件的传输总时长。
io_timeout = 60
```

安装工具将此文件与默认服务名合成为完整的 `/etc/fileserver/config.toml`，配置字段与现有部署一致：

```toml
# 文件服务部署配置；修改后重启 fileserver 生效。

# 发布的 TCP 服务名，须以 ._tcp 结尾并与客户端声明一致。
# 改名时需要同步生成客户端声明并重新构建客户端。
service = "_rtl8852be-test._tcp"
# 目标机器上的共享目录，必须为绝对路径；已有部署保留原目录。
files = "/srv/fileserver"
# 在 NDI 上监听的 TCP 端口，范围 1–65535，由服务注册通告给客户端。
port = 7777
# 连接 I/O 空闲期限，正整数秒；不限制整个文件的传输总时长。
io_timeout = 60
```

程序的缺省值同样读取这两份版本化来源，可在编译时嵌入原文。Rust `Default` 不再另写同一组字面量。安装已有文件时保留其内容。

已有部署切换到 `fileserver` 时，将原配置保留字段值安装到目标配置路径，共享文件继续使用配置中的原目录。服务单元切换后只启用 `fileserver.service`，避免两个文件服务重复监听和注册。无线服务的配对状态独立保留。

### 客户端策略与安装身份

`ios/AwareClient/Client.plist` 保存 `ConnectionTimeoutSeconds` 和 `IOIdleTimeoutSeconds`，初始均为 BE200 基线的 60 秒。客户端读取原生 plist，分别用于建链失败期限与连接读写空闲期限。服务端和客户端各自拥有期限，不从服务端下载策略后才能开始连接。

```xml
<plist version="1.0">
<dict>
    <!-- 客户端随应用交付的策略，修改后重新构建并安装应用。 -->
    <!-- 单次数据连接建立的最长时间，正整数秒。 -->
    <key>ConnectionTimeoutSeconds</key>
    <integer>60</integer>
    <!-- 连接读写允许的无进展时长，正整数秒，不是整个文件的传输时限。 -->
    <key>IOIdleTimeoutSeconds</key>
    <integer>60</integer>
</dict>
</plist>
```

bundle ID、显示名和签名设置放在 `ios/Configurations/*.xcconfig`，由既有安装身份的 scheme 选择。两个客户端 bundle 与诊断 App 的身份分别保留。服务名来自共用契约，不在每个 scheme 中另写。

256 KiB 读写块、分片分配算法和 HTTP 缓冲布局保留在文件应用实现中。两端缓冲块无需相同，HTTP 定义的是字节流及范围。单次单连接／多连接选择仍由 App 操作决定。

## 构建、平台与安装

### 构建输入

固定源码版本、依赖锁、补丁顺序和固件补丁参数保留现有文件：`source.json`、`Cargo.lock`、`series`、Kconfig 与固件 JSON。它们属于产物定义，随代码审阅和版本化。

`config/build.toml` 拥有并行度和 C / MIPS 编译器等通用默认值。`mips_compiler` 使用包含 MIPS16 后端的 Clang 21，`U2_CC` 和 `--mips-cc` 可覆盖。

`platforms/rock5b/build.toml` 拥有板级选择，如目标 ARM64 和 RTL 的 `CONFIG_PLATFORM_ARM_ROCKCHIP=y`。BE200 和 RTL 的驱动功能选择仍在各自硬件目录。RTL 构建配方中的硬编码 `ARCH=arm64` 和 ROCKCHIP 选项改从选定平台取得。

本机路径与工具链放入持久保存、不入 Git 的 `config/local/build.toml`，例如：

```toml
# 本机构建示例；修改后用于下一次构建，不影响已安装产物。

# 目标平台配置名，对应 platforms 下的目录。
platform = "rock5b"
# Rust Linux 目标 triple；需安装对应标准库并配置匹配的 linker。
rust_target = "aarch64-unknown-linux-gnu"
# 构建机上目标内核构建目录的绝对路径，需匹配目标内核版本及配置。
kernel_headers = "/opt/kernel-headers/rock5b"
# 可选：目标内核发行号，交叉构建时填写目标机器的 uname -r 结果。
# 仅在目标原生构建时可以省略并使用本机内核发行号。
# kernel_release = "目标内核发行号"
# hostap 等 C 用户态组件的编译器；填写 PATH 中的命令名或绝对路径。
c_compiler = "aarch64-linux-gnu-gcc"
# 内核交叉工具链前缀，包含末尾连字符；可填写带绝对路径的前缀。
cross_compile = "aarch64-linux-gnu-"
# 最大并行构建任务数，正整数，按构建机内存和 CPU 资源设置。
jobs = 4
```

`kernel_release` 在构建目标与运行主机不同时显式给出；目标原生构建可以使用本机内核发行号。本机绝对路径不进入平台默认配置。

构建参数按字段使用：显式命令参数 → 对应的既有环境变量 → 本机构建文件 → 平台默认值。现有 `CARGO_BUILD_TARGET`、`KERNEL_RELEASE`、`KLIB_BUILD` / `KSRC`、`CC`、`U2_CC`、`ARCH`、`CROSS_COMPILE`、`JOBS` 和 `DESTDIR` 映射到唯一内部字段。后续各构建步骤只读解析后的目标，不各自再解释一遍环境。

### 地区与设备安装选择

网卡型号不能决定部署地区。RTL 的 `rtw_country_code=US` 从硬件通用默认文件中移出，改为明确的安装输入。BE200 的 `platform_cn=1` 保留其现有含义：对应地区下的特定平台提示，不当作通用国家码的等价开关。

安装选择持久保存在 `config/local/install.toml`；示例只展示 RTL 部署字段：

```toml
# 本地安装示例；字段由本次目标设备确定，在执行安装时生效。

# 要安装的硬件支持包，对应 hardware 下的目录，并须匹配实际网卡。
hardware = "rtl8852be"
# 目标板级配置，对应 platforms 下的目录。
platform = "rock5b"
# 目标机器上网卡的完整 PCI 地址，用于运行配置和设备管理规则。
pci = "0002:21:00.0"

[rtl8852be]
# 实际部署地区的两字母国家码；示例为 CN，安装为 RTL 模块参数。
country = "CN"
```

地区值由操作者按实际部署环境填写。安装器据此生成对应的原生 modprobe 文件，缺少必需值时报告缺失，不自动写入另一地区。BE200 使用独立的安装字段表达既有 `--cn` 选择。运行时可用信道以实际驱动结果为准。

选定 PCI 是设备管理规则与首次运行配置的共同安装输入。已有运行配置优先；改变选卡时，同一次部署显式更新 PCI 和关联的 NetworkManager / systemd 设备规则，避免三处各写一个接口名。

### 原生系统配置

systemd 单元拥有进程启动、重启策略和外部停止期限；D-Bus 策略拥有本机访问约定；modprobe 拥有模块参数；NetworkManager 拥有设备管理权。原生格式的文件由对应系统组件直接消费。

固定的 systemd 与 D-Bus 文件直接安装。需要实际 PCI、接口名或安装地区的文件由安装器按目标设备生成。ROCK 5B 的 usbmoded 关机依赖随板级安装步骤部署。

控制阶段的期限与 systemd `TimeoutStopSec` 含义不同。修改内部清理预算时，应同步调整 systemd 期限以覆盖完整退出流程。

## 加载和变更规则

各配置域独立解析，不建立跨域通用覆盖树。

| 配置域 | 解析顺序与行为 |
| --- | --- |
| 无线服务 | 嵌入的公共默认文件 → 指定的运行 TOML；现有 `--config` 选择文件 |
| 文件服务 | 嵌入的服务契约及默认文件 → 指定的运行 TOML → 现有显式命令参数 |
| 无线策略 | 设备选择 → 对应硬件文件；显式 `radio_profile` 整体替代这一份文件 |
| iOS | 构建时的服务契约及 scheme；应用读取最终资源与 Client.plist |
| 构建安装 | 明确的目标及本机输入，经 aware-build 解析后交给对应配方 |

字段缺省只在解析时补齐；数组整体替换。未知字段、无效单位或不能被后端执行的参数立即报错，指出来源文件和字段。配置不包含 shell 命令、表达式或任意递归 include。

启动时输出实际选择的硬件、PCI、配置文件和生效的非敏感参数，便于将运行结果与配置对应。硬件能力与协商状态分别输出，不伪装成配置值。

运行配置在对应服务重启后生效。配对窗口、服务注册和传输取消继续使用现有控制接口。重启无线服务会结束当前 NDP；重启文件服务会结束其传输。配置变更与启动按明确的操作流程执行。

升级保留 `/etc` 中已有配置及所有配对状态。新增字段从版本化默认文件补齐；现有顶层键直接继续使用。对已有配置的变更只修改明确指定字段。

## 状态与生成数据

| 文件或数据 | 所有者 | 处理方式 |
| --- | --- | --- |
| `components/hostap/hostap.config` | 构建维护者 | hostap 编译输入，随源码版本化 |
| `/run/wifiawared/bootstrap.conf`、控制套接字 | 无线服务 | 每次启动生成，退出清理 |
| `/var/lib/wifiawared/nan.conf` | hostap | 包含更新和持久化的配对状态，升级保留 |
| `/var/lib/wifiawared/local-nik` | 设备身份 | 本机生成、持久保留，不进入仓库 |
| 对端密钥、实际 NDP、协商信道和接口地址 | 协议栈、驱动 | 从运行状态取得，不写成静态配置 |
| `config/local` 中的本机配置 | 部署者 | 持久输入，不入 Git，构建清理保留 |
| `.build/config` 中的 iOS 资源和安装文件 | 构建／安装工具 | 可重建输出，只由工具写入 |

`nan.conf` 虽然带有配置文件后缀，其生命周期由配对状态决定。安装模板不能覆盖该文件，也不能从硬件配置重新生成身份。

## 已知限制

- 修改无线策略只能选择已实现能力；RTL 目前只支持其已验证的启动组合，扩展组合仍需实现与真机验收。
- 默认的 60 秒文件 I/O 期限容纳 BE200 已记录的约 19.7 秒停顿。已有部署的较短期限保留其实际行为；传输容忍期限不改变性能验收标准。
- 客户端声明的发现服务名随应用构建交付。服务端单独改名会导致现有客户端无法发现。
- 地区安装配置和期望频率不构成硬件实际可用信道或带宽的声明。

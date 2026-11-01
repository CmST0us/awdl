# iOS 应用与设备验收

AwareClient 使用一份 Swift 源码，通过 Wi-Fi Aware 服务契约发现设备。两个安装身份由 Configurations 内的 xcconfig 与 AwareClient / AwareClientBE200 scheme 选择；构建方法见[公共构建入口](../docs/build.md)。

Client.plist 的每项配置注明单位及含义。Info.plist 中的服务占位符由公共构建工具填充；直接使用 Xcode 同样执行生成阶段。AwareLab 诊断 App、真机测试和日志读取工具使用同一服务契约，保留原有安装身份。

AwareLabUITests 保留真机诊断用例，设备使用前通过 scripts/publisher_observation/device_lease.py 取得租约。testCommonClient 接受 AWARE_CLIENT_BUNDLE_ID（省略为标准 AwareClient）、AWARE_CLIENT_DEVICE_ID（已有配对 ID）、AWARE_CLIENT_ACTION（connect / download / upload / cancel / reconnect）。上传用 AWARE_CLIENT_FOLDER 指定文件选择器中的应用目录显示名（省略为 AwareClient.xcconfig 中的显示名）。只有选择 pair 时才使用 AWARE_M5_PIN。

测试设备、签名团队和实验窗口来自执行环境；原始日志、截图及配对信息保留在 .private。验收不通过删除配对或重置系统网络恢复。

AwareLab 的厂商、型号和配对目标名从公共无线配置的 identity 生成。`AWARE_VENDOR_NAME`、`AWARE_MODEL_NAME`、`AWARE_TARGET_NAME` 可指定实际部署信息，`AWARE_DISPLAY_ITEM_NAME` 可指定配对卡片显示名。应用、UI 测试和日志读取工具共用这些来源；安装和清理工具从 xcconfig 读取保留的 bundle 身份。

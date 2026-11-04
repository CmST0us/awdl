# iOS 应用与设备验收

AwareClient 使用一份 Swift 源码，通过 Wi-Fi Aware 服务契约发现设备。两个安装身份由 Configurations 内的 xcconfig 与 AwareClient / AwareClientBE200 scheme 选择；构建方法见[公共构建入口](../docs/build.md)。

Client.plist 的每项配置注明单位及含义。Info.plist 中的服务占位符由公共构建工具填充；直接使用 Xcode 同样执行生成阶段。AwareLab 诊断 App、真机测试和日志读取工具使用同一服务契约，保留原有安装身份。

AwareLabUITests 保留真机诊断用例，设备使用前通过 scripts/publisher_observation/device_lease.py 取得租约。testCommonClient 接受 AWARE_CLIENT_BUNDLE_ID（省略为标准 AwareClient）、AWARE_CLIENT_DEVICE_ID（已有配对 ID）、AWARE_CLIENT_ACTION（connect / download / upload / cancel / reconnect）。上传用 AWARE_CLIENT_FOLDER 指定文件选择器中的应用目录显示名（省略为 AwareClient.xcconfig 中的显示名）。只有选择 pair 时才使用 AWARE_M5_PIN。

测试设备、签名团队和实验窗口来自执行环境；原始日志、截图及配对信息保留在 .private。验收不通过删除配对或重置系统网络恢复。

AwareLab 的厂商、型号和配对目标名从公共无线配置的 identity 生成。`AWARE_VENDOR_NAME`、`AWARE_MODEL_NAME`、`AWARE_TARGET_NAME` 可指定实际部署信息，`AWARE_DISPLAY_ITEM_NAME` 可指定配对卡片显示名。应用、UI 测试和日志读取工具共用这些来源；安装和清理工具从 xcconfig 读取保留的 bundle 身份。

## AirDrop 自动接收验收

`AirDropTests/testReceiveFile` 使用现有 AwareLab Runner 将设备设为竖屏并操作系统设置：先关闭接收，再选择“所有人（10 分钟）”，确认选中状态后回到主屏幕。收到与配置发送方及文件名匹配的提示后，自动点击“接受”并确认提示关闭。支持中文和英文系统界面。

使用 Python 3.11 或更新版本及已配置签名的 Xcode。将 `config/examples/airdrop-uitest.toml` 复制到 `config/local/airdrop-uitest.toml`，填写真机 UDID、开发团队、目标板 SSH、共享租约与发送文件。每个字段的单位、约束和省略行为均在示例中注明。设备须已解锁、完成开发配对并开启开发者模式；目标板的无线与 AirDrop 服务须已启动，SSH 使用非交互认证。

```sh
python3 ios/scripts/run_airdrop_ui.py
python3 ios/scripts/run_airdrop_ui.py --file /var/tmp/airdrop-large.txt
```

入口持有 iOS 共享租约，完成构建和真机启动；收到 UI 的 `ready` 事件后才取得板端共享租约并调用 `airdrop send`。`accepted` 记录系统点击完成，板端 `delivered` 记录协议交付；两者均成功才判定整轮通过。失败或取消时停止活动的板端 AirDrop 操作。设置、请求、接受后的截图和界面树保存在 xcresult；完整日志与 `result.json` 位于 `.private/artifacts/airdrop-ui-*`。

“所有人”按系统规则在十分钟后恢复；每轮测试重新开启。测试保留既有应用身份、配对记录和接收文件。UI 自动点击成功不代表无线传输成功，结果中分别保留两端事件。

`result.json` 的 `throughput` 同时记录接受到 Upload 成功响应的整文件平均速度，以及正文进度按时间线性插值得到的 5 秒滑窗峰值。单位为十进制 MB/s；原文件与压缩正文分开统计，峰值原文件等效速度按两者总字节比例换算。峰值是发送端应用正文进度，不是空口速率或接收端落盘速率；不足 5 秒的传输不报告滑窗峰值。

## AirDrop 自动发送验收

`AirDropTests/testSendFile` 从系统“文件”App 搜索指定文件，通过选择与共享菜单打开 AirDrop，选中指定接收者并等待“已发送”。开始时记录可用空间和系统 Wi-Fi 页面，保留 AP 连接；文件、云端副本和安装身份均保留。板端先运行 `airdrop receive`，在收到匹配的 `request` 后接受。

真机 `xctestrun` 的 `AwareLabUITests.EnvironmentVariables` 接受这些必填输入：

| 字段 | 含义与约束 |
| --- | --- |
| `AIRDROP_FILENAME` | 文件 App 中现有文件的完整名称，非空，包含扩展名 |
| `AIRDROP_RECEIVER_NAME` | AirDrop 接收者的系统显示名，非空，与板端配置一致 |
| `AIRDROP_STEP_TIMEOUT` | 单次界面元素出现的失败期限，正数秒 |
| `AIRDROP_REQUEST_TIMEOUT` | 文件搜索、接收者发现和等待发送完成各阶段的失败期限，正数秒 |

使用已配置签名的 AwareLab scheme 构建后，以 `-only-testing:AwareLabUITests/AirDropTests/testSendFile` 运行。操作设备前持有共享租约。`send-ready`、`send-requested`、`sent` 分别表示分享界面就绪、点击接收者及系统确认完成。验收同时要求板端 `received`、最终文件字节数与原文件逐字节比较通过；UI 成功不能代替落盘验证。

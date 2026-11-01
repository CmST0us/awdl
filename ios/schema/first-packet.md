# 原生 TCP 首包契约

服务 `_fileserver._tcp` 由 ROCK 5B 发布，TCP 端口 7777。板端监听器绑定 NDI 的 IPv6 link-local 地址及接口；iPad 使用 WiFiAware 返回的端点建立连接，并读取 `NWPath.wifiAware` 与系统配对 ID 对应。

同一连接只做一次交换。UTF-8 ASCII 文本，以单个 LF 结束，每行含 LF 最长 256 字节：

```
HELLO <run_id> <message_id>\n
ACK <run_id> <message_id>\n
```

`run_id` 与 App 的运行 ID 一致，1–100 个 ASCII 字母、数字、连字符或下划线。`message_id` 为 `1`。板端收齐 HELLO 后发送相同标识的 ACK；iPad 处理 TCP 分段，只在完整匹配 ACK 后记录 `native_data_ack_received`。记录字节数包含 LF。失败、截断或不同标识均不通过。

双向首包要求板端实际接收 HELLO 与 iPad 实际接收 ACK 两份证据，关联运行 ID、消息 ID、字节数、配对身份与 NDI 会话。发送 API 完成仅记录为本端发送完成。冷配对、缓存重连、原生路径证明分别验收。

App 的解析/连接/响应失败兜底为 5 秒，完成即继续，不做自动重试。端点发现耗时未有已配对场景测量；超过兜底记录失败，后续依据实测调整。

双端记录关联命令（会话映射来自同轮协议/驱动事件）：

```sh
python3 ios/scripts/check_first_packet.py --app-events <events.jsonl> --board-events <board.jsonl> --run-id <app-run-id> --experiment-id <experiment> --paired-device-id <system-id> --ndp-session <ndp-session> --peer-nmi <ipad-nmi>
```

检查器要求实际接收事件，输出只证明关联的应用收包；安全关联、NDI 绑定与路径隔离证据仍由同轮驱动/协议记录提供。跨机单调时钟不直接比较。

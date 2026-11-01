# 原生链路批量测量

测量复用已授权 Wi-Fi Aware 设备的 TCP 7777 端点。App 在连接就绪后核对 `path.wifiAware.endpoint.device.id`，板端监听器绑定 NDI，接纳连接时核对对端 IPv6 和接口 scope。管理网络只负责部署与采集。

每次传输发送 `BULK <run_id> <sequence> GET|PUT <bytes>\n`，板端回复 `READY <run_id> <sequence> <bytes>\n`。GET 为板端到 iPad，PUT 为 iPad 到板端；正文第 i 字节为 `i % 251`，接收端逐字节比较。正文长度由请求指定，上限16MiB；App收发和板端GET供数使用256KiB缓冲，逐字节完整比较使用固定周期模式与memcmp，按实际进展每秒记录日志。流式处理不依赖TCP分段边界。PUT 接收进展达到1秒采样间隔时，板端发送 `PROGRESS <run_id> <sequence> <received_bytes>\n`，App并发读取进展与发送正文。PUT 完成后板端回复 `DONE <run_id> <sequence> <bytes> <elapsed_us>\n`。

同一个 TCP/NDP 连接按 `AWARE_BENCH_DIRECTIONS` 执行 GET、PUT，默认两个方向交替。`AWARE_BENCH_BYTES` 指定单块字节数；未设置 `AWARE_BENCH_TOTAL_BYTES` 时，`AWARE_BENCH_SECONDS` 是最低观察时长（0 表示一组，最长300秒）。设置总字节目标后，每个所选方向累计到该字节数即结束，块数不整除目标时最后一块取余数；`AWARE_BENCH_SECONDS` 同时成为完成上限，到时未达到目标按失败结束。操作由完成回调推进；GET由iPad对每个256KiB接收请求设置5秒失败兜底，PUT由板端对5秒无接收进展判失败；GET截止意味着下一整块未完成，不单独证明TCP完全没有进展，整体实验上限由联调配置另行约束。

吞吐以十进制 MB/s 报告。GET 的耗时从 App 发送请求前到收到全部正文，包含请求及响应头开销；PUT 的耗时从板端提交 READY 后到正文接收完成。两方向分别累加接收字节和耗时。另以App发送请求至收到完整正文/完成确认统一记录`down/up_end_to_end_seconds`及`down/up_end_to_end_MBps`，跨方向比较使用这组端到端字段。板端 `bulk-submitted` 只说明提交到 TCP，不能替代 iPad 的 `bulk_receiver_complete`。App 的 `benchmark.json`、`events.jsonl` 与板端同 run/sequence 的接收记录共同构成证据。

该入口按方向交替测量，不是全双工带宽测试。连接重复性单独统计。300秒/5秒滑窗/90 MB/s 的性能验收仍按设计要求判定，短样本不替代持续性能验收。

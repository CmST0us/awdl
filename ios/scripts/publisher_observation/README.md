# iPad 连接与订阅观测

单iPad源PID对照使用 `prepare_subscription_probe.py` 冻结程序及其共用服务、身份配置，再由已分配的只读窗口通过 `prepare_subscription_pid.py` 获取当前deviceaccessd PID。观察入口 `subscription_probe.py` 同时建立两条源流，收到各自StartActivity成功响应后才启动一次已安装观察用例。双流建立限1秒，包含在12秒活动和15秒总预算内；超出即结束且不启动UI。PID变化立即失败，不重试。collector复用库的JSON格式和记录解码，按其StartActivity协议在响应成功处报告ready。清理后两流共用1秒STOP/EOF预算。

源PID对照以搜索点击返回后的 `observer_search_triggered` 记录 UI 动作；marker 携带点击前的设备 `search_started_epoch`，只接纳此边界之后的 DADiscovery 与 Subscriber started 源记录；不依赖全流是否包含 App 启动日志。该事件本身不表示系统已订阅。双端发布继续使用基线查询后的 `observer_search_requested`。

STOP取消待收记录后显式断开本地activity和lockdown transport，再等待SDK资源关闭；stderr记录各关闭阶段。`collector-complete`表示已输出记录保持完整、连接关闭，不表示设备日志已全部排空。父控制器仍核对退出0与EOF，保留1秒失败兜底。

新Runner仅完成本地构建时，使用 `prepare_subscription_probe.py --built-manifest <构建输入manifest>` 冻结候选，`installed=false`；执行入口拒绝此状态。实际安装并记录新产物证据后才能进入设备窗口。

认证首帧取证可配置 `auth_capture_argv`，在本轮 `pairing_pin_typed` marker 后启动只读捕获。`stop_after_auth_rx=true` 表示收到该捕获完成事件后收口，结果标为 authentication capture；它不要求或代表系统配对成功。捕获采用5秒兜底，原始帧保存为私有 `auth-frames.jsonl`。

配对观察采用24秒活动、27秒总兜底。实测自动填PIN约在第20.3秒完成，提交再需约1.9秒；认证捕获完成事件会提前结束观察。板端部署与清理由外层30秒控制器统一约束。

RTL 驱动首帧与硬件回收的双端控制入口位于 `hardware/rtl8852be/diagnostics/control.py`。通用 iPad 观测入口仅负责系统订阅、配对状态、连接与日志采集。

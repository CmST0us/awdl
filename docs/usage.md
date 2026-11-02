# 运行

无线服务使用 `/etc/wirelessd/config.toml`，文件服务使用 `/etc/fileserver/config.toml`。所有新增字段及单位见[配置](configuration.md)。修改运行配置后重启对应服务；修改客户端契约或 Client.plist 后重新构建应用。

```sh
sudo systemctl start wirelessd fileserver
sudo wirelessd status
sudo wirelessd pair --seconds 120
```

在 AwareClient 中配对 Wi-Fi Aware 设备，输入本次窗口的 PIN。已有配对直接选择设备连接，系统通过缓存身份认证建立安全 NDP。窗口结束不撤销文件服务或已建立的传输。

```sh
sudo wirelessd close-pairing
journalctl -u wirelessd -u fileserver
```

文件列表来自配置的共享目录，支持下载、上传、取消与多连接下载。应用可独立选择发现服务名和端口；文件服务的 TCP 名称必须与客户端构建契约一致。

BE200 运行时仅允许所选 PHY 上的 NAN/NAN_DATA。RTL 保留未关联的物理控制接口及原厂 p2p0，并创建原生持久 ndi0。一套守护进程使用固定 nan0/ndi0 名称，一次管理一块网卡。

`/var/lib/wirelessd` 内的 `nan.conf` 和 `local-nik` 属于配对状态，升级保留。`/run/wirelessd` 属于运行数据。服务启动不清空共享目录，也不删除已有配对记录。

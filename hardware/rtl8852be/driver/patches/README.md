# RTL8852BE 补丁序列

基线见 `../source.json`。`series` 按顺序应用，内核代码不依赖仓库外的源码片段。

| 补丁 | 职责 |
| --- | --- |
| `0001-platform-observation.patch` | 固定平台配置与原厂观测入口 |
| `0002-native-diagnostics.patch` | 原生命令、寄存器读取与诊断入口 |
| `0003-native-nan.patch` | NAN 角色、发现、管理保护、调度、NDI 与数据完成路径 |

固件镜像由独立的固件构建阶段嵌入。补丁准备出的树保留上游固件数组；公共 `scripts/build-driver.sh rtl8852be` 入口组合完整 U2 镜像。

修改补丁后重新运行 `scripts/aware-build prepare-source rtl8852be --kernel-release <目标内核版本>`，回归直接读取生成树。新增补丁加入 `series`，保持相对于上一层的可独立应用性。共享 UAPI 变更须同时更新 `../include/wa_nan_uapi.h`，准备入口会拒绝 ABI 不一致。

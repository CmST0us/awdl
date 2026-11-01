# 驱动

RTL8852BE 的 NAN 实现，采用与 BE200 一致的固定基线加有序补丁方式。`source.json` 锁定上游归档，`patches/series` 定义补丁顺序；正式内核实现以补丁为准，生成树不纳入版本管理。

## 结构

```
hardware/rtl8852be/driver/
  source.json     固定版本、来源、归档校验与驱动子目录
  patches/        平台观测、原生诊断、完整 NAN 的有序补丁
  include/        用户态共享 UAPI，与补丁内内核副本核对
  构建输出位于 .build/targets/<target>/drivers/rtl8852be/<kernel>/source
```

## 补丁后的 NAN 源码

| 文件 | 内容 |
| --- | --- |
| `wa_nan_cycle.h` | 角色、端口、func0/1/2 序列 |
| `wa_nan_hal.h` | HAL 接口 |
| `wa_nan_h2c.h` | NAN H2C 命令 |
| `wa_nan_mgmt.h` | 管理帧 MACID 与发送 |
| `wa_nan_bcn_template.h` | 63 字节信标模板 |
| `wa_nan_sdf.h` | 163/183 字节 SDF 构造 |
| `wa_nan_sha256.h` | HMAC-SHA-256，NIRA 用 |
| `wa_nan_pairing.h` | NIRA、pairing SDF、PIN、bootstrap |
| `wa_nan_mgmt_rx.h` | NAN 管理帧接收 ring |
| `wa_nan_stageb.h` | 启用流程与事件驱动状态机 |
| `wa_nan_stageb_api.h` | 对外接口 |
| `wa_nan_cap.h` | 能力检查 |
| `wa_nan_ndi.h` | NDI网卡、peer/密钥生命周期与数据队列 |
| `wa_nan_phy.h` | 关联NAN peer的PHY统计，保留原始RX元数据 |
| `wa_nan_data_ccmp.h` | 三地址数据CCMP、逐TID重放与MIC校验 |

## 构建

见 [BUILD.md](BUILD.md)。公共入口 `scripts/build-driver.sh rtl8852be` 负责源码、完整固件、Kbuild 和最终嵌入核对；本机输入由 `config/local/build.toml` 维护。

## 约束

- 原生 NAN，不依赖 cfg80211 的 NAN 角色。
- 管理帧发送走 PHL txreq 与 txfb completion。
- 接收在 `phl_rx.c` capture 到 ring，由 `nan-rx` 排空。
- 生产路径事件驱动；`wait_done` 只作失败兜底。

## 控制接口

Vendor 命令、完成语义和已知限制见 [NAN-UAPI.md](NAN-UAPI.md)，唯一属性定义为 `include/wa_nan_uapi.h`。

用户态 hostap RTL 后端位于独立组件 [hostap RTL 后端](../hostap/README.md)。

## source.json 字段

`name` / `version` 为驱动标识与版本；`license` 为驱动授权标识；`repository` / `commit` 固定 Radxa 来源；`archive` 为归档地址；`sha256` 为原项目的归档校验，避免应用到错误基线；`directory` 为归档内的驱动子目录。修改来源或补丁后重新准备和构建。

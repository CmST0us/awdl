# U2 正式补丁序列

`series` 按顺序列出正式补丁描述，输入固定为 `../source.json` 指定的原始 U2 二进制。每一步以上一步的镜像为输入，生成新的 `candidate.bin`；最终镜像由 `tools/build_nan.py` 统一输出并可嵌入驱动数组。

| 补丁 | 内容 |
| --- | --- |
| `0001-dw-phase.json` | 校验原字节并调整 DW 起点 |
| `0002-nan-stop.json` | 原生启动、预约与完整 STOP |
| `0003-beacon.json` | 原生信标入口，关闭逐信标诊断输出 |
| `0004-faw-refresh.json` | FAW 更新 |
| `0005-bigtk.json` | BIGTK 保护、成员门控与双信道状态 |
| `0006-faw-anchor.json` | FAW anchor，固定注入地址 |

原厂基线没有对应 C 源码，补丁采用二进制字节替换及 MIPS16e/C 编译注入。JSON 描述固定顺序、构建模块与参数，具体代码在本目录的 `.S`、`.c`、`.h` 中，链接和镜像写入由 `../tools/` 的对应模块完成。修改后从基线完整重建，安全段摘要由工具更新。

`experiments/` 和未列入 `series` 的诊断工具不参与正式构建。`../u2/rtl8852be_fw_nan.bin` 是已验收交付镜像，不能作为正式补丁链的输入；发布更新时同时更新镜像与 `VERSION.nan`。

## JSON 字段

每步的 `name` 是构建输出子目录名。字节替换步骤的 `offset` 为原镜像内十六进制字节偏移，`expected` 是该位置必须匹配的原字节，`replacement` 是等长替换字节。编译注入步骤的 `module` 为 tools 内执行该步的 Python 模块，`options` 为传入参数。

`0003` 的 `options.diagnostic` 控制逐信标诊断，正式镜像为 false。`0006` 的 `options.target` 为 FAW anchor 的固件虚拟注入地址，单位字节，须位于该补丁预留的代码区。修改任何字段后从原始镜像重建并核对模块嵌入结果。

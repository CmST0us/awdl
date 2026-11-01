# U2 自定义固件注入

正常 FWDL 可以加载含自定义代码的 U2 镜像，自定义代码也能在固件里实际执行。安全段摘要重算通过后，芯片既接受改一个立即数，也接受放一段新函数并把控制流劫持过去。

## 结论

本机非安全芯片（`is_sec_ic=0`）的正常下载只比对安全段明文摘要，不验证 MSS 签名。修改代码段、注入自定义 MIPS16e 代码，只要同步重算摘要，都能完成下载、启动并执行。

实测覆盖了三种镜像：

| 镜像 | SHA-256 | 加载 | 执行 |
| --- | --- | --- | --- |
| 调度日志立即数 | `f30cf51f…` | 通过 | `line: 2971` 打印 |
| 安全段注入 | `d73a16eb…` | 通过 | 取指异常（该区不可执行） |
| 代码段 stub 注入 | `c2731db4…` | 通过 | `line: 4095` 打印 |

`WA_OBS download sha256=` 与各自候选逐字节一致，排除“加载了别的固件”。

## 可执行注入的最终做法

安全段 `0x3C0`–`0x7FF` 虽然随段加载，但**不可执行**：把函数放进去后，`jal` 跳过去立即触发取指异常，固件 SER 日志给出 `FW EPC/RA = 0xb8e11ce9`、`FW status = 0xdd001300`。连只有 `jr ra` 的两字节版本也一样。这个区域只能写数据，不能放代码。

可执行的位置在代码段内部。选调试命令表里的 stub `0xb89a69fc`（原内容 `jr ra; li v0,1`，8 字节，只在调试命令下调用）：

- `0x3B796` 的 `li a0,2953` 替换为 `jal 0xB89A69FC`；
- `0x36A9C` 的 stub 覆盖为 `li a0,4095; jr ra; nop`（8 字节）。

START 进入调度函数后跳到 stub，把行号改成 4095 再返回，原流程继续。

```sh
mipsel-linux-gnu-as -EL -mips32r2 -mabi=32 -o custom.o custom.S
mipsel-linux-gnu-objcopy -O binary -j .text custom.o custom.bin
python3 hardware/rtl8852be/firmware/tools/u2_custom_firmware.py hardware/rtl8852be/firmware/u2/rtl8852be_fw.bin custom.bin out.bin \
  --hook-hex d1187f9a --code-offset 0x36A9C --allow-overwrite \
  --array-input <基线 hal8852b_fw.c> --array-output <候选 hal8852b_fw.c>
```

`--hook-hex` 是对应目标地址的 4 字节 `jal`，`--code-offset` 是代码写入位置；省略时仍用安全段空白区。工具拒绝覆盖非零的安全段空白区，写完后重算两份摘要。

## 证据

`.private/artifacts/U2-FW-BYPASS-001/`：

| 窗口 | 结果 |
| --- | --- |
| `20261023-STUB-02` | PASS：`line: 4095` 两条、`host-start result=0 cookie=1`、原生 STOP、138 参数恢复读回一致 |
| `20261023-RET-01` | 安全段不可执行：`FW EPC/RA = 0xb8e11ce9`、SER |
| `20261023-CUSTOM-01/02/03` | 安全段注入镜像加载成功、执行取指异常 |
| `20261023-CONTROL-03` | marker 固件同窗口 PASS，用于排除环境因素 |
| `20261023-DIGEST-01/02` | 修改类镜像加载，DIGEST-02 有 marker 执行 |

STUB-02 的模块 srcversion 为 `A216D896819D4B7559FDF63`，镜像 SHA-256 `c2731db4c0e12c1298598271910a6a58323e0fffac4ba6805153fd17f8a5a7a3`。每个窗口的 `archive/` 保留板端完整记录。

## 判定与对照

| 组 | 观测 | 结论 |
| --- | --- | --- |
| 阴性对照 | 不重算摘要的修改 3 次 `FWDL_SECURITY_FAIL(73)` | 校验真实生效 |
| 阳性对照 | 重算摘要的 5 窗口 10 次下载全部 `result=0`、`init-ready` | 校验被绕过 |
| 运行时 | `line: 2971`、`line: 4095` | 修改后的镜像确实在运行 |
| 区域对照 | 安全段 `jal` 即 SER，代码段 stub `jal` 正常 | 可执行区在代码段 |

## 边界

- 只验证 `is_sec_ic=0` 的非安全芯片；安全芯片与 `mss_idx>0` 分支未验证。
- 注入复用现有 8 字节 stub，没有扩展镜像几何或新增 section；更大的自定义代码需要另找可执行空间。
- 被覆盖的 stub 属于调试命令表，不参与正常 NAN 流程。
- 扫描互斥的完成回调比 GO 晚约 17 秒，START 需要在扫描 release 后立即发起；这是驱动时序问题，不影响固件结论。

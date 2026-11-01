# U2 固件修改准入：重算安全段摘要

基线 `hardware/rtl8852be/firmware/u2/rtl8852be_fw.bin`，325304 字节。本机芯片被驱动识别为非安全配置（`is_sec_ic=0`），正常 FWDL 仍会因镜像改动返回 `FWDL_SECURITY_FAIL`（73）。原因不是签名不可伪造，而是安全段内的明文摘要未同步更新。

## 安全段结构

段 2（type 9）`0x4eab8`–`0x4f2b8`，2048 字节：

| 偏移 | 长度 | 内容 |
| --- | ---: | --- |
| `0x000` | 16 | 头：`c0 ff fc ff ff ff 00…`，末两字节 `c0 01` 是签名偏移 `0x1C0` |
| `0x010`–`0x03f` | 48 | 全 `0xff` / 全 `0` 填充 |
| `0x040` | 64 | `SHA256(代码段) || SHA256(数据段)` |
| `0x100` | 64 | 同一摘要的副本 |
| `0x1c0` | 512 | MSS 签名槽，驱动在下载前用文件尾按 key index 选中的签名覆盖 |
| `0x3c0`–`0x7ff` | — | 全 0 |

摘要覆盖 `0xa0`–`0x4b540`（代码段）和 `0x4b540`–`0x4eab8`（数据段），不含 FWHDR、动态头与安全段自身。文件尾 `0x4f2b8` 起两份 512 字节签名槽，`mss_idx` 由 efuse 的 external PN/customer/serial 决定。

## 驱动侧处理

`phl/hal_g6/mac/mac_ax/fwdl.c:fwhdr_parser()` 对 8852B 把安全段长度强制为 2048，并按 `FWDL_SECURITY_SECTION_CONSTANT = 64 + 6×32×2 = 0x1C0` 把选中的签名拷进安全段。`check_fw_rdy()` 只读 `R_AX_WCPU_FW_CTRL` 的 `B_AX_WCPU_FWDL_STS`，收到 `FWDL_SECURITY_FAIL` 就返回 `MACFWSECBOOT`；校验由芯片侧执行，驱动没有本地验签。

## 绕过

修改代码段后重算两份摘要即可通过正常下载。本机实测表明该校验只比对明文摘要，不验证 MSS 签名：同一份被拒绝的字节修改，在摘要同步后 `attempt-end result=0`、`init-ready`，改动点按 marker 执行。

工具：

```sh
python3 hardware/rtl8852be/firmware/tools/u2_digest_bypass.py hardware/rtl8852be/firmware/u2/rtl8852be_fw.bin <候选.bin> \
  --array-input <基线 hal8852b_fw.c> --array-output <候选 hal8852b_fw.c>
```

工具只改 `0x3B798` 的调度日志立即数（`09`→`1B`，line 2953→2971）并重算摘要，用于准入与执行验证；替换内嵌数组、重建模块后按正常流程加载。

## 证据

`.private/artifacts/U2-FW-BYPASS-001/20261023-DIGEST-02/`：候选 SHA-256 `f30cf51f…` 与内核 `WA_OBS download sha256=` 一致，两次 `attempt-end result=0`，`hal_mac_print_fw_version … Ready`，`C2H log: [NAN] FAIL in func: 255, line: 2971, ret = 0` 与 `ret = 7` 各一条，原生 STOP 与 138 参数恢复读回一致。同目录 `archive/` 保留板端完整记录。

`20261023-DIGEST-01`、`20261023-CUSTOM-01/02/03`、`20261023-RET-01`、`20261023-STUB-02` 同样 `result=0` 并 `init-ready`，其中 CUSTOM/RET/STUB 加载的是注入自定义代码的镜像；合计九次下载尝试，零失败。`20261023-STUB-02` 另外取得注入代码 `line: 4095` 的运行时执行证据。注入方式见 [u2-custom-firmware](u2-custom-firmware.md)。

## 边界

- 只验证了本机这张 `is_sec_ic=0` 的非安全芯片。安全芯片是否验签、以及 `mss_idx>0` 分支的行为没有证据。
- 摘要校验的覆盖范围由镜像中的明文摘要反推，未从 ROM 侧确认；新增内容仍受镜像几何与启动表约束。
- 本结论只解决“正常下载是否接受修改后的镜像”，不涉及 `mac_ram_boot`、逐帧回调或运行时代码注入。

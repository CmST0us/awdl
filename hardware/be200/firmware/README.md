# 固件

| 文件 | 用途 |
| --- | --- |
| `iwlwifi-gl-c0-fm-c0-c108.ucode` | BE200 GL C0 / FM C0，运行版本108.45105872.0 |
| `iwlwifi-gl-c0-fm-c0.pnvm` | 对应设备的PNVM平台数据 |

来自目标板已验证的固件组合，与 `source.json` 固定的 linux-firmware 提交逐字节一致。上游发布记录见 [linux-firmware WHENCE](https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+/24247053afda2bab6b4ace42402dbf9f3b1305d6/WHENCE)。二进制保持原样，授权条件见 `LICENCE.iwlwifi_firmware`。安装到 `/lib/firmware` 后，在下次驱动加载时生效。

当前交付和真机验收范围是 GL C0 / FM C0。PCI `8086:272b` 还覆盖其他 stepping，实际固件前缀由驱动读取的 MAC/RF 修订选择；B0/B0 等组合需要对应固件及独立验收，不能复用本组合的能力结论。

## source.json 字段

`repository` / `commit` 固定固件来源；`version` 为固件版本；`license` 指向原始再分发条款。`files` 列出交付文件，每项的 `file` 是当前目录文件名，`upstream_path` 是固定来源中的路径。更改固件后重新安装并加载对应驱动。

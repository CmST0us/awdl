# 固件信标CMAC

`u2_bip.c`提供无运行库依赖的AES-CMAC-128计算，密钥上下文保存AES轮密钥和两个CMAC子密钥。`u2_beacon_mic`接收包含完整18字节MMIE的beacon，输出8字节MIC；调用者负责有效帧长度、Key ID、IPN分配和发送生命周期。

CMAC依据[RFC 4493](https://www.rfc-editor.org/rfc/rfc4493)。信标输入按[Linux mac80211的BIP实现](https://github.com/torvalds/linux/blob/v6.1/net/mac80211/aes_cmac.c)处理：AAD为屏蔽Retry/Power Management/More Data后的Frame Control及三个地址；Timestamp和末尾MIC按零输入，Duration与Sequence Control不参与。BSSID、NAN Master/Cluster属性及MMIE的Key ID/IPN保留在认证输入中。

## 构建与验证

使用项目已有`flange-build:latest`中的Clang生成MIPS16汇编，转换为U2原生栈操作、延迟槽跳转与立即数寻址，再由GNU binutils组装。输入和输出均位于仓库内，中间文件放`.private/`。每个外部命令的失败兜底为5秒。只读表由最终链接放入数据区，不能从代码扩展区读取。

```sh
python3 hardware/rtl8852be/firmware/tools/u2_compile_c.py hardware/rtl8852be/firmware/patches/u2_bip.c .private/bip/u2_bip.o
python3 hardware/rtl8852be/firmware/tools/test_u2_bip.py .private/bip-test \
  --beacon <原生63字节信标文件> \
  --openssl /opt/homebrew/opt/openssl@3/bin/openssl --mips
```

测试覆盖4组RFC向量、8种长度的OpenSSL对照、真实信标的屏蔽字段及认证字段修改，并通过QEMU执行实际MIPS16对象。外部`memset`由最终固件提供。目标指令证据为`.private/artifacts/M5-BIGTK-INTEGRATION-02/cmac-native/`。

## 接入状态

算法已接入[原生BIGTK](u2-bigtk.md)的独立签名缓冲区、共享IPN及TX完成路径；单板实测355µs。生产主机密钥回调与GET_SEQ仍待接入，完整安全启动保持未支持错误。

#!/usr/bin/env python3
"""向 U2 固件注入一段自定义 MIPS16e 代码并重算安全段摘要。

自定义代码写入安全段 0x3C0 起的空白区（VA 0xb8E11CE8），该区不在摘要覆盖
范围内且不属于签名槽。代码段 0x3B796 的 `li a0,2953` 被替换为
`jal 0xb8E11CE8`，调用后返回原流程。工具保留镜像长度并重算两份明文摘要。
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

HOOK_OFFSET = 0x3B796
HOOK_OLD = bytes.fromhex('81 f3 09 6c')
HOOK_NEW = bytes.fromhex('11 1b 3a 47')  # MIPS16e jal 0xb8e11ce8
CODE_OFFSET = 0x4EAB8 + 0x3C0
CODE_MAX = 0x800 - 0x3C0
CODE_VA = 0xB8E11928 + 0x3C0

CODE_RANGE = (0xA0, 0x4B540)
DATA_RANGE = (0x4B540, 0x4EAB8)
DIGEST_OFFSETS = (0x4EAB8 + 0x40, 0x4EAB8 + 0x100)
IMAGE_SIZE = 325304


def digest(image):
    return (hashlib.sha256(image[CODE_RANGE[0]:CODE_RANGE[1]]).digest()
            + hashlib.sha256(image[DATA_RANGE[0]:DATA_RANGE[1]]).digest())


def patch_array(source, image, candidate):
    match = re.search(r'u8 array_8852b_u2_nicce\[\] = \{(.*?)\n\};', source, re.S)
    if not match:
        raise SystemExit('U2 内嵌数组不存在')
    tokens = list(re.finditer(r'0x([0-9a-fA-F]{2})', match[1]))
    if bytes(int(t[1], 16) for t in tokens) != image:
        raise SystemExit('内嵌数组与基线镜像不一致')
    body = match.group(1)
    edits = sorted((i, image[i], candidate[i]) for i in range(len(image))
                   if image[i] != candidate[i])
    for index, _old, new in reversed(edits):
        token = tokens[index]
        start, end = match.start(1) + token.start(), match.start(1) + token.end()
        body = body[:start - match.start(1)] + f'0x{new:02X}' + body[end - match.start(1):]
    return source[:match.start(1)] + body + source[match.end(1):]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='基线 U2 镜像')
    parser.add_argument('code', type=Path, help='MIPS16e 自定义代码二进制')
    parser.add_argument('output', type=Path, help='候选 U2 镜像，必须不存在')
    parser.add_argument('--array-input', type=Path, help='冻结源树中的 hal8852b_fw.c')
    parser.add_argument('--array-output', type=Path, help='输出的内嵌数组源码')
    parser.add_argument('--hook-hex', help='替换调度日志指令的 4 字节 jal，十六进制')
    parser.add_argument('--code-offset', type=lambda x: int(x, 0),
                        help='自定义代码写入的文件偏移；默认安全段空白区')
    parser.add_argument('--allow-overwrite', action='store_true',
                        help='允许覆盖代码区原有字节（用于复用现有函数空间）')
    args = parser.parse_args()
    if bool(args.array_input) != bool(args.array_output):
        parser.error('array-input 与 array-output 必须同时指定')

    original = args.source.read_bytes()
    code = args.code.read_bytes()
    if len(original) != IMAGE_SIZE:
        parser.error(f'输入长度 {len(original)} 不是 {IMAGE_SIZE}')
    if original[HOOK_OFFSET:HOOK_OFFSET + 4] != HOOK_OLD:
        parser.error(f'偏移 {HOOK_OFFSET:#x} 不是预期的调度日志指令')
    if not code or len(code) > CODE_MAX:
        parser.error(f'自定义代码长度必须在 1..{CODE_MAX}')
    hook = bytes.fromhex(args.hook_hex) if args.hook_hex else HOOK_NEW
    if len(hook) != 4:
        parser.error('hook 必须是 4 字节')
    code_offset = args.code_offset if args.code_offset is not None else CODE_OFFSET
    if code_offset + len(code) > IMAGE_SIZE:
        parser.error('代码超出镜像')
    if code_offset == CODE_OFFSET and not args.allow_overwrite:
        if original[CODE_OFFSET:CODE_OFFSET + CODE_MAX] != bytes(CODE_MAX):
            parser.error('安全段空白区不是全零，拒绝覆盖')

    candidate = bytearray(original)
    candidate[HOOK_OFFSET:HOOK_OFFSET + 4] = hook
    candidate[code_offset:code_offset + len(code)] = code
    value = digest(candidate)
    for offset in DIGEST_OFFSETS:
        candidate[offset:offset + 64] = value
    if digest(candidate) != bytes(candidate[DIGEST_OFFSETS[0]:DIGEST_OFFSETS[0] + 64]):
        raise SystemExit('候选摘要自检失败')

    if args.array_input:
        if args.array_input.resolve() == args.array_output.resolve():
            parser.error('数组输入与输出必须分开')
        source = args.array_input.read_text()
        patched = patch_array(source, original, bytes(candidate))
        with args.array_output.open('x') as output:
            output.write(patched)

    with args.output.open('xb') as output:
        output.write(candidate)
    print(json.dumps({
        'source': str(args.source), 'candidate': str(args.output),
        'bytes': len(candidate), 'code_bytes': len(code),
        'code_offset': hex(code_offset), 'hook_offset': hex(HOOK_OFFSET),
        'hook': hook.hex(' '),
        'digest': value.hex(),
    }, indent=2))


if __name__ == '__main__':
    main()

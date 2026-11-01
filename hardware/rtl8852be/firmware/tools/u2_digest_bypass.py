#!/usr/bin/env python3
"""生成重算安全段摘要的 U2 候选。

U2 安全段（type 9）的 0x40 与 0x100 处各存一份 64 字节明文摘要：
SHA256(代码段) || SHA256(数据段)。正常 FWDL 时芯片按该摘要校验镜像。
本工具修改代码段中的 NAN 调度日志立即数，并重算两份摘要，用于检验
非安全芯片（is_sec_ic=0）的正常下载路径是否只比对明文摘要。
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

MARKER_ADDRESS = 0xB89AB6F6
INSTRUCTION_OFFSET = MARKER_ADDRESS - 0xB8970000 + 0xA0
MARKER_OLD = 0x09
MARKER_NEW = 0x1B
MARKER_INSTRUCTION = bytes.fromhex('81 f3 09 6c')

CODE_RANGE = (0xA0, 0x4B540)
DATA_RANGE = (0x4B540, 0x4EAB8)
DIGEST_OFFSETS = (0x4EAB8 + 0x40, 0x4EAB8 + 0x100)
DIGEST_LEN = 64
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
    return source[:match.start(1)] + body + source[match.end(1):], edits


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='基线 U2 镜像')
    parser.add_argument('output', type=Path, help='候选 U2 镜像，必须不存在')
    parser.add_argument('--array-input', type=Path, help='冻结源树中的 hal8852b_fw.c')
    parser.add_argument('--array-output', type=Path, help='输出的内嵌数组源码')
    args = parser.parse_args()
    if bool(args.array_input) != bool(args.array_output):
        parser.error('array-input 与 array-output 必须同时指定')

    original = args.source.read_bytes()
    if len(original) != IMAGE_SIZE:
        parser.error(f'输入长度 {len(original)} 不是 {IMAGE_SIZE}')
    if original[INSTRUCTION_OFFSET:INSTRUCTION_OFFSET + 4] != MARKER_INSTRUCTION:
        parser.error(f'偏移 {INSTRUCTION_OFFSET:#x} 不是预期的调度日志指令')

    candidate = bytearray(original)
    candidate[INSTRUCTION_OFFSET + 2] = MARKER_NEW
    value = digest(candidate)
    if len(value) != DIGEST_LEN:
        raise SystemExit('摘要长度异常')
    for offset in DIGEST_OFFSETS:
        candidate[offset:offset + DIGEST_LEN] = value

    if digest(candidate) != bytes(candidate[DIGEST_OFFSETS[0]:DIGEST_OFFSETS[0] + DIGEST_LEN]):
        raise SystemExit('候选摘要自检失败')

    embedded = None
    edits = []
    if args.array_input:
        source = args.array_input.read_text()
        if args.array_input.resolve() == args.array_output.resolve():
            parser.error('数组输入与输出必须分开')
        embedded, edits = patch_array(source, original, bytes(candidate))
        if len(edits) != 65:
            raise SystemExit(f'预期 65 处字节变化，实际 {len(edits)}')

    with args.output.open('xb') as output:
        output.write(candidate)
    if embedded is not None:
        with args.array_output.open('x') as output:
            output.write(embedded)

    print(json.dumps({
        'source': str(args.source), 'candidate': str(args.output),
        'bytes': len(candidate),
        'marker': {'address': hex(MARKER_ADDRESS), 'offset': hex(INSTRUCTION_OFFSET + 2),
                   'old': hex(MARKER_OLD), 'new': hex(MARKER_NEW),
                   'old_line': 2953, 'marker_line': 2971},
        'digest_offsets': [hex(x) for x in DIGEST_OFFSETS],
        'digest': value.hex(),
        'code_sha256': hashlib.sha256(candidate[CODE_RANGE[0]:CODE_RANGE[1]]).hexdigest(),
        'data_sha256': hashlib.sha256(candidate[DATA_RANGE[0]:DATA_RANGE[1]]).hexdigest(),
        'changed_bytes': len(edits),
        'changed_offsets': [hex(i) for i, _o, _n in edits],
    }, indent=2))


if __name__ == '__main__':
    main()

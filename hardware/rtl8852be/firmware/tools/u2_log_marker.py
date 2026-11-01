#!/usr/bin/env python3
"""生成 U2 原生调度日志的等长准入候选。"""
import argparse
import json
import re
from pathlib import Path

ADDRESS = 0xB89AB6F6
OFFSET = ADDRESS - 0xB8970000 + 0xA0
OLD = bytes.fromhex('81 f3 09 6c')
NEW = bytes.fromhex('81 f3 1b 6c')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--array-input', type=Path, help='冻结源树中的 hal8852b_fw.c')
    parser.add_argument('--array-output', type=Path, help='独立输出的内嵌数组源码')
    args = parser.parse_args()
    if bool(args.array_input) != bool(args.array_output):
        parser.error('array-input 与 array-output 必须同时指定')
    original = args.source.read_bytes()
    if args.source.resolve() == args.output.resolve():
        parser.error('输入与输出必须分开')
    if len(original) != 325304 or original[OFFSET:OFFSET + 4] != OLD:
        parser.error('输入不是预期 U2 指令与长度')
    candidate = bytearray(original)
    candidate[OFFSET:OFFSET + 4] = NEW
    embedded = None
    if args.array_input:
        if args.array_input.resolve() == args.array_output.resolve():
            parser.error('数组输入与输出必须分开')
        source = args.array_input.read_text()
        match = re.search(r'u8 array_8852b_u2_nicce\[\] = \{(.*?)\n\};', source, re.S)
        if not match:
            parser.error('U2 内嵌数组不存在')
        values = list(re.finditer(r'0x([0-9a-fA-F]{2})', match[1]))
        if bytes(int(x[1], 16) for x in values) != original:
            parser.error('内嵌数组与基线镜像不一致')
        token = values[OFFSET + 2]
        start, end = match.start(1) + token.start(), match.start(1) + token.end()
        embedded = source[:start] + '0x1B' + source[end:]
    with args.output.open('xb') as output:
        output.write(candidate)
    if embedded is not None:
        with args.array_output.open('x') as output:
            output.write(embedded)
    print(json.dumps({'source': str(args.source), 'candidate': str(args.output),
                      'bytes': len(candidate), 'instruction_address': hex(ADDRESS),
                      'instruction_offset': hex(OFFSET), 'old': OLD.hex(' '),
                      'new': NEW.hex(' '), 'old_line': 2953, 'marker_line': 2971,
                      'differences': [{'offset': hex(i), 'old': a, 'new': b}
                                      for i, (a, b) in enumerate(zip(original, candidate)) if a != b]}, indent=2))


if __name__ == '__main__':
    main()

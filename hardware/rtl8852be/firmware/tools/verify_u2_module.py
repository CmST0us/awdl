#!/usr/bin/env python3
"""从 AArch64 模块符号提取 U2，直接比对指定镜像。"""
import argparse
import json
from pathlib import Path
import struct


def extract(data):
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    if header[0][:6] != b'\x7fELF\x02\x01' or header[1:3] != (1, 183):
        raise ValueError('需要小端 ELF64 AArch64 可重定位模块')
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, header[6] + i*header[11])
                for i in range(header[12])]
    found = []
    for section in sections:
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        names = data[strings[4]:strings[4]+strings[5]]
        for offset in range(section[4], section[4]+section[5], section[9]):
            name, info, other, index, value, size = struct.unpack_from('<IBBHQQ', data, offset)
            if names[name:].split(b'\0', 1)[0] != b'array_8852b_u2_nicce':
                continue
            target = sections[index]
            start = target[4]+value
            found.append((start, data[start:start+size]))
    if len(found) != 1:
        raise ValueError('模块必须含唯一 U2 数组符号')
    return found[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('module', type=Path)
    parser.add_argument('firmware', type=Path)
    parser.add_argument('--extract', type=Path)
    args = parser.parse_args()
    offset, embedded = extract(args.module.read_bytes())
    expected = args.firmware.read_bytes()
    equal = embedded == expected
    if args.extract:
        with args.extract.open('xb') as output:
            output.write(embedded)
    print(json.dumps(dict(module=str(args.module), firmware=str(args.firmware),
                          symbol='array_8852b_u2_nicce', file_offset=hex(offset),
                          embedded_bytes=len(embedded), expected_bytes=len(expected),
                          equal=equal), indent=2))
    return 0 if equal else 1


if __name__ == '__main__':
    raise SystemExit(main())

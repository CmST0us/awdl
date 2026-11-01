#!/usr/bin/env python3
"""rtl8852be U2 固件开发工具链。

覆盖镜像解析、摘要重算、字节修改、代码注入、数组嵌入与模块验证。
自定义代码写入已知可复用槽位，入口用 MIPS16e jal 劫持，摘要自动重算。
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

IMAGE_SIZE = 325304
HOOK_OFFSET = 0x3B796
HOOK_OLD = bytes.fromhex('81 f3 09 6c')
HOOK_PREFIX = bytes.fromhex('81 f3')
HOOK_VA = 0xB89AB6F6
CODE_RANGE = (0xA0, 0x4B540)
DATA_RANGE = (0x4B540, 0x4EAB8)
SEC_OFFSET = 0x4EAB8
DIGEST_OFFSETS = (SEC_OFFSET + 0x40, SEC_OFFSET + 0x100)
SEC_VA = 0xB8E11928

# 已确认可复用的代码槽：地址、文件偏移、容量、原用途。
SLOTS = {
    'stub': {'va': 0xB89A69FC, 'offset': 0x36A9C, 'size': 8,
             'note': 'NANPSTIMERTEST 调试 stub，仅调试命令表引用'},
    'debug-handler': {'va': 0xB89A84FC, 'offset': 0x3859C, 'size': 0x11C,
                      'note': 'NANRCRECSYNC 调试 handler，仅调试命令表引用'},
    'secure-gap': {'va': SEC_VA + 0x3C0, 'offset': SEC_OFFSET + 0x3C0,
                   'size': 0x800 - 0x3C0,
                   'note': '安全段空白区；数据可用，不可执行（jal 即 SER）'},
}
DATA_SLOTS = {'secure-gap'}


def read_image(path, size=None):
    data = Path(path).read_bytes()
    if size is not None and len(data) != size:
        raise SystemExit(f'{path}: 长度 {len(data)} 不是 {size}')
    return data


HEADER_BASE = 0x20
SECTION_LEN = 0x10


def header_fields(image):
    import struct
    dw3 = struct.unpack_from('<I', image, 0x0C)[0]
    dw6 = struct.unpack_from('<I', image, 0x18)[0]
    return {'hdr_len': (dw3 >> 16) & 0xFF, 'section_num': (dw6 >> 8) & 0xFF}


def sections(image):
    import struct
    rows = []
    for i in range(header_fields(image)['section_num']):
        dw0, dw1, dw2, _dw3 = struct.unpack_from(
            '<IIII', image, HEADER_BASE + i * SECTION_LEN)
        rows.append({'va': dw0, 'dladdr': dw0 & 0x1FFFFFFF, 'length': dw1 & 0xFFFFFF,
                     'flags': dw1 >> 24, 'type': (dw1 >> 24) & 0xF, 'mssc': dw2})
    return rows


def section_offsets(image):
    """各 section 在文件中的起始偏移，按描述顺序累加。"""
    positions = []
    pos = header_fields(image)['hdr_len']
    for row in sections(image):
        positions.append(pos)
        pos += row['length']
    return positions


def layout(image):
    """按 section 描述返回 (code_len, data_len)。"""
    rows = sections(image)
    return rows[0]['length'], rows[1]['length']


def digest_layout(image, code_len, data_len):
    base = header_fields(image)['hdr_len']
    code = image[base:base + code_len]
    data = image[base + code_len:base + code_len + data_len]
    return hashlib.sha256(code).digest() + hashlib.sha256(data).digest()


def digest(image):
    code_len, data_len = layout(image)
    return digest_layout(image, code_len, data_len)


def security_offset(image):
    rows = sections(image)
    for index, row in enumerate(rows):
        if row['type'] == 9:
            return section_offsets(image)[index]
    raise SystemExit('镜像缺少安全段')


def with_digest(image):
    code_len, data_len = layout(image)
    sec_off = security_offset(image)
    out = bytearray(image)
    value = digest_layout(out, code_len, data_len)
    for offset in (sec_off + 0x40, sec_off + 0x100):
        out[offset:offset + 64] = value
    return bytes(out)


def render_array(image, template=None):
    lines = []
    for i in range(0, len(image), 8):
        lines.append(''.join(f'0x{b:02X}, ' for b in image[i:i + 8]).rstrip())
    body = 'u8 array_8852b_u2_nicce[] = {\n' + '\n'.join(lines) + '\n};\n'
    if template:
        match = re.search(r'u8 array_8852b_u2_nicce\[\] = \{.*?\n\};', template, re.S)
        if not match:
            raise SystemExit('模板缺少 U2 数组声明')
        return template[:match.start()] + body.rstrip('\n') + template[match.end():]
    return body


def assemble_jal(target_va, at_va):
    """用 binutils 生成 at_va 处跳转到 target_va 的 MIPS16e jal。"""
    assembler = shutil.which('mipsel-linux-gnu-as')
    linker = shutil.which('mipsel-linux-gnu-ld')
    objcopy = shutil.which('mipsel-linux-gnu-objcopy')
    if not all((assembler, linker, objcopy)):
        raise SystemExit('需要 mipsel-linux-gnu-as/ld/objcopy 生成 jal')
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'h.S').write_text(
            f'\t.set noreorder\n\t.set mips16\n\t.text\n\tjal\t{target_va:#x}\n\tnop\n')
        (tmp / 'l.ld').write_text(f'SECTIONS {{ . = {at_va:#x}; .text : {{ *(.text) }} }}\n')
        subprocess.run([assembler, '-EL', '-mips32r2', '-mabi=32',
                        '-o', str(tmp / 'h.o'), str(tmp / 'h.S')], check=True, timeout=5)
        subprocess.run([linker, '-T', str(tmp / 'l.ld'), '-o', str(tmp / 'h.elf'),
                        str(tmp / 'h.o')], check=True, timeout=5)
        subprocess.run([objcopy, '-O', 'binary', '-j', '.text',
                        str(tmp / 'h.elf'), str(tmp / 'h.bin')], check=True, timeout=5)
        code = (tmp / 'h.bin').read_bytes()[:4]
    if len(code) != 4:
        raise SystemExit('jal 生成失败')
    return code


def extract_symbol(module, symbol):
    import struct
    data = Path(module).read_bytes()
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    if header[0][:6] != b'\x7fELF\x02\x01' or header[1:3] != (1, 183):
        raise SystemExit('需要小端 ELF64 AArch64 可重定位模块')
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, header[6] + i * header[11])
                for i in range(header[12])]
    found = []
    for section in sections:
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        for offset in range(section[4], section[4] + section[5], section[9]):
            name, info, other, index, value, size = struct.unpack_from('<IBBHQQ', data, offset)
            if names[name:].split(b'\0', 1)[0] != symbol.encode('ascii'):
                continue
            target = sections[index]
            start = target[4] + value
            found.append((start, data[start:start + size]))
    if len(found) != 1:
        raise SystemExit(f'模块必须含唯一符号 {symbol}')
    return found[0]


def extract_embedded(module):
    return extract_symbol(module, 'array_8852b_u2_nicce')


def cmd_info(args):
    image = read_image(args.image)
    rows = sections(image)
    offsets = section_offsets(image)
    stored = b''
    for index, row in enumerate(rows):
        if row['type'] == 9:
            stored = image[offsets[index] + 0x40:offsets[index] + 0x80]
    info = {
        'path': args.image,
        'size': len(image),
        'sha256': hashlib.sha256(image).hexdigest(),
        'header': header_fields(image),
        'sections': [{'index': index, 'va': f'{row["va"]:#x}',
                      'dladdr': f'{row["dladdr"]:#x}', 'length': row['length'],
                      'type': row['type'], 'flags': f'{row["flags"]:#04x}',
                      'offset': f'{offsets[index]:#x}', 'mssc': row['mssc']}
                     for index, row in enumerate(rows)],
        'digest': {'stored': stored.hex(), 'computed': digest(image).hex(),
                   'matches': stored == digest(image)},
    }
    print(json.dumps(info, indent=2))


def cmd_slots(args):
    rows = []
    for name, slot in SLOTS.items():
        rows.append({'name': name, 'va': hex(slot['va']), 'offset': hex(slot['offset']),
                     'size': slot['size'], 'executable': name not in DATA_SLOTS,
                     'note': slot['note']})
    print(json.dumps(rows, indent=2))


def cmd_patch(args):
    image = read_image(args.image)
    offset = int(args.offset, 0)
    patch = bytes.fromhex(args.hex.replace(' ', ''))
    if offset < 0 or offset + len(patch) > len(image):
        raise SystemExit('补丁超出镜像')
    out = bytearray(image)
    out[offset:offset + len(patch)] = patch
    result = with_digest(out)
    Path(args.output).write_bytes(result)
    print(json.dumps({'output': args.output, 'offset': hex(offset),
                      'sha256': hashlib.sha256(result).hexdigest()}, indent=2))


def cmd_inject(args):
    image = read_image(args.image)
    slot = SLOTS[args.slot]
    if args.slot in DATA_SLOTS:
        raise SystemExit(f'{args.slot} 只可放数据，不能执行代码')
    code = Path(args.code).read_bytes()
    if not code or len(code) > slot['size']:
        raise SystemExit(f'代码长度必须在 1..{slot["size"]} 字节')
    if image[HOOK_OFFSET:HOOK_OFFSET + 2] != HOOK_PREFIX:
        raise SystemExit('hook 位置的原始指令与预期不符')
    hook_va = int(args.hook_va, 0) if args.hook_va else HOOK_VA
    jal = assemble_jal(slot['va'], hook_va)
    out = bytearray(image)
    out[HOOK_OFFSET:HOOK_OFFSET + 4] = jal
    out[slot['offset']:slot['offset'] + len(code)] = code
    result = with_digest(out)
    Path(args.output).write_bytes(result)
    print(json.dumps({'output': args.output, 'slot': args.slot,
                      'slot_va': hex(slot['va']), 'hook_va': hex(hook_va),
                      'jal': jal.hex(' '), 'code_bytes': len(code),
                      'sha256': hashlib.sha256(result).hexdigest(),
                      'digest': digest(result).hex()}, indent=2))


def embed_image(path, image, template):
    source = render_array(image, template)
    source = re.sub(r'(u32 array_length_8852b_u2_nicce = )\d+(;)',
                    lambda m: m[1] + str(len(image)) + m[2], source)
    header = path.with_suffix('.h')
    original_header = header.read_text()
    declaration = re.sub(r'(extern u8 array_8852b_u2_nicce\[)\d+(\];)',
                         lambda m: m[1] + str(len(image)) + m[2], original_header)
    if source != template:
        path.write_text(source)
    if declaration != original_header:
        header.write_text(declaration)


def cmd_embed(args):
    image = read_image(args.image)
    path = Path(args.array)
    embed_image(path, image, path.read_text())
    print(json.dumps({'array': str(path), 'header': str(path.with_suffix('.h')),
                      'image_bytes': len(image)}, indent=2))


def cmd_extend(args):
    import struct
    data = Path(args.image).read_bytes()
    code_len, data_len = layout(data)
    extra = int(args.bytes, 0)
    new_code_len = code_len + extra
    header = data[:0xA0]
    code = data[0xA0:0xA0 + code_len]
    rest = data[0xA0 + code_len:]
    out = bytearray(header + code + bytes(extra) + rest)
    dw1 = struct.unpack_from('<I', out, 0x24)[0]
    struct.pack_into('<I', out, 0x24, (dw1 & 0xFF000000) | new_code_len)
    result = with_digest(out)
    Path(args.output).write_bytes(result)
    if args.array_output:
        template = Path(args.template).read_text() if args.template else None
        Path(args.array_output).write_text(render_array(result, template))
    print(json.dumps({'output': args.output, 'code_len_before': hex(code_len),
                      'code_len_after': hex(new_code_len),
                      'size_before': len(data), 'size_after': len(result),
                      'sha256': hashlib.sha256(result).hexdigest(),
                      'digest': digest(result).hex()}, indent=2))


def cmd_add_section(args):
    """在安全段之前插入一个新的代码段，用于把代码放到 IRAM 之外。"""
    import struct
    image = read_image(args.image)
    fields = header_fields(image)
    rows = sections(image)
    offsets = section_offsets(image)
    names = ['code', 'data', 'security']
    if len(rows) != 3 or [row['type'] for row in rows] != [2, 1, 9]:
        raise SystemExit('只支持 code/data/security 三段基线布局')
    payload = Path(args.payload).read_bytes() if args.payload else bytes(
        [int(args.fill, 0)]) * int(args.bytes, 0)
    if not payload:
        raise SystemExit('新段不能为空')
    if int(args.address, 0) % 4:
        raise SystemExit('新段地址必须 4 字节对齐')
    body = image[fields['hdr_len']:]
    parts = [body[offset:offset + row['length']] for offset, row in zip(
        [o - fields['hdr_len'] for o in offsets], rows)]
    sig = body[offsets[-1] + rows[-1]['length'] - fields['hdr_len']:]
    for name, row in zip(names, rows):
        print(f'{name:8s} va={row["va"]:#010x} len={row["length"]:#x}', file=sys.stderr)
    hdr = bytearray(args.new_hdr_len)
    hdr[:0x40] = image[:0x40]
    struct.pack_into('<IIII', hdr, 0x40, int(args.address, 0),
                     (int(args.flags, 0) << 24) | len(payload), 0, 0)
    hdr[0x50:0x60] = image[0x40:0x50]
    hdr[0x60:args.new_hdr_len] = image[0x50:fields['hdr_len']]
    dw3 = struct.unpack_from('<I', hdr, 0x0C)[0]
    struct.pack_into('<I', hdr, 0x0C, (dw3 & 0xFFFF) | (args.new_hdr_len << 16))
    dw6 = struct.unpack_from('<I', hdr, 0x18)[0]
    struct.pack_into('<I', hdr, 0x18, (dw6 & ~0xFF00) | (4 << 8))
    packed = bytes(hdr) + parts[0] + parts[1] + payload + parts[2] + sig
    result = with_digest(packed)
    Path(args.output).write_bytes(result)
    if args.array_output:
        template = Path(args.template).read_text() if args.template else None
        Path(args.array_output).write_text(render_array(result, template))
    print(json.dumps({'output': args.output, 'size': len(result),
                      'sha256': hashlib.sha256(result).hexdigest(),
                      'section_num': 4, 'hdr_len': args.new_hdr_len,
                      'new_section': {'address': args.address, 'length': len(payload),
                                      'flags': args.flags},
                      'digest': digest(result).hex()}, indent=2))


def cmd_extend_data(args):
    """扩大数据段并追加代码，用于把可执行内容放进 RAM 数据区。"""
    import struct
    image = read_image(args.image)
    rows = sections(image)
    offsets = section_offsets(image)
    fields = header_fields(image)
    if len(rows) != 3 or [row['type'] for row in rows] != [2, 1, 9]:
        raise SystemExit('需要 code/data/security 三段基线布局')
    payload = Path(args.payload).read_bytes() if args.payload else bytes(
        [int(args.fill, 0)]) * int(args.bytes, 0)
    if not payload:
        raise SystemExit('payload 为空')
    lengths = [row['length'] for row in rows]
    parts = [image[offset:offset + length] for offset, length in zip(offsets, lengths)]
    tail = image[offsets[-1] + lengths[-1]:]
    out = bytearray(image[:fields['hdr_len']] + parts[0] + parts[1] + payload
                    + parts[2] + tail)
    struct.pack_into('<I', out, HEADER_BASE + SECTION_LEN,
                     (rows[1]['flags'] << 24) | (lengths[1] + len(payload)))
    entry = rows[1]['va'] + lengths[1]
    if args.hook_va:
        if image[HOOK_OFFSET:HOOK_OFFSET + 2] != HOOK_PREFIX:
            raise SystemExit('hook 位置原始指令与预期不符')
        out[HOOK_OFFSET:HOOK_OFFSET + 4] = assemble_jal(entry, int(args.hook_va, 0))
    result = with_digest(out)
    Path(args.output).write_bytes(result)
    if args.array_output:
        template = Path(args.template).read_text() if args.template else None
        Path(args.array_output).write_text(render_array(result, template))
    print(json.dumps({'output': args.output, 'size': len(result),
                      'sha256': hashlib.sha256(result).hexdigest(),
                      'data_before': hex(lengths[1]),
                      'data_after': hex(lengths[1] + len(payload)),
                      'entry': hex(entry), 'payload_bytes': len(payload),
                      'hooked': bool(args.hook_va),
                      'digest': digest(result).hex()}, indent=2))

def cmd_verify(args):
    import struct
    image = read_image(args.image)
    offset, embedded = extract_embedded(args.module)
    _, length = extract_symbol(args.module, 'array_length_8852b_u2_nicce')
    if len(length) != 4:
        raise SystemExit('U2 下载长度符号必须为4字节')
    download_length = struct.unpack('<I', length)[0]
    equal = embedded == image
    length_equal = download_length == len(image)
    print(json.dumps({'module': args.module, 'image': args.image,
                      'embedded_offset': hex(offset), 'embedded_bytes': len(embedded),
                      'expected_bytes': len(image), 'equal': equal,
                      'download_length': download_length,
                      'length_equal': length_equal}, indent=2))
    return 0 if equal and length_equal else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)

    p = sub.add_parser('info', help='解析镜像头、段与摘要')
    p.add_argument('image')
    p.set_defaults(func=cmd_info)

    p = sub.add_parser('slots', help='列出可复用代码槽')
    p.set_defaults(func=cmd_slots)

    p = sub.add_parser('patch', help='按偏移改字节并重算摘要')
    p.add_argument('image')
    p.add_argument('--offset', required=True)
    p.add_argument('--hex', required=True)
    p.add_argument('-o', '--output', required=True)
    p.set_defaults(func=cmd_patch)

    p = sub.add_parser('inject', help='把自定义代码写入槽位并劫持入口')
    p.add_argument('image')
    p.add_argument('--slot', required=True, choices=sorted(SLOTS))
    p.add_argument('--code', required=True)
    p.add_argument('--hook-va', help='入口地址，默认调度日志立即数')
    p.add_argument('-o', '--output', required=True)
    p.set_defaults(func=cmd_inject)

    p = sub.add_parser('embed', help='同步嵌入镜像、数组声明与下载长度')
    p.add_argument('image')
    p.add_argument('--array', required=True, help='生成树中的 hal8852b_fw.c')
    p.set_defaults(func=cmd_embed)

    p = sub.add_parser('extend', help='扩大代码段并填充，探测可用 RAM')
    p.add_argument('image')
    p.add_argument('--bytes', required=True, help='新增字节数，如 0x10000')
    p.add_argument('--array-output', help='同时输出新的数组源码')
    p.add_argument('--template', help='基线 hal8852b_fw.c，用于复用文件头')
    p.add_argument('-o', '--output', required=True)
    p.set_defaults(func=cmd_extend)

    p = sub.add_parser('add-section', help='在安全段前插入新代码段')
    p.add_argument('image')
    p.add_argument('--address', required=True, help='新段加载地址，如 0xb8e16000')
    p.add_argument('--bytes', default='0', help='填充长度')
    p.add_argument('--payload', help='新段内容文件；省略则按 --fill 填充')
    p.add_argument('--fill', default='0x00', help='填充字节')
    p.add_argument('--flags', default='0x42', help='段标志高字节，代码段为 0x42')
    p.add_argument('--new-hdr-len', type=lambda v: int(v, 0), default=0xB0)
    p.add_argument('--array-output', help='同时输出新的数组源码')
    p.add_argument('--template', help='基线 hal8852b_fw.c，用于复用文件头')
    p.add_argument('-o', '--output', required=True)
    p.set_defaults(func=cmd_add_section)

    p = sub.add_parser('extend-data', help='扩大数据段并追加代码')
    p.add_argument('image')
    p.add_argument('--bytes', default='0', help='填充长度')
    p.add_argument('--payload', help='追加内容文件')
    p.add_argument('--fill', default='0x00')
    p.add_argument('--hook-va', help='在该地址写入 jal 跳转到新内容')
    p.add_argument('--array-output')
    p.add_argument('--template')
    p.add_argument('-o', '--output', required=True)
    p.set_defaults(func=cmd_extend_data)

    p = sub.add_parser('verify', help='比对模块内嵌镜像')
    p.add_argument('module')
    p.add_argument('image')
    p.set_defaults(func=cmd_verify)

    args = parser.parse_args()
    return args.func(args) or 0


if __name__ == '__main__':
    sys.exit(main())

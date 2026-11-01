#!/usr/bin/env python3
"""Attach the native BIGTK owner to the U2 beacon/FAW candidate."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
from u2_compile_c import compile_c
from u2fw import layout, with_digest, security_offset

BASE = 0xb8970000
TARGET = 0xb89bbda0
STATE = 0xb8e11ce8


def build(source, out):
    image = bytearray(source.read_bytes())
    if len(image) != 329400:
        raise ValueError('expected 4 KiB FAW refresh candidate')
    entry = 0xb89addb4 - BASE + 0xa0
    if image[entry:entry+8] != bytes.fromhex('fd63056204d04467'):
        raise ValueError('NAN dispatcher prefix mismatch')
    out.mkdir(parents=True, exist_ok=False)
    patches = Path(__file__).resolve().parents[1] / 'patches'

    def run(argv):
        p = subprocess.run(argv, text=True, capture_output=True, timeout=5)
        with (out/'commands.log').open('a') as f:
            f.write(repr(argv)+'\n'+p.stdout+p.stderr)
        p.check_returncode()
        return p.stdout

    objects = []
    for name in ('u2_bip', 'u2_bigtk', 'u2_bigtk_native'):
        obj = compile_c(patches/(name+'.c'), out/(name+'.o'))
        objects.append(str(obj))
    run(['mipsel-linux-gnu-as', '-EL', '-mips32r2', '-mabi=32', '-o',
         str(out/'entry.o'), str(patches/'u2_bigtk.S')])
    objects.append(str(out/'entry.o'))
    (out/'bigtk.ld').write_text(f'''
u2_bigtk_state = {STATE:#x};
SECTIONS {{
 .text {TARGET:#x} : SUBALIGN(4) {{ *(.text .text.*) }}
 .rodata {STATE+0x310:#x} : SUBALIGN(4) {{ *(.rodata .rodata.*) }}
 /DISCARD/ : {{ *(.pdr) *(.comment) *(.reginfo) *(.MIPS.abiflags) }}
}}
''')
    run(['mipsel-linux-gnu-ld', '-T', str(out/'bigtk.ld'), '-o', str(out/'bigtk.elf')] + objects)
    run(['mipsel-linux-gnu-objcopy', '-O', 'binary', '-j', '.text',
         str(out/'bigtk.elf'), str(out/'bigtk.bin')])
    run(['mipsel-linux-gnu-objcopy', '-O', 'binary', '-j', '.rodata',
         str(out/'bigtk.elf'), str(out/'rodata.bin')])
    symbols = {s.split()[2]: int(s.split()[0], 16)
               for s in run(['mipsel-linux-gnu-nm', str(out/'bigtk.elf')]).splitlines()
               if len(s.split()) == 3}
    (out/'bigtk.asm').write_text(run(['mipsel-linux-gnu-objdump', '-d', str(out/'bigtk.elf')]))
    code = (out/'bigtk.bin').read_bytes()
    offset = TARGET - BASE + 0xa0
    old_length, _ = layout(image)
    needed = offset + len(code) - (0xa0 + old_length)
    extra = max(0, (needed + 4095) // 4096 * 4096)
    if extra > 0x3000:
        raise ValueError('BIGTK exceeds the 16 KiB code extension limit')
    image[0xa0+old_length:0xa0+old_length] = bytes(extra)
    word = struct.unpack_from('<I', image, 0x24)[0]
    struct.pack_into('<I', image, 0x24, (word & 0xff000000) | (old_length + extra))
    if any(image[offset:offset+len(code)]):
        raise ValueError('BIGTK code region occupied')
    image[offset:offset+len(code)] = code
    data = security_offset(image) + 0x3c0
    if any(image[data:data+1088]):
        raise ValueError('BIGTK data region occupied')
    rodata = (out/'rodata.bin').read_bytes()
    # Reserve the last word at 0xb8e12124 for diagnostic gate sequencing.
    if 0x310+len(rodata) > 1084:
        raise ValueError('BIGTK constants exceed data region')
    image[data+0x310:data+0x310+len(rodata)] = rodata

    def branch(target):
        path = out/'branch.S'
        path.write_text('.set noreorder\n.set mips16\n.text\njal '+hex(target & ~1)+'\nnop\n')
        run(['mipsel-linux-gnu-as', '-EL', '-mips32r2', '-mabi=32', '-o', str(out/'branch.o'), str(path)])
        run(['mipsel-linux-gnu-objcopy', '-O', 'binary', '-j', '.text', str(out/'branch.o'), str(out/'branch.bin')])
        return (out/'branch.bin').read_bytes()[:4]

    image[entry:entry+8] = bytes.fromhex('7f67') + branch(symbols['big_dispatch_entry']) + bytes.fromhex('0065')
    arm = 0xb89a7dca - BASE + 0xa0
    if image[arm:arm+6] != bytes.fromhex('056d40eb006c'):
        raise ValueError('NAN event timer call mismatch')
    image[arm:arm+6] = branch(symbols['u2_nan_schedule']) + bytes.fromhex('0065')
    begin = 0xb89ac530 - BASE + 0xa0
    if image[begin:begin+2] != bytes.fromhex('7f67'):
        raise ValueError('expected guarded FAW begin entry')
    image[begin:begin+8] = bytes.fromhex('7f67') + branch(symbols['big_faw_begin_entry']) + bytes.fromhex('0065')
    # Preserve the deadline registers and bind the native timer to its MACID.
    timer = 0xb89ac67c - BASE + 0xa0
    if image[timer:timer+6] != bytes.fromhex('04d240eb006c'):
        raise ValueError('FAW end timer call mismatch')
    image[timer:timer+6] = branch(symbols['u2_nan_faw_schedule_end']) + bytes.fromhex('0065')
    for start, end, old, new in (
            (0xb89aadae, 0xb89aadb2, 0xb89a2c44, 'u2_nan_iqc_channel'),
            (0xb89ae418, 0xb89ae41c, 0xb89a2c44, 'u2_nan_iqc_channel'),
            (0xb89ae5e8, 0xb89ae5ec, 0xb89a2c44, 'u2_nan_iqc_channel'),
            (0xb89ae89c, 0xb89ae8a0, 0xb89a2c44, 'u2_nan_iqc_channel'),
            (0xb89aea40, 0xb89aea44, 0xb89a2c44, 'u2_nan_iqc_channel'),
            (0xb89ac158, 0xb89ac15c, 0xb89a2c44, 'u2_nan_discovery_channel'),
            (0xb89ac22c, 0xb89ac230, 0xb89a2c44, 'u2_nan_discovery_channel'),
            (0xb89ac2aa, 0xb89ac2ae, 0xb89a2c44, 'u2_nan_discovery_channel'),
            (0xb89ac5fa, 0xb89ac5fe, 0xb89a2c44, 'u2_nan_faw_channel'),
            (0xb89ac62c, 0xb89ac630, 0xb8979c0c, 'u2_nan_faw_gate'),
            (0xb89a690c, 0xb89a6910, 0xb8979c0c, 'u2_nan_faw_gate'),
            (0xb89bb8a0, 0xb89bb8e0, 0xb89bb91c, 'u2_bigtk_prepare'),
            (0xb89bb9b8, 0xb89bb9fc, 0xb89bb9fc, 'u2_bigtk_gate'),
            (0xb89bba14, 0xb89bba90, 0xb89bb8f4, 'big_trace_done')):
        a, b = start-BASE+0xa0, end-BASE+0xa0
        at = image.find(branch(old), a, b)
        if at < 0 or image.find(branch(old), at+4, b) >= 0:
            raise ValueError('trace call mismatch: '+new)
        image[at:at+4] = branch(symbols[new])
    (out/'candidate.bin').write_bytes(with_digest(image))
    result = dict(code_bytes=len(code), constant_bytes=len(rodata), total_extension=4096+extra,
                  state=hex(STATE), symbols={k: hex(v) for k,v in symbols.items()})
    (out/'build.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k != 'symbols'}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    build(args.source, args.output)

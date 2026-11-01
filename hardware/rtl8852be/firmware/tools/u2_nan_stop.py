#!/usr/bin/env python3
"""Build the U2 NAN port startup and single-management-MACID STOP patch."""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess

from u2fw import IMAGE_SIZE, with_digest

BASE = 0xb8970000
CODE_LENGTH = 0x4b4a0
EXTENSION = BASE + CODE_LENGTH
EXTRA = 0x1000
CALLBACKS = (
    ('dw_release', 0xb89a6930, 'fc63076206d0f7f0016a'),
    ('dw_end', 0xb89ac2fc, 'f663136212d111d0'),
    ('faw_begin', 0xb89ac530, 'f8630f620ed10dd0'),
    ('faw_end', 0xb89a6840, 'f7f0016c80348034'),
    ('dw_initial', 0xb89aac80, 'f463176216d115d0'),
    ('warmup', 0xb89aa880, 'f663136212d111d0'),
)


def build(source, out):
    image = source.read_bytes()
    if len(image) != IMAGE_SIZE:
        raise ValueError('expected the original U2 section geometry')
    out.mkdir(parents=True, exist_ok=False)
    prefixes = []
    for name, address, raw in CALLBACKS:
        prefix = bytes.fromhex(raw)
        offset = address - BASE + 0xa0
        if image[offset:offset + len(prefix)] != prefix:
            raise ValueError(f'native prefix mismatch: {name}')
        target = address + len(prefix) | 1
        prefixes.append(f'''.balign 4
.globl {name}_body
.type {name}_body,@function
.ent {name}_body
{name}_body:
.insn
.byte {','.join(hex(b) for b in prefix)}
li $a3, {target >> 16}
sll $a3, 8
sll $a3, 8
addiu $a3, {target & 65535 if target & 65535 < 32768 else (target & 65535) - 65536}
jr $a3
nop
.end {name}_body
''')
    # Account for the signed immediate used by addiu.
    for i, (_, address, raw) in enumerate(CALLBACKS):
        target = address + len(bytes.fromhex(raw)) | 1
        prefixes[i] = prefixes[i].replace(f'li $a3, {target >> 16}',
                                          f'li $a3, {(target + 0x8000) >> 16}')
    (out / 'native_prefixes.S').write_text('\n'.join(prefixes))
    assembly = Path(__file__).resolve().parents[1] / 'patches/u2_nan_stop.S'
    commands = []

    def run(args):
        commands.append(args)
        p = subprocess.run(args, capture_output=True, text=True, timeout=5)
        with (out / "commands.log").open("a") as log:
            log.write(repr(args) + "\n" + p.stdout + p.stderr)
        if p.returncode:
            raise RuntimeError("\n".join(p.stderr.splitlines()[-6:]))
        return p.stdout

    tools = {name: shutil.which('mipsel-linux-gnu-' + name)
             for name in ('as', 'ld', 'objcopy', 'objdump', 'nm')}
    if not all(tools.values()):
        raise ValueError('MIPS GNU binutils are required')
    run([tools['as'], '-EL', '-mips32r2', '-mabi=32', '-I', str(out),
         '-o', str(out / 'patch.o'), str(assembly)])
    (out / 'patch.ld').write_text(
        f'SECTIONS {{ . = {EXTENSION:#x}; .text : {{ *(.text) }} }}\n')
    run([tools['ld'], '-T', str(out / 'patch.ld'), '-o', str(out / 'patch.elf'),
         str(out / 'patch.o')])
    run([tools['objcopy'], '-O', 'binary', '-j', '.text',
         str(out / 'patch.elf'), str(out / 'patch.bin')])
    (out / 'patch.asm').write_text(run([tools['objdump'], '-d', str(out / 'patch.elf')]))
    symbols = {row.split()[2]: int(row.split()[0], 16) for row in
               run([tools['nm'], '-n', str(out / 'patch.elf')]).splitlines()
               if len(row.split()) == 3}
    code = (out / 'patch.bin').read_bytes()
    if len(code) > EXTRA:
        raise ValueError('patch exceeds the tested 4 KiB extension')
    split = 0xa0 + CODE_LENGTH
    result = bytearray(image[:split] + code + bytes(EXTRA - len(code)) + image[split:])
    section = struct.unpack_from('<I', result, 0x24)[0]
    struct.pack_into('<I', result, 0x24, (section & 0xff000000) | (CODE_LENGTH + EXTRA))
    hooks = [(name, address, len(bytes.fromhex(raw))) for name, address, raw in CALLBACKS]
    hooks.append(('stop_dispatch', 0xb89adddc, 4))
    if image[0xb89adddc-BASE+0xa0:0xb89adde0-BASE+0xa0] != bytes.fromhex('d118b4b1'):
        raise ValueError('native func2 dispatch call mismatch')
    reset = 0xb89a6b7e - BASE + 0xa0
    if image[reset:reset+8] != bytes.fromhex('206d4ded40e99067'):
        raise ValueError('native port TSF reset mismatch')
    hooks.append(('port_tsf_reset', 0xb89a6b7e, 8))
    for name, address, length in hooks:
        entry = out / (name + '-entry')
        text = '.set noreorder\n.set mips16\n.text\n'
        if name not in ('stop_dispatch', 'port_tsf_reset'):
            text += 'move $v1, $ra\n'
        text += f'jal {symbols[name] & ~1:#x}\nnop\n'
        entry.with_suffix('.S').write_text(text)
        entry.with_suffix('.ld').write_text(
            f'SECTIONS {{ .text {address:#x} : SUBALIGN(2) {{ *(.text) }} }}\n')
        run([tools['as'], '-EL', '-mips32r2', '-mabi=32', '-o',
             str(entry.with_suffix('.o')), str(entry.with_suffix('.S'))])
        run([tools['ld'], '-T', str(entry.with_suffix('.ld')), '-o',
             str(entry.with_suffix('.elf')), str(entry.with_suffix('.o'))])
        run([tools['objcopy'], '-O', 'binary', '-j', '.text',
             str(entry.with_suffix('.elf')), str(entry.with_suffix('.bin'))])
        hook = entry.with_suffix('.bin').read_bytes()[:4 if name == 'stop_dispatch' else 8]
        hook += bytes.fromhex('0065') * ((length - len(hook)) // 2)
        result[address-BASE+0xa0:address-BASE+0xa0+length] = hook
    (out / 'candidate.bin').write_bytes(with_digest(result))
    (out / 'build.json').write_text(json.dumps(dict(code_bytes=len(code),
        extension_bytes=EXTRA, symbols=symbols, hardware_validated=False), indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path, help='new output directory')
    args = parser.parse_args()
    build(args.source, args.output)

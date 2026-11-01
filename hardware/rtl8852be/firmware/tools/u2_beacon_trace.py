#!/usr/bin/env python3
"""Trace native beacon preparation, template updates, gates and TX results."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

from u2fw import with_digest

BASE = 0xb8970000
ENTRY = 0xb89a4818
TARGET = 0xb89bb8a0


def build(source, out, *, diagnostic=True):
    image = bytearray(source.read_bytes())
    start = TARGET - BASE + 0xa0
    hook = ENTRY - BASE + 0xa0
    if len(image) != 329400 or image[hook:hook + 8] != bytes.fromhex('f1631d621cd11bd0'):
        raise ValueError('expected U2 beacon entry with 4 KiB code extension')
    if image[0x38458] != 0:
        raise ValueError('expected the 512 TU NAN DW phase patch')
    if any(image[start:start + 0xc00]):
        raise ValueError('diagnostic extension region is occupied')
    out.mkdir(parents=True, exist_ok=False)
    tools = {name: shutil.which('mipsel-linux-gnu-' + name)
             for name in ('as', 'ld', 'objcopy', 'objdump', 'nm')}
    if not all(tools.values()):
        raise ValueError('MIPS GNU binutils are required')

    def run(args):
        p = subprocess.run(args, capture_output=True, text=True, timeout=5)
        with (out / 'commands.log').open('a') as log:
            log.write(repr(args) + '\n' + p.stdout + p.stderr)
        p.check_returncode()
        return p.stdout

    def assemble(name, source_path, address):
        stem = out / name
        stem.with_suffix('.ld').write_text(
            f'SECTIONS {{ .text {address:#x} : SUBALIGN(2) {{ *(.text) }} }}\n')
        run([tools['as'], '-EL', '-mips32r2', '-mabi=32', '--defsym',
             'WA_BEACON_TRACE='+str(int(diagnostic)), '-o', str(stem.with_suffix('.o')), str(source_path)])
        run([tools['ld'], '-T', str(stem.with_suffix('.ld')), '-o', str(stem.with_suffix('.elf')), str(stem.with_suffix('.o'))])
        run([tools['objcopy'], '-O', 'binary', '-j', '.text', str(stem.with_suffix('.elf')), str(stem.with_suffix('.bin'))])
        stem.with_suffix('.asm').write_text(run([tools['objdump'], '-d', str(stem.with_suffix('.elf'))]))
        return stem.with_suffix('.bin').read_bytes()

    code = assemble('trace', Path(__file__).resolve().parents[1] / 'patches/u2_beacon_trace.S', TARGET)
    if len(code) > 0xc00:
        raise ValueError('diagnostic code exceeds extension')
    (out / 'entry.S').write_text(
        f'.set noreorder\n.set mips16\n.text\nmove $v1, $ra\njal {TARGET:#x}\nnop\n')
    veneer = assemble('entry', out / 'entry.S', ENTRY)
    image[hook:hook + 8] = veneer[:8]
    symbols = {line.split()[-1]: int(line.split()[0], 16) for line in run([tools['nm'], str(out/'trace.elf')]).splitlines() if len(line.split()) == 3}
    for name, address, expected in (
        ('field', 0xb89a41d8, 'fd63056204d0f7f0016a'),
        ('gate', 0xb89a6c80, 'fc63076206d105d0'),
        ('tx', 0xb8992e32, '6da08ca0aea0')):
        at = address - BASE + 0xa0
        original = bytes.fromhex(expected)
        if image[at:at+len(original)] != original:
            raise ValueError('hook mismatch ' + name)
        target = symbols[name + '_trace'] & ~1
        prefix = 'move $v1, $ra\n' if name != 'tx' else ''
        (out/(name+'.S')).write_text('.set noreorder\n.set mips16\n.text\n' + prefix + f'jal {target:#x}\nnop\n')
        branch = assemble(name, out/(name+'.S'), address)
        used = 6 if name == 'tx' else 8
        image[at:at+len(original)] = branch[:used] + bytes.fromhex('0065') * ((len(original)-used)//2)

    image[start:start + len(code)] = code
    (out / 'candidate.bin').write_bytes(with_digest(image))
    (out / 'build.json').write_text(json.dumps(dict(code_bytes=len(code), entry=hex(ENTRY),
        target=hex(TARGET), diagnostic=diagnostic, marker_gate=248, marker_field=250, marker_tx=251, marker_enter=252, marker_return=253), indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path, help='new output directory')
    args = parser.parse_args()
    build(args.source, args.output)

#!/usr/bin/env python3
"""Refresh missed native FAW candidates in a U2 4 KiB code extension."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

from u2fw import with_digest

BASE = 0xb8970000
ENTRY = 0xb89ab278
TARGET = 0xb89bbca0
PREFIX = bytes.fromhex('f763116210d10fd0')


def build(source, out):
    image = bytearray(source.read_bytes())
    hook = ENTRY - BASE + 0xa0
    start = TARGET - BASE + 0xa0
    if len(image) != 329400 or image[hook:hook + 8] != PREFIX:
        raise ValueError('expected U2 native selector with 4 KiB code extension')
    if any(image[start:start + 0x800]):
        raise ValueError('FAW extension region is occupied')
    out.mkdir(parents=True, exist_ok=False)
    tools = {name: shutil.which('mipsel-linux-gnu-' + name)
             for name in ('as', 'ld', 'objcopy', 'objdump')}
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
        run([tools['as'], '-EL', '-mips32r2', '-mabi=32', '-o',
             str(stem.with_suffix('.o')), str(source_path)])
        run([tools['ld'], '-T', str(stem.with_suffix('.ld')), '-o',
             str(stem.with_suffix('.elf')), str(stem.with_suffix('.o'))])
        run([tools['objcopy'], '-O', 'binary', '-j', '.text',
             str(stem.with_suffix('.elf')), str(stem.with_suffix('.bin'))])
        stem.with_suffix('.asm').write_text(run(
            [tools['objdump'], '-d', str(stem.with_suffix('.elf'))]))
        return stem.with_suffix('.bin').read_bytes()

    code = assemble('refresh', Path(__file__).resolve().parents[1] /
                    'patches/u2_faw_refresh.S', TARGET)
    if len(code) > 0x800:
        raise ValueError('FAW code exceeds extension')
    (out / 'entry.S').write_text(
        f'.set noreorder\n.set mips16\n.text\nmove $v1, $ra\njal {TARGET:#x}\nnop\n')
    entry = assemble('entry', out / 'entry.S', ENTRY)
    image[hook:hook + 8] = entry[:8]
    image[start:start + len(code)] = code
    (out / 'candidate.bin').write_bytes(with_digest(image))
    (out / 'build.json').write_text(json.dumps(dict(code_bytes=len(code),
        entry=hex(ENTRY), target=hex(TARGET), marker_refresh=247), indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path, help='new output directory')
    args = parser.parse_args()
    build(args.source, args.output)

#!/usr/bin/env python3
"""Add native FAW calculator input capture to the BIGTK U2 candidate."""
import argparse
import json
from pathlib import Path
import subprocess
from u2fw import with_digest


def build(source, output):
    data = bytearray(source.read_bytes())
    entry, target = 0xb89ad4ec, 0xb89bcac0
    offset = lambda address: address - 0xb8970000 + 0xa0
    if data[offset(entry):offset(entry)+8] != bytes.fromhex('f463176216d115d0'):
        raise ValueError('native FAW calculator prefix mismatch')
    output.mkdir(parents=True, exist_ok=False)
    def assemble(name, path, address):
        stem = output/name
        stem.with_suffix('.ld').write_text(
            f'SECTIONS {{ .text {address:#x} : SUBALIGN(2) {{ *(.text) }} }}\n')
        commands = [
            ['mipsel-linux-gnu-as', '-EL', '-mips32r2', '-mabi=32', '-o', str(stem.with_suffix('.o')), str(path)],
            ['mipsel-linux-gnu-ld', '-T', str(stem.with_suffix('.ld')), '-o', str(stem.with_suffix('.elf')), str(stem.with_suffix('.o'))],
            ['mipsel-linux-gnu-objcopy', '-O', 'binary', '-j', '.text', str(stem.with_suffix('.elf')), str(stem.with_suffix('.bin'))]]
        for command in commands:
            result = subprocess.run(command, capture_output=True, text=True, timeout=5)
            with (output/'commands.log').open('a') as f:
                f.write(repr(command)+'\n'+result.stdout+result.stderr)
            result.check_returncode()
        return stem.with_suffix('.bin').read_bytes()
    code = assemble('trace', Path(__file__).resolve().parents[1]/'patches/u2_faw_clock_trace.S', target)
    if len(code)>128 or any(data[offset(target):offset(target)+len(code)]):
        raise ValueError('trace code slot occupied')
    stub = output/'entry.S'
    stub.write_text(f'.set noreorder\n.set mips16\n.text\nmove $v1,$ra\njal {target:#x}\nnop\n')
    branch = assemble('entry', stub, entry)
    if branch[:2] != bytes.fromhex('7f67') or branch[6:8] != bytes.fromhex('0065'):
        raise ValueError('unaligned MIPS16 entry')
    data[offset(entry):offset(entry)+8] = branch[:8]
    data[offset(target):offset(target)+len(code)] = code
    (output/'candidate.bin').write_bytes(with_digest(data))
    (output/'build.json').write_text(json.dumps(dict(entry=hex(entry), target=hex(target), bytes=len(code), record='0xb8e12080', record_bytes=24),indent=2)+'\n')


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    parser.add_argument('output',type=Path)
    args=parser.parse_args()
    build(args.source,args.output)

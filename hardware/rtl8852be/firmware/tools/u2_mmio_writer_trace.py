#!/usr/bin/env python3
"""Build an isolated U2 candidate tracing writes to aggregate retry policy."""
import argparse
import json
import re
from pathlib import Path
import subprocess
from u2fw import with_digest
from u2_nan_gate_trace import build as gate_build


def build(source, output):
    output.mkdir(parents=True, exist_ok=False)
    gate_build(source, output / 'gate')
    image = bytearray((output / 'gate/candidate.bin').read_bytes())
    patches = Path(__file__).resolve().parents[1] / 'patches'
    for address, expected in ((0xb89993bc, 'fd63056204d0'), (0xb8999cec, 'fd630562')):
        start = address - 0xb8970000 + 0xa0
        if image[start:start + len(bytes.fromhex(expected))] != bytes.fromhex(expected):
            raise ValueError('native setter/dispatcher stack layout mismatch')

    def assemble(name, source_path, address):
        stem = output / name
        stem.with_suffix('.ld').write_text(
            f'SECTIONS {{ .text {address:#x} : SUBALIGN(2) {{ *(.text) }} }}\n')
        commands = [
            ['mipsel-linux-gnu-as', '-EL', '-mips32r2', '-mabi=32', '-o', str(stem.with_suffix('.o')), str(source_path)],
            ['mipsel-linux-gnu-ld', '-T', str(stem.with_suffix('.ld')), '-o', str(stem.with_suffix('.elf')), str(stem.with_suffix('.o'))],
            ['mipsel-linux-gnu-objcopy', '-O', 'binary', '-j', '.text', str(stem.with_suffix('.elf')), str(stem.with_suffix('.bin'))],
        ]
        for command in commands:
            subprocess.run(command, check=True, timeout=5)
        asm = subprocess.check_output(['mipsel-linux-gnu-objdump', '-d', str(stem.with_suffix('.elf'))], text=True, timeout=5)
        stem.with_suffix('.asm').write_text(asm)
        instructions = re.findall(r'^\s*[0-9a-f]+:\s+((?:[0-9a-f]{4}\s+)+)\s*(\w+)\b', asm, re.M)
        for previous, current in zip(instructions, instructions[1:]):
            if previous[1] in ('jal', 'jalr', 'jr') and len(current[0].split()) != 1:
                raise ValueError('MIPS16 delay slot must contain one 16-bit instruction')
        return stem.with_suffix('.bin').read_bytes()

    writer = assemble('writer', patches / 'u2_mmio_writer_trace.S', 0xb89bdb00)
    offset = lambda address: address - 0xb8970000 + 0xa0
    start = offset(0xb89bdb00)
    if len(writer) > 0x400 or any(image[start:start + len(writer)]):
        raise ValueError('writer trace slot occupied')
    image[start:start + len(writer)] = writer
    gate_source = (patches / 'u2_nan_gate_trace.S').read_text()
    anchor = ' sw $v0,64($sp)\n'
    if gate_source.count(anchor) != 1:
        raise ValueError('gate return-save anchor mismatch')
    gate_source = gate_source.replace(anchor, anchor + ' jal 0xb89bdb00\n nop\n')
    path = output / 'gate-install.S'
    path.write_text(gate_source)
    gate = assemble('gate-install', path, 0xb89bd900)
    if len(gate) > 0x100:
        raise ValueError('gate trace exceeds slot')
    start = offset(0xb89bd900)
    image[start:start + 0x100] = gate + bytes(0x100 - len(gate))
    (output / 'candidate.bin').write_bytes(with_digest(image))
    result = dict(writer_bytes=len(writer), gate_bytes=len(gate), register='0xc604',
                  markers={'241': 8, '242': 16, '244': 32}, parent_marker=243,
                  lifecycle='Installed after native gate call; candidate-only until module reload resets firmware RAM.')
    (output / 'build.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    print(json.dumps(build(args.source, args.output)))

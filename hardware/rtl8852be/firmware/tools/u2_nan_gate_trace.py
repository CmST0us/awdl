#!/usr/bin/env python3
"""Instrument native DW/FAW MACID writes in the complete U2 NAN image."""
import argparse
import json
import re
import struct
from pathlib import Path
import subprocess
from u2fw import with_digest, security_offset, layout


def build(source, output, target=0xb89be600, channel_target=0xb89be700, sequenced=False):
    image = bytearray(source.read_bytes())
    entry = 0xb8979c0c
    offset = lambda address: address - 0xb8970000 + 0xa0
    if len(image) not in (337592, 341688) or image[offset(entry):offset(entry)+8] != bytes.fromhex('f763116210d10fd0'):
        raise ValueError('expected complete U2 with native MACID gate entry')
    # Last word of the 1088-byte BIGTK data allocation is the trace counter.
    if sequenced and any(image[security_offset(image)+0x7fc:security_offset(image)+0x800]):
        raise ValueError('gate sequence RAM slot occupied')
    code_length, _ = layout(image)
    needed = max(target, channel_target) + 0x100 - 0xb8970000 - code_length
    extra = max(0, (needed + 4095) // 4096 * 4096)
    if extra:
        image[0xa0+code_length:0xa0+code_length] = bytes(extra)
        word = struct.unpack_from('<I', image, 0x24)[0]
        struct.pack_into('<I', image, 0x24, (word & 0xff000000) | (code_length+extra))
    output.mkdir(parents=True, exist_ok=False)

    def run(argv):
        result = subprocess.run(argv, capture_output=True, text=True, timeout=5)
        with (output/'commands.log').open('a') as log:
            log.write(repr(argv)+'\n'+result.stdout+result.stderr)
        result.check_returncode()
        return result.stdout

    def assemble(name, path, address):
        stem = output/name
        stem.with_suffix('.ld').write_text(
            f'SECTIONS {{ .text {address:#x} : SUBALIGN(2) {{ *(.text) }} }}\n')
        defines = ['--defsym', 'GATE_SEQUENCE=1'] if sequenced and name == 'trace' else []
        run(['mipsel-linux-gnu-as', '-EL', '-mips32r2', '-mabi=32', *defines, '-o',
             str(stem.with_suffix('.o')), str(path)])
        run(['mipsel-linux-gnu-ld', '-T', str(stem.with_suffix('.ld')), '-o',
             str(stem.with_suffix('.elf')), str(stem.with_suffix('.o'))])
        run(['mipsel-linux-gnu-objcopy', '-O', 'binary', '-j', '.text',
             str(stem.with_suffix('.elf')), str(stem.with_suffix('.bin'))])
        disassembly = run(['mipsel-linux-gnu-objdump', '-d', str(stem.with_suffix('.elf'))])
        stem.with_suffix('.asm').write_text(disassembly)
        instructions = re.findall(r'^\s*[0-9a-f]+:\s+((?:[0-9a-f]{4}\s+)+)\s*(\w+)\b', disassembly, re.M)
        for previous, current in zip(instructions, instructions[1:]):
            if previous[1] in ('jal', 'jalr', 'jr') and len(current[0].split()) != 1:
                raise ValueError('MIPS16 delay slot must contain one 16-bit instruction')
        return stem.with_suffix('.bin').read_bytes()

    code = assemble('trace', Path(__file__).resolve().parents[1]/'patches/u2_nan_gate_trace.S', target)
    if len(code) > 0x100 or any(image[offset(target):offset(target)+len(code)]):
        raise ValueError('MACID trace code slot occupied')
    stub = output/'entry.S'
    stub.write_text(f'.set noreorder\n.set mips16\n.text\nmove $v1,$ra\njal {target:#x}\nnop\n')
    branch = assemble('entry', stub, entry)
    image[offset(entry):offset(entry)+8] = branch[:8]
    image[offset(target):offset(target)+len(code)] = code
    channel_entry = 0xb89a2c44
    if image[offset(channel_entry):offset(channel_entry)+8] != bytes.fromhex('f1631d621cd11bd0'):
        raise ValueError('native channel entry mismatch')
    channel = assemble('channel', Path(__file__).resolve().parents[1]/'patches/u2_nan_channel_trace.S', channel_target)
    if len(channel)>0x100 or any(image[offset(channel_target):offset(channel_target)+len(channel)]):
        raise ValueError('channel trace code slot occupied')
    stub.write_text(f'.set noreorder\n.set mips16\n.text\nmove $v1,$ra\njal {channel_target:#x}\nnop\n')
    branch = assemble('channel-entry', stub, channel_entry)
    image[offset(channel_entry):offset(channel_entry)+8] = branch[:8]
    image[offset(channel_target):offset(channel_target)+len(channel)] = channel
    (output/'candidate.bin').write_bytes(with_digest(image))
    result = dict(entry=hex(entry), target=hex(target), bytes=len(code), marker=249 if sequenced else 246,
                  channel_entry=hex(channel_entry), channel_target=hex(channel_target), channel_bytes=len(channel), channel_marker=245)
    if sequenced:
        result['sequence_address'] = '0xb8e12124'
    (output/'build.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--sequenced', action='store_true', help='emit global gate sequence numbers (marker 249)')
    args = parser.parse_args()
    slots = dict(target=0xb89be600, channel_target=0xb89be700) if args.sequenced else {}
    print(json.dumps(build(args.source, args.output, sequenced=args.sequenced, **slots)))

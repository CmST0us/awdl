#!/usr/bin/env python3
"""Refresh native FAW clock anchors before recalculating a record."""
import argparse
import json
import struct
from pathlib import Path
import subprocess
from u2fw import with_digest, layout


def build(source, output, target=0xb89bd800):
    data=bytearray(source.read_bytes())
    entry=0xb89ad6c0
    offset=lambda address: address-0xb8970000+0xa0
    if len(data) not in (333496, 337592, 341688) or data[offset(entry):offset(entry)+8]!=bytes.fromhex('f2631b621ad119d0'):
        raise ValueError('expected U2 BIGTK native record calculator')
    code_length, _ = layout(data)
    extra = max(0, target + 128 - 0xb8970000 - code_length)
    extra = (extra + 4095) // 4096 * 4096
    data[0xa0+code_length:0xa0+code_length] = bytes(extra)
    word = struct.unpack_from('<I', data, 0x24)[0]
    struct.pack_into('<I', data, 0x24, (word & 0xff000000) | (code_length+extra))
    output.mkdir(parents=True,exist_ok=False)
    def assemble(name,path,address):
        stem=output/name
        stem.with_suffix('.ld').write_text(f'SECTIONS {{ .text {address:#x} : SUBALIGN(2) {{ *(.text) }} }}\n')
        commands=[
            ['mipsel-linux-gnu-as','-EL','-mips32r2','-mabi=32','-o',str(stem.with_suffix('.o')),str(path)],
            ['mipsel-linux-gnu-ld','-T',str(stem.with_suffix('.ld')),'-o',str(stem.with_suffix('.elf')),str(stem.with_suffix('.o'))],
            ['mipsel-linux-gnu-objcopy','-O','binary','-j','.text',str(stem.with_suffix('.elf')),str(stem.with_suffix('.bin'))]]
        for command in commands:
            result=subprocess.run(command,text=True,capture_output=True,timeout=5)
            with (output/'commands.log').open('a') as f:f.write(repr(command)+'\n'+result.stdout+result.stderr)
            result.check_returncode()
        return stem.with_suffix('.bin').read_bytes()
    code=assemble('anchor',Path(__file__).resolve().parents[1]/'patches/u2_faw_anchor.S',target)
    if len(code)>128 or any(data[offset(target):offset(target)+len(code)]):
        raise ValueError('FAW anchor code slot occupied')
    stub=output/'entry.S'
    stub.write_text(f'.set noreorder\n.set mips16\n.text\nmove $v1,$ra\njal {target:#x}\nnop\n')
    branch=assemble('entry',stub,entry)
    if branch[:2]!=bytes.fromhex('7f67') or branch[6:8]!=bytes.fromhex('0065'):
        raise ValueError('unaligned MIPS16 entry')
    data[offset(entry):offset(entry)+8]=branch[:8]
    data[offset(target):offset(target)+len(code)]=code
    (output/'candidate.bin').write_bytes(with_digest(data))
    (output/'build.json').write_text(json.dumps(dict(entry=hex(entry),target=hex(target),bytes=len(code),anchor='0xb8e16de4'),indent=2)+'\n')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--target', type=lambda value: int(value, 0), default=0xb89bd800)
    args=p.parse_args();build(args.source,args.output,args.target)

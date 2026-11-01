#!/usr/bin/env python3
"""Retain the established 528-byte MMIO probe with relocated NAN trace slots."""
import argparse
import json
from pathlib import Path
import subprocess
from u2_nan_gate_trace import build as gate_build
from u2fw import with_digest


def build(source, reference, output):
    reference = reference.read_bytes()
    offset = lambda address: address - 0xb8970000 + 0xa0
    if len(reference) != 337592:
        raise ValueError('expected complete observation reference')
    original_image = source.read_bytes()
    for address, size in ((0xb89bdb00, 528), (0xb89bde00, 256), (0xb89bdf00, 256)):
        if any(original_image[offset(address):offset(address)+size]):
            raise ValueError('observation slot overlaps candidate code')
    gate_build(source, output, target=0xb89bdf00, channel_target=0xb89bde00)
    image = bytearray((output/'candidate.bin').read_bytes())
    text = (Path(__file__).resolve().parents[1]/'patches/u2_nan_gate_trace.S').read_text()
    anchor = ' sw $v0,64($sp)\n'
    if text.count(anchor) != 1:
        raise ValueError('gate return-save anchor mismatch')
    path = output/'gate-install.S'
    path.write_text(text.replace(anchor, anchor+' jal 0xb89bdb00\n nop\n'))
    def assemble(address, name):
        stem=output/name
        stem.with_suffix('.ld').write_text(f'SECTIONS {{ .text {address:#x} : SUBALIGN(2) {{ *(.text) }} }}\n')
        for command in (
            ['mipsel-linux-gnu-as','-EL','-mips32r2','-mabi=32','-o',str(stem.with_suffix('.o')),str(path)],
            ['mipsel-linux-gnu-ld','-T',str(stem.with_suffix('.ld')),'-o',str(stem.with_suffix('.elf')),str(stem.with_suffix('.o'))],
            ['mipsel-linux-gnu-objcopy','-O','binary','-j','.text',str(stem.with_suffix('.elf')),str(stem.with_suffix('.bin'))]):
            subprocess.run(command,check=True,timeout=5,capture_output=True)
        return stem.with_suffix('.bin').read_bytes()
    original=assemble(0xb89bd900,'gate-reference')
    if len(original)!=240 or reference[offset(0xb89bd900):offset(0xb89bd900)+240]!=original:
        raise ValueError('reference gate differs from established MMIO installation path')
    channel=(output/'channel.bin').read_bytes()
    gate_path = path
    path = Path(__file__).resolve().parents[1]/'patches/u2_nan_channel_trace.S'
    original_channel = assemble(0xb89bda00, 'channel-reference')
    if reference[offset(0xb89bda00):offset(0xb89bda00)+len(original_channel)] != original_channel:
        raise ValueError('reference channel observer differs')
    path = gate_path
    # Relative branches and the internal JAL are relocated by the linker.
    relocated=assemble(0xb89bdf00,'gate-relocated')
    if len(relocated)!=240:
        raise ValueError('relocated gate size changed')
    writer=reference[offset(0xb89bdb00):offset(0xb89bdb00)+528]
    if not any(writer) or any(image[offset(0xb89bdb00):offset(0xb89bdb00)+528]):
        raise ValueError('MMIO writer slot unavailable')
    image[offset(0xb89bdb00):offset(0xb89bdb00)+528]=writer
    image[offset(0xb89bdf00):offset(0xb89bdf00)+256]=relocated+bytes(16)
    (output/'candidate.bin').write_bytes(with_digest(image))
    result=dict(writer_address='0xb89bdb00',writer_bytes=528,writer_identical=True,
                gate_address='0xb89bdf00',gate_bytes=240,reference_gate_equal=True,
                channel_address='0xb89bde00',channel_bytes=len(channel),reference_channel_equal=True)
    (output/'relocation.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path);p.add_argument('reference',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args();print(json.dumps(build(a.source,a.reference,a.output)))

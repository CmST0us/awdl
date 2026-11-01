#!/usr/bin/env python3
"""Compile freestanding U2 MIPS16 C with Clang and GNU binutils."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time


def u2_instructions(source):
    """Lower Clang's MIPS16e compact stack/branch/sign-extension operations."""
    def immediate(register, value):
        value = int(value) & 0xffffffff
        low = (value & 0xffff) - (0x10000 if value & 0x8000 else 0)
        high = ((value + 0x8000) >> 16) & 0xffff
        return f'\tli {register}, {high}\n\tsll {register}, {register}, 16\n\taddiu {register}, {low}\n'

    # The instruction extension cannot be read through the data bus.
    source = re.sub(r'\tlw\s+(\$\w+), 1f\n\s*b\s+2f\n\s*\.align\s+2\n1:\s*\.word\s+(-?\d+)\n2:\n',
                    lambda m: immediate(m[1], m[2]), source)
    constants = dict(re.findall(r'^(\$CPI\w+):\n\s*\.4byte\s+(-?\d+)', source, re.M))
    source = re.sub(r'\tlw\s+(\$\w+), (\$CPI\w+)[^\n]*\n',
                    lambda m: immediate(m[1], constants[m[2]]), source)
    if re.search(r'\blw\s+\$\w+,\s*(?:\$CPI|\d+f\b)', source):
        raise ValueError('unhandled instruction-space constant load')
    output = []
    for line in source.splitlines(True):
        instruction = line.split('#', 1)[0].strip()
        if instruction == '.text':
            output.append('.set noreorder\n')
        m = re.fullmatch(r'(save|restore)\s+(.+)', instruction)
        if m:
            args = [v.strip() for v in m[2].split(',')]
            size, registers = int(args[-1]), args[:-1]
            if set(registers) - {'$16', '$17', '$ra'} or size < 4*len(registers):
                raise ValueError('unsupported U2 stack layout: '+instruction)
            registers = [v for v in ('$ra', '$16', '$17') if v in registers]
            if m[1] == 'save':
                output.append(f'\taddiu $sp, -{size}\n')
                output += [f'\tsw {reg}, {size-4*(i+1)}($sp)\n'
                           for i,reg in enumerate(registers)]
            else:
                for i,reg in enumerate(registers):
                    dest = '$7' if reg == '$ra' else reg
                    output.append(f'\tlw {dest}, {size-4*(i+1)}($sp)\n')
                    if reg == '$ra': output.append('\tmove $ra, $7\n')
                output.append(f'\taddiu $sp, {size}\n')
            continue
        m = re.fullmatch(r'(jrc|jalrc)\s+(\$\w+)', instruction)
        if m:
            output.append(f'\t{"jr" if m[1] == "jrc" else "jalr"} {m[2]}\n\tnop\n')
            continue
        m = re.fullmatch(r'(seb|seh|zeb|zeh)\s+(\$\w+)', instruction)
        if m:
            amount = 24 if m[1][-1] == 'b' else 16
            shift = 'sra' if m[1][0] == 's' else 'srl'
            output.append(f'\tsll {m[2]}, {m[2]}, {amount}\n\t{shift} {m[2]}, {m[2]}, {amount}\n')
            continue
        output.append(line)
    return ''.join(output)


def compile_c(source, output):
    root = Path(__file__).resolve().parents[4]
    source = source.resolve().relative_to(root)
    output = output.resolve().relative_to(root)
    (root / output).parent.mkdir(parents=True, exist_ok=True)
    assembly = output.with_suffix('.s')
    command = [os.environ.get('U2_CC', 'clang'), '--target=mipsel-none-elf', '-march=mips32r2', '-mips16',
               '-mabi=32', '-mno-abicalls', '-fno-pic', '-G0', '-ffreestanding',
               '-fno-builtin', '-fno-jump-tables', '-ffunction-sections', '-fdata-sections',
               '-fno-stack-protector', '-fno-addrsig', '-Oz', '-Wall', '-Wextra',
               '-Werror', '-I' + str(Path(__file__).resolve().parents[1] / 'patches'), '-S', str(source), '-o', str(assembly)]
    commands = [command, ['mipsel-linux-gnu-as', '-EL', '-mips32r2', '-mabi=32',
                         '-o', str(output), str(assembly)]]
    results = []
    for command in commands:
        begin = time.monotonic()
        p = subprocess.run(command, cwd=root, text=True, capture_output=True, timeout=5)
        results.append(dict(command=command, returncode=p.returncode,
                            seconds=time.monotonic()-begin,
                            stdout=p.stdout, stderr=p.stderr))
        (root / output.with_suffix('.build.json')).write_text(
            json.dumps(results, indent=2) + '\n')
        p.check_returncode()
        if command == commands[0]:
            original = (root / assembly).read_text()
            (root / output.with_suffix('.clang.s')).write_text(original)
            (root / assembly).write_text(u2_instructions(original))
    return root / output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    compile_c(args.source, args.output)

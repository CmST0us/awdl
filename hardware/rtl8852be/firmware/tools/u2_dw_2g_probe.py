#!/usr/bin/env python3
"""Build a 2.4 GHz-only U2 DW phase ablation module without recompiling the driver."""
import argparse
from pathlib import Path

from u2fw import extract_embedded, with_digest


def patch_module(source, destination):
    offset, image = extract_embedded(source)
    # module0 initial-window selector: next 128 TU versus next 512 TU.
    position = 0xB89A83B8 - 0xB8970000 + 0xA0
    if len(image) != 325304 or image[position:position + 4] != bytes.fromhex('80 5c 35 60'):
        raise ValueError('expected U2 sltiu a0,128; bteqz at 0xb89a83b8')
    candidate = bytearray(image)
    candidate[position] = 0  # sltiu a0,0 selects the existing next-512TU branch.
    candidate = with_digest(candidate)
    module = bytearray(Path(source).read_bytes())
    module[offset:offset + len(candidate)] = candidate
    Path(destination).write_bytes(module)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.baseline.resolve() == args.output.resolve():
        parser.error('output must differ from baseline')
    patch_module(args.baseline, args.output)

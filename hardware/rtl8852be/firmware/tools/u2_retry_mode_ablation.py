#!/usr/bin/env python3
"""Build a diagnostic U2 variant retaining AGG retry mode at the native setter."""
import argparse
import json
from pathlib import Path
from u2fw import with_digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    image = bytearray(args.source.read_bytes())
    address = 0xb89993e8
    offset = address - 0xb8970000 + 0xa0
    if len(image) != 337592 or image[offset:offset+4] != bytes.fromhex('036cd2f4'):
        raise ValueError('expected native U2 retry-mode setter')
    # The setter clears bits1:0 then ORs (*argument & mask). Keep mode zero.
    image[offset:offset+2] = bytes.fromhex('006c')
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output/'candidate.bin').write_bytes(with_digest(image))
    result = dict(address=hex(address), old='li a0,3', new='li a0,0',
                  scope='Diagnostic ablation only; original per-object state transitions retained.')
    (args.output/'build.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()

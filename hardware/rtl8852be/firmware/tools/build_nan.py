#!/usr/bin/env python3
"""Build the dual-band U2 NAN image from the original firmware and patches."""
import argparse
from pathlib import Path
import hashlib
import importlib
import json
from u2fw import with_digest, embed_image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--array', type=Path, help='replace the U2 array in a generated driver tree')
    args = parser.parse_args()
    template = args.array.read_text() if args.array else None
    root = Path(__file__).resolve().parents[1]
    spec = json.loads((root / 'source.json').read_text())
    current = root / spec['file']
    baseline = current.read_bytes()
    if len(baseline) != spec['size'] or hashlib.sha256(baseline).hexdigest() != spec['sha256']:
        raise ValueError('firmware does not match the pinned U2 baseline')
    args.output.mkdir(parents=True, exist_ok=False)
    for entry in (root / 'patches/series').read_text().splitlines():
        entry = entry.split('#', 1)[0].strip()
        if not entry:
            continue
        step = json.loads((root / 'patches' / entry).read_text())
        name = step['name']
        target = args.output / name
        if 'module' in step:
            module = importlib.import_module(step['module'])
            module.build(current, target, **step.get('options', {}))
        else:
            image = bytearray(current.read_bytes())
            offset = int(step['offset'], 0)
            expected = bytes.fromhex(step['expected'])
            replacement = bytes.fromhex(step['replacement'])
            if len(expected) != len(replacement) or image[offset:offset + len(expected)] != expected:
                raise ValueError(f'{entry}: unexpected patch bytes')
            image[offset:offset + len(expected)] = replacement
            target.mkdir()
            (target / 'candidate.bin').write_bytes(with_digest(image))
        current = target / 'candidate.bin'
        print(name + ': complete', flush=True)
    image = current.read_bytes()
    (args.output / 'candidate.bin').write_bytes(image)
    if args.array:
        embed_image(args.array, image, template)


if __name__ == '__main__':
    main()

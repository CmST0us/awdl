#!/usr/bin/env python3
"""Read the MT792x release-info capability TLV without changing firmware."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def inspect(path):
    path = Path(path)
    data = (subprocess.check_output(['zstd', '-dc', str(path)])
            if path.suffix == '.zst' else path.read_bytes())
    if len(data) < 36:
        raise ValueError('firmware trailer is missing')
    trailer = data[-36:]
    regions = trailer[2]
    region_start = len(data) - 36 - regions * 40
    if not regions or region_start < 0:
        raise ValueError('invalid firmware region count')
    payload_size = sum(struct.unpack_from('<I', data, region_start + i * 40 + 20)[0]
                       for i in range(regions))
    offset = payload_size + 16
    if offset + 4 > region_start:
        raise ValueError('release-info header is outside payload')
    length, _, _ = struct.unpack_from('<HBB', data, offset)
    offset += 4
    end = offset + length
    if end > region_start:
        raise ValueError('release-info exceeds firmware regions')
    capability = 0
    segment = None
    while offset < end:
        if offset + 4 > end:
            raise ValueError('truncated release-info TLV')
        length, padding, tag = struct.unpack_from('<HBB', data, offset)
        offset += 4
        if offset + length + padding > end:
            raise ValueError('release-info TLV exceeds container')
        if tag == 4:
            if length < 16:
                raise ValueError('truncated firmware feature TLV')
            segment, capability = data[offset:offset + 2]
            break
        offset += length + padding
    return {
        'sha256': hashlib.sha256(data).hexdigest(),
        'size_bytes': len(data),
        'build': trailer[17:32].rstrip(b'\0').decode('ascii'),
        'feature_segment': segment,
        'features': capability,
        'nan': bool(capability & 0x20),
        'cnm': bool(capability & 0x80),
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('firmware', type=Path)
    args = parser.parse_args()
    print(json.dumps(inspect(args.firmware), indent=2))

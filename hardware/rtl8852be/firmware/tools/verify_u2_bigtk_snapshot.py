#!/usr/bin/env python3
"""Independently verify both native BIGTK snapshot frames with OpenSSL."""
import argparse
import json
from pathlib import Path
import struct
import subprocess


def verify(path, openssl):
    data = path.read_bytes()
    if len(data) != 204 or data[1] not in (6, 7) or data[15] != 1:
        raise ValueError('BIGTK snapshot ABI or active owner mismatch')
    prepared, completed, skipped, error, us = struct.unpack_from('<5I', data, 16)
    if error or completed < 4 or prepared < completed:
        raise ValueError('native beacon completion incomplete')
    high = int.from_bytes(data[8:14], 'little')
    frames = []
    for i in range(2):
        f = data[36+84*i:36+84*i+81]
        pn = int.from_bytes(f[67:73], 'little')
        if f[0] != 0x80 or f[10:16] != data[2:8] or not 0 < pn <= high or \
                f[63:67] != bytes((76,16,data[1],0)):
            raise ValueError('beacon owner, MMIE or IPN mismatch')
        aad = bytes((f[0], f[1] & ~0x38)) + f[4:22]
        p = subprocess.run([openssl, 'mac', '-cipher', 'AES-128-CBC', '-macopt',
                            'hexkey:2b7e151628aed2a6abf7158809cf4f3c', 'CMAC'],
                           input=aad+bytes(8)+f[32:-8]+bytes(8),
                           capture_output=True, check=True, timeout=5)
        if bytes.fromhex(p.stdout.decode().strip())[:8] != f[73:81]:
            raise ValueError('beacon MIC mismatch')
        frames.append(dict(slot=i, ipn=pn, mic='PASS'))
    if frames[0]['ipn'] == frames[1]['ipn']:
        raise ValueError('IPN reused across native ports')
    return dict(status='PASS', prepared=prepared, completed=completed,
                skipped=skipped, calculation_us=us, frames=frames)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshot', type=Path)
    parser.add_argument('--openssl', required=True)
    args = parser.parse_args()
    print(json.dumps(verify(args.snapshot, args.openssl)))

#!/usr/bin/env python3
"""Prepare pinned MT7925 firmware and kernel inputs under .build; never install."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tarfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUTPUT = ROOT / '.build/dependencies/mt7925'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def fetch(url, path, expected):
    if not path.exists():
        partial = path.with_name(path.name + '.part')
        subprocess.run(['curl', '-fL', '--retry', '2', '--max-time', '300',
                        url, '-o', str(partial)], check=True)
        if digest(partial) != expected:
            raise ValueError(f'SHA-256 mismatch: {partial}')
        partial.rename(path)
    if digest(path) != expected:
        raise ValueError(f'SHA-256 mismatch: {path}')
    print(f'verified {path.name}', flush=True)


def unpack(source, destination, patches=()):
    archive = OUTPUT / source['file']
    fetch(source['url'], archive, source['sha256'])
    inputs = {'archive': source['sha256'],
              'patches': [digest(patch) for patch in patches]}
    marker = destination / '.prepared.json'
    if marker.exists() and json.loads(marker.read_text()) == inputs:
        return
    if destination.exists():
        raise RuntimeError(f'Incomplete or changed source inputs at {destination}; '
                           'preserve any diagnostics and remove only this rebuildable directory')
    # Both kernel archives expand to roughly 4 GiB together. Leave build headroom.
    if shutil.disk_usage(OUTPUT).free < 8 * 1024 ** 3:
        raise RuntimeError('At least 8 GiB free space is required before extraction')
    destination.mkdir(parents=True)
    with tarfile.open(archive) as tar:
        prefix = source['directory'] + '/'
        for member in tar.getmembers():
            if member.name == source['directory']:
                continue
            if not member.name.startswith(prefix):
                raise ValueError(f'Unexpected archive root: {member.name}')
            member.name = member.name[len(prefix):]
            tar.extract(member, destination, filter='data')
    for patch in patches:
        subprocess.run(['patch', '--batch', '--forward', '--fuzz=0', '-p1',
                        '-i', str(patch)], cwd=destination, check=True)
    marker.write_text(json.dumps(inputs, indent=2) + '\n')


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    firmware = json.loads((HERE / 'firmware/source.json').read_text())
    fw_dir = OUTPUT / 'firmware'
    fw_dir.mkdir(exist_ok=True)
    for entry in firmware['files']:
        url = (firmware['repository'].removesuffix('.git') + '/-/raw/' +
               firmware['commit'] + '/' + entry['upstream_path'])
        fetch(url, fw_dir / entry['file'], entry['sha256'])
    spec = importlib.util.spec_from_file_location('mt7925_firmware', HERE / 'firmware/inspect.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    capabilities = module.inspect(fw_dir / 'WIFI_RAM_CODE_MT7925_1_1.bin')
    required = firmware['required_features']
    if capabilities['features'] & required != required:
        raise RuntimeError('Pinned firmware does not advertise both NAN and CNM')
    if capabilities['build'] != firmware['version']:
        raise RuntimeError('Pinned firmware build does not match manifest')
    print(json.dumps(capabilities), flush=True)

    driver = json.loads((HERE / 'driver/source.json').read_text())
    series = HERE / 'driver' / driver['patch_series']
    patches = [(series.parent / line).resolve() for line in series.read_text().splitlines()
               if line and not line.startswith('#')]
    unpack(driver, OUTPUT / 'source/wireless-next', patches)
    platform = json.loads((ROOT / 'platforms/dragon-q8b/source.json').read_text())
    unpack(platform['kernel'], OUTPUT / 'source/radxa-kernel')
    unpack(platform['packaging'], OUTPUT / 'source/linux-qcom')
    print('Prepared firmware, patched wireless kernel and Q8B platform reference; '
          'these are source inputs, not a built or boot-verified Q8B kernel.')


if __name__ == '__main__':
    main()

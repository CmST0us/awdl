#!/usr/bin/env python3
"""取得固定上游源码并按顺序应用补丁。"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[3]


def apply_series(source: Path, directory: Path) -> None:
    for line in (directory / 'series').read_text().splitlines():
        name = line.split('#', 1)[0].strip()
        if name:
            # macOS 的 patch 对 git 删除补丁只清空文件，-E 使删除结果与 GNU patch 一致。
            subprocess.run(['patch', '--batch', '--forward', '--fuzz=0', '-E', '-p1',
                            '-i', str(directory / name)], cwd=source, check=True)


def download(url: str, path: Path) -> None:
    with urlopen(url, timeout=5) as response, path.open('wb') as output:
        shutil.copyfileobj(response, output)


def prepare_driver(archive: Path | None, output: Path | None) -> None:
    directory = ROOT / 'hardware/rtl8852be/driver'
    spec = json.loads((directory / 'source.json').read_text())
    output = (output or ROOT / '.build/driver/source').resolve()
    series = directory / 'patches/series'
    inputs = [directory / 'source.json', series, directory / 'include/wa_nan_uapi.h']
    inputs += [directory / 'patches' / line.split('#', 1)[0].strip()
               for line in series.read_text().splitlines() if line.split('#', 1)[0].strip()]
    recorded = output / '.project-inputs'
    if output.exists() and all((recorded / p.relative_to(directory)).is_file() and
            p.read_bytes() == (recorded / p.relative_to(directory)).read_bytes() for p in inputs):
        print(output)
        return
    if archive is None:
        archive = ROOT / '.build/downloads/rkwifibt.tar.gz'
        if not archive.exists():
            archive.parent.mkdir(parents=True, exist_ok=True)
            download(spec['archive'], archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != spec['sha256']:
        raise ValueError('驱动基线归档与 source.json 不一致')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='driver-', dir=output.parent) as temp:
        source = Path(temp) / 'source'
        source.mkdir()
        prefix = spec['directory'] + '/'
        with tarfile.open(archive) as package:
            for member in package.getmembers():
                if member.name.startswith(prefix):
                    member.name = member.name[len(prefix):]
                    if member.name:
                        package.extract(member, source, filter='data')
        apply_series(source, directory / 'patches')
        # Userspace and the standalone kernel tree must expose the same vendor ABI.
        if any((source / name).read_bytes() != (directory / 'include/wa_nan_uapi.h').read_bytes()
               for name in ('include/wa_nan_uapi.h', 'phl/wa_nan_uapi.h')):
            raise ValueError('补丁中的 UAPI 与用户态公开头文件不一致')
        for item in inputs:
            target = source / '.project-inputs' / item.relative_to(directory)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(item, target)
        if output.exists():
            shutil.rmtree(output)
        source.rename(output)
    print(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    prepare_driver(args.archive, args.out)

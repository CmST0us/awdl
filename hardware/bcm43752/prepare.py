#!/usr/bin/env python3
"""Expand the pinned DHD source and replay its ordered native NAN patches."""

import argparse
import io
import json
from pathlib import Path
import subprocess
import tarfile
import shutil
import hashlib
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    origin = parser.add_mutually_exclusive_group()
    origin.add_argument("--source-checkout", type=Path,
                        help="local rockchip-linux/kernel Git repository containing the pinned commit")
    origin.add_argument("--archive", type=Path,
                        help="exact git-archive tar identified by driver/source.json")
    parser.add_argument("--output", required=True, type=Path,
                        help="expanded-source directory below this project's .build")
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent / "driver"
    source = json.loads((directory / "source.json").read_text())
    root = directory.parents[2]
    output = args.output.resolve()
    try:
        if not output.is_relative_to(root / ".build"):
            raise ValueError("expanded upstream source must be placed under .build")
        series = directory / source["patch_series"]
        patches = [series.parent / line.split("#", 1)[0].strip()
                   for line in series.read_text().splitlines()
                   if line.split("#", 1)[0].strip()]
        owned = [directory / n for n in ("wl_nan_host.c", "wl_nan_host.h",
                                         "wl_nan_ndi.c", "wl_nan_ndi.h")]
        headers = [directory.parent / "include" / n
                   for n in ("nan_host_abi.h", "nan_vendor_abi.h")]
        inputs = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in [Path(__file__).resolve(), directory / "source.json", series,
                            *patches, *owned, *headers]}
        marker = output / ".project-inputs.json"
        if output.exists():
            if not marker.is_file():
                raise ValueError(f"refusing to replace unrecorded source: {output}")
            if json.loads(marker.read_text()) == inputs:
                print(f"Unchanged source at {output}")
                return
        if args.source_checkout:
            archive = subprocess.check_output([
                "git", "-C", str(args.source_checkout), "archive", "--format=tar",
                source["commit"] + ":" + source["directory"],
            ])
        else:
            archive = (args.archive or root / ".build/downloads/bcm43752-dhd-77168c8d.tar").read_bytes()
        if hashlib.sha256(archive).hexdigest() != source["archive_sha256"]:
            raise ValueError("DHD archive does not match the pinned source")
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="bcm43752-", dir=output.parent) as temporary:
            expanded = Path(temporary) / "source"
            expanded.mkdir()
            with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
                tar.extractall(expanded, filter="data")
            # Project-owned transport and its single ABI sources are copied into
            # the upstream build tree; generated copies are never edited inputs.
            for item in owned:
                shutil.copyfile(item, expanded / item.name)
            for item in headers:
                shutil.copyfile(item, expanded / "include" / item.name)
            for patch in patches:
                subprocess.run(["git", "apply", "--check", str(patch)], cwd=expanded, check=True)
                subprocess.run(["git", "apply", str(patch)], cwd=expanded, check=True)
            (expanded / marker.name).write_text(json.dumps(inputs, sort_keys=True, indent=2) + "\n")
            if output.exists():
                shutil.rmtree(output)
            expanded.rename(output)
        print(f"Prepared {source['commit']} at {output}")
    except (OSError, ValueError, subprocess.CalledProcessError, tarfile.TarError) as exc:
        parser.exit(1, f"prepare failed (partial output retained for inspection): {exc}\n")


if __name__ == "__main__":
    main()

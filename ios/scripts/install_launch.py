#!/usr/bin/env python3
"""Install this project's App and Runner while holding the shared device lease."""
from app_identity import APP_BUNDLE
import argparse
import datetime
import json
import os
from pathlib import Path
import subprocess
from publisher_observation.device_lease import DeviceLease

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--launch', action='store_true', help='launch App after both installs complete')
args = parser.parse_args()
udid = os.environ['AWARE_DEVICE_UDID']
derived = Path(os.environ.get('AWARE_DERIVED_DATA', root / '.private/ios/DerivedData')).resolve()
products = derived / 'Build/Products/Debug-iphoneos'
run_id = 'IOS-INSTALL-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
out = root / '.private/artifacts' / run_id
out.mkdir(parents=True, exist_ok=False)
with DeviceLease(udid, run_id):
    for index, name in enumerate(('AwareLab.app', 'AwareLabUITests-Runner.app'), 1):
        path = out / f'install-{index}.json'
        with (out / f'install-{index}.log').open('w') as log:
            subprocess.run(['xcrun', 'devicectl', 'device', 'install', 'app', '--device', udid,
                            str(products / name), '--timeout', '5', '--json-output', str(path)],
                           stdout=log, stderr=subprocess.STDOUT, timeout=5, check=True)
        if json.loads(path.read_text())['info']['outcome'] != 'success':
            raise RuntimeError(f'Installation failed: {name}')
    if args.launch:
        with (out / 'launch.log').open('w') as log:
            subprocess.run(['xcrun', 'devicectl', 'device', 'process', 'launch', '--device', udid,
                            APP_BUNDLE, '--timeout', '5',
                            '--json-output', str(out / 'launch.json')],
                           stdout=log, stderr=subprocess.STDOUT, timeout=5, check=True)
print(out)

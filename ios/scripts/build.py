#!/usr/bin/env python3
"""Build signed physical-device artifacts with a bounded completion wait."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--provision', action='store_true', help='fetch or create development profiles in the configured team')
args = parser.parse_args()

root = Path(__file__).resolve().parents[2]
derived = Path(os.environ.get('AWARE_DERIVED_DATA', root / '.private/ios/DerivedData')).resolve()
derived.mkdir(parents=True, exist_ok=True)
# Full App and test compilation completed in 9.807 s; use twice that time.
budget = int(os.environ.get('AWARE_BUILD_TIMEOUT', '20'))
if not 1 <= budget <= 29:
    raise SystemExit('AWARE_BUILD_TIMEOUT must be within 1..29 seconds')
cmd = ['xcodebuild', '-project', str(root / 'ios/AwareLab.xcodeproj'),
       '-scheme', 'AwareLab', '-configuration', 'Debug',
       '-destination', 'id=' + os.environ['AWARE_DEVICE_UDID'],
       '-derivedDataPath', str(derived),
       'DEVELOPMENT_TEAM=' + os.environ['DEVELOPMENT_TEAM'], 'build-for-testing']
if args.provision:
    cmd += ['-allowProvisioningUpdates', '-allowProvisioningDeviceRegistration']
from publisher_observation.device_lease import DeviceLease
lease = DeviceLease(os.environ['AWARE_DEVICE_UDID'], 'IOS-BUILD')
start = time.monotonic()
with (derived.parent / 'build.log').open('w') as log:
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        rc = proc.wait(timeout=budget)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            proc.wait(timeout=0.5)
        except subprocess.TimeoutExpired:
            proc.kill()
            try:
                proc.wait(timeout=0.5)
            except subprocess.TimeoutExpired:
                print(f"Build process {proc.pid} has not exited after SIGKILL", file=sys.stderr)
        rc = 124
lease.__exit__()
print(f'build exit={rc} elapsed={time.monotonic()-start:.3f}s log={derived.parent / "build.log"}')
sys.exit(rc)

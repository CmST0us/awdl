#!/usr/bin/env python3
"""Capture once through an existing Apple tunnel in an exclusive 8 s window."""
import argparse
import asyncio
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


from publisher_observation.native_transport import configure_native


async def capture_once(tunnel, service_type, output, deadline):
    result = dict(screenshot=False, handle_closed=False, pairing_requests=0,
                  rsd_attempts_limit=1)

    async def capture():
        rsd = await tunnel.aopen()
        result['rsd_connected'] = True
        async with service_type(rsd) as service:
            response = await service.capture_screenshot()
        (output / 'screen.png').write_bytes(response['image'])
        result['screenshot'] = True

    try:
        await asyncio.wait_for(capture(), max(.001, deadline - time.monotonic()))
    except Exception as error:
        result['error'] = type(error).__name__ + ': ' + str(error)
    finally:
        try:
            await asyncio.wait_for(tunnel.aclose(), .8)
            result['handle_closed'] = True
        except Exception as error:
            result['close_error'] = type(error).__name__ + ': ' + str(error)
        (output / 'capture-result.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def child(args):
    from pymobiledevice3.remote import native_tunnel
    from pymobiledevice3.remote.core_device.screen_capture_service import ScreenCaptureService
    configure_native(native_tunnel)
    loop = asyncio.new_event_loop()
    try:
        result = loop.run_until_complete(capture_once(
            native_tunnel.NativeRemotedTunnel(serial=args.udid), ScreenCaptureService,
            args.output, args.child_deadline))
    finally:
        loop.close()
    # The library uses worker threads for XPC. Do not let their shutdown extend
    # the device window after the explicit handle close has completed or failed.
    os._exit(0 if result['screenshot'] and result['handle_closed'] else 1)


def run(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    start = time.monotonic()
    forced = False
    argv = [sys.executable, str(Path(__file__).resolve()), '--udid', args.udid,
            '--output', str(output), '--child-deadline', str(start + 5)]
    with (output / 'stdout.log').open('w') as stdout, (output / 'stderr.log').open('w') as stderr:
        process = subprocess.Popen(argv, stdout=stdout, stderr=stderr, start_new_session=True)
        try:
            capture_rc = process.wait(timeout=max(.001, start + 6 - time.monotonic()))
        except subprocess.TimeoutExpired:
            forced = True
            os.killpg(process.pid, signal.SIGKILL)
            capture_rc = process.wait(timeout=.1)
    capture_elapsed = time.monotonic() - start
    argv = ['xcrun', 'devicectl', 'device', 'info', 'details', '--device', args.udid,
            '--json-output', str(output / 'connection.json')]
    with (output / 'connection.log').open('w') as log:
        try:
            check = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT,
                                   timeout=max(.001, min(2, start + 8 - time.monotonic())))
            connection_rc = check.returncode
        except subprocess.TimeoutExpired:
            connection_rc = 124
    connected = False
    connection_error = None
    if connection_rc == 0:
        try:
            reply = json.loads((output / 'connection.json').read_text())
            device = reply['result']
            connected = (reply['info']['outcome'] == 'success'
                         and device['hardwareProperties']['udid'] == args.udid
                         and device['connectionProperties']['tunnelState'] == 'connected')
        except (OSError, ValueError, KeyError) as error:
            connection_error = str(error)
    result = dict(elapsed_seconds=time.monotonic() - start, capture_elapsed=capture_elapsed,
                  capture_rc=capture_rc, forced_kill=forced, connection_rc=connection_rc,
                  connection_confirmed=connected, connection_error=connection_error,
                  device_window_released=True)
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
    return 0 if capture_rc == 0 and connected and not forced else 1


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--udid', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--child-deadline', type=float, help=argparse.SUPPRESS)
    return parser.parse_args(argv)


if __name__ == '__main__':
    args = parse_args()
    if args.child_deadline is not None:
        child(args)
    else:
        raise SystemExit(run(args))

#!/usr/bin/env python3
"""Independent 900-second watchdog for exactly one capture process."""
import argparse
import json
import os
import pathlib
import signal
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]
PRODUCTION_TIMEOUT_SECONDS = 900


def supervise(command, output, log_path, *, timeout_seconds=PRODUCTION_TIMEOUT_SECONDS):
    """Record this attempt only; direction-wide failure counts belong to the ledger.

    The timeout argument supports offline tests. Production always supplies 900.
    Leave output absent until the child exits: capture owns its initial mkdir.
    """
    if output.exists():
        raise FileExistsError('Evidence directory exists; refusing repeat')
    log_path.parent.mkdir(parents=True, exist_ok=True)
    started = 0
    failure = None
    with log_path.open('w') as log:
        try:
            process = subprocess.Popen(
                command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        except OSError as exc:
            rc = 127 if isinstance(exc, FileNotFoundError) else 126
            failure = {'status': 'FAIL', 'phase': 'process_start',
                       'error_type': type(exc).__name__, 'error': str(exc),
                       'device_capture_may_continue': False}
        else:
            started = 1
            try:
                rc = process.wait(timeout=timeout_seconds)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass  # Child may have exited at the watchdog boundary.
                process.wait()
                rc = 124
                failure = {'status': 'FAIL', 'phase': 'watchdog',
                           'error': f'{timeout_seconds}s hard watchdog',
                           'device_capture_may_continue': True}

    # uvx/dependency loading can fail before capture creates its evidence directory.
    output.mkdir(parents=True, exist_ok=True)
    result_path = output / 'result.json'
    if failure is not None:
        result_path.write_text(json.dumps(failure, indent=2))
    elif rc != 0 and not result_path.exists():
        result_path.write_text(json.dumps({
            'status': 'FAIL', 'phase': 'child_exit', 'returncode': rc,
            'error': 'Child exited without an attempt result',
            'device_capture_may_continue': 'unknown'}, indent=2))
    (output / 'supervisor.json').write_text(json.dumps({
        'returncode': rc, 'hard_timeout_seconds': timeout_seconds,
        'capture_processes_started': started, 'log': str(log_path)}, indent=2))
    return rc


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--run-id', required=True)
    args = parser.parse_args()
    if not args.run_id or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in args.run_id):
        parser.error('invalid run id')
    config = {}
    env_path = pathlib.Path(os.environ.get('AWARE_LOCAL_ENV', ROOT / '.private/IOS-M1-001/local.env'))
    for line in env_path.read_text().splitlines():
        if line.strip() and not line.startswith('#'):
            key, value = line.split('=', 1)
            config[key] = value
    output = ROOT / '.private/artifacts' / args.run_id
    log_path = ROOT / '.private/IOS-M1-001' / (args.run_id + '-supervisor.log')
    command = ['uvx', '--from', 'pymobiledevice3==11.41.2', 'python',
               str(ROOT / 'ios/scripts/capture_sysdiagnose.py'),
               '--udid', config['IPAD_UDID'], '--output', str(output)]
    rc = supervise(command, output, log_path, timeout_seconds=PRODUCTION_TIMEOUT_SECONDS)
    print(json.dumps({'output': str(output), 'returncode': rc}), flush=True)
    return rc


if __name__ == '__main__':
    raise SystemExit(main())

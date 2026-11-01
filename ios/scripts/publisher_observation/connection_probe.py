#!/usr/bin/env python3
"""Observe one connection to a saved paired device with both log sources."""
import json
import os
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import plistlib
import queue
import signal
import subprocess
import threading
import time

from device_control import cleanup_ipad, destination_spec
from device_lease import DeviceLease
from subscription_probe import close_streams


def execute(config, out):
    with DeviceLease(config['udid'], config['run_id']):
        return observe(config, out)


def observe(config, out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=False)
    run_id = config['run_id']
    destination_spec(config['xctestrun'], out / 'destination.xctestrun', run_id)
    spec = plistlib.loads((out / 'destination.xctestrun').read_bytes())
    spec['AwareLabUITests']['EnvironmentVariables']['AWARE_PAIRED_DEVICE_ID'] = str(config['paired_device_id'])
    (out / 'destination.xctestrun').write_bytes(plistlib.dumps(spec))
    inbox = queue.Queue()
    procs, readers, files, stats = {}, {}, [], {}
    start = time.monotonic()
    active_end, deadline = start + 22, start + 26
    observed = None
    error = None
    board_rc = None
    lock = threading.Lock()

    def record(event, **fields):
        with lock, (out / 'timeline.jsonl').open('a') as log:
            log.write(json.dumps(dict(event=event, run_id=run_id,
                                     monotonic=time.monotonic(), **fields)) + '\n')

    def spawn(name, argv, pid=None):
        err = (out / (name + '.stderr')).open('w')
        files.append(err)
        proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=err, text=True, bufsize=1, start_new_session=True)
        procs[name] = proc
        stats[name] = dict(pid=pid, ready=False, complete=False, valid=0, invalid=0)

        def read():
            with (out / (name + '.log')).open('w') as log:
                for line in proc.stdout:
                    log.write(line)
                    log.flush()
                    inbox.put((name, line))
            inbox.put((name, None))

        readers[name] = threading.Thread(target=read, daemon=True)
        readers[name].start()

    def accept(name, line):
        nonlocal observed, board_rc
        if line is None:
            return
        if name == 'ui':
            if 'AWARE_CONNECTION ' not in line:
                return
            row = json.loads(line.split('AWARE_CONNECTION ', 1)[1])
            if row.get('experiment_id') != run_id:
                return
            record(row['event'], marker=row)
            if row['event'] == 'connection_ready' and board_rc is None:
                with (out / 'board-start.log').open('w') as log:
                    result = subprocess.run(config['start_argv'], stdout=log,
                                            stderr=subprocess.STDOUT, timeout=5)
                board_rc = result.returncode
                if board_rc:
                    raise RuntimeError('board START failed')
            elif row['event'] == 'connection_observed':
                observed = row
            return
        state = stats[name]
        try:
            row = json.loads(line)
        except ValueError:
            state['invalid'] += 1
            return
        if row.get('event') in ('collector-ready', 'collector-complete'):
            if row.get('run_id') == run_id and row.get('requested_pid') == state['pid']:
                state['ready' if row['event'] == 'collector-ready' else 'complete'] = True
        else:
            state['valid'] += 1

    try:
        for name, pid in [('all', -1), ('pid', config['deviceaccessd_pid'])]:
            spawn(name, [config['python'], config['collector'], '--udid', config['udid'],
                         '--run-id', run_id, '--pid', str(pid)], pid)
        while time.monotonic() < active_end:
            if 'ui' not in procs:
                if all(stats[n]['ready'] for n in ('all', 'pid')):
                    spawn('ui', ['xcodebuild', '-xctestrun', str(out / 'destination.xctestrun'),
                                 '-destination', 'id=' + config['udid'], '-resultBundlePath',
                                 str(out / 'tests.xcresult'), '-collect-test-diagnostics', 'never',
                                 '-parallel-testing-enabled', 'NO',
                                 '-only-testing:AwareLabUITests/M5PairingTests/testCachedConnectionObservation',
                                 'test-without-building'])
                elif time.monotonic() >= start + 2:
                    raise RuntimeError('collector readiness exceeded 2 seconds')
            try:
                name, line = inbox.get(timeout=max(.001, min(1, active_end - time.monotonic())))
            except queue.Empty:
                continue
            if line is None:
                raise RuntimeError(name + ' exited before terminal marker')
            accept(name, line)
            if observed:
                break
        if not observed:
            raise RuntimeError('connection observation exceeded activity deadline')
    except Exception as exc:
        error = str(exc)
        record('error', error=error)
    finally:
        if 'ui' in procs and procs['ui'].poll() is None:
            os.killpg(procs['ui'].pid, signal.SIGTERM)
        clean = cleanup_ipad(config['udid'], out, deadline - 1, record) if 'ui' in procs else True
        streams = {n: procs[n] for n in ('all', 'pid') if n in procs}
        forced = close_streams(streams, readers, min(deadline, time.monotonic() + 1), record)
        while not inbox.empty():
            name, line = inbox.get_nowait()
            if name != 'ui':
                accept(name, line)
        for name, proc in streams.items():
            stats[name].update(forced=forced[name], returncode=proc.poll(), eof=not readers[name].is_alive())
        complete = len(streams) == 2 and all(
            stats[n]['complete'] and stats[n]['valid'] and not stats[n]['invalid']
            and not forced[n] and stats[n]['eof'] and stats[n]['returncode'] == 0 for n in streams)
        result = dict(run_id=run_id, observed=observed, error=error, board_start_rc=board_rc,
                      collectors=stats, collectors_complete=complete, cleanup=clean,
                      elapsed_seconds=time.monotonic() - start)
        (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
        for file in files:
            file.close()
    return 0 if observed and error is None and clean and complete and board_rc == 0 else 1

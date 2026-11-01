#!/usr/bin/env python3
"""U2 日志修改的单次原生启停与原模块恢复。"""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import time
sys.dont_write_bytecode = True
from publication import Session, handshake
from board import Uncertain, require

MARKER = re.compile(r'C2H log: \[NAN\] FAIL in func: 255, line: 2971, ret = (\d+)')
OLD = re.compile(r'C2H log: \[NAN\] FAIL in func: 255, line: 2953, ret = (\d+)')


def load_evidence(rows):
    submitted = [x for x in rows if 'WA_OBS download ' in x and 'symbol=array_8852b_u2_nicce len=325304 ' in x]
    completed = [x for x in rows if re.search(r'WA_OBS attempt-end call=\d+ attempt=\d+ result=0\b', x)]
    failed = [x for x in rows if re.search(r'WA_OBS attempt-end call=\d+ attempt=\d+ result=[1-9]\d*\b', x)]
    ready = [x for x in rows if 'hal_mac_print_fw_version: FW version ' in x and ', Ready' in x]
    calls = [re.search(r'fwdl_call=(\d+)\b', x)[1] for x in submitted if re.search(r'fwdl_call=(\d+)\b', x)]
    done = [re.search(r'call=(\d+)\b', x)[1] for x in completed]
    coherent = bool(calls) and len(calls) == len(submitted) and len(set(calls)) == len(calls) and sorted(calls) == sorted(done)
    return dict(submission=submitted, normal_download_completed=completed, init_ready=ready, failed_attempts=failed, calls_coherent=coherent)


def marker_evidence(rows):
    starts = [(i, re.search(r'phase=stageb-ready op=(\d+)\b', x)) for i, x in enumerate(rows)]
    starts = [(i, m[1]) for i, m in starts if m]
    if len(starts) != 1:
        return None
    index, operation = starts[0]
    markers = [dict(record=x, state=int(MARKER.search(x)[1])) for x in rows[index:] if MARKER.search(x)]
    if not markers:
        return None
    require(not any(OLD.search(x) for x in rows), 'baseline line appeared in candidate START')
    return dict(stageb_operation=operation, markers=markers)


class MarkerSession(Session):
    def load_candidate(self):
        with self.changed:
            self.load_begin = len(self.kernel_rows)
        super().load_candidate()
        self.wait_event(lambda: bool(load_evidence(self.kernel_rows[self.load_begin:])['init_ready']), host_required=False)
        with self.changed:
            self.loading = load_evidence(self.kernel_rows[self.load_begin:])
        self.save('loading.json', self.loading)
        require(self.loading['calls_coherent'], 'normal download call/completion mismatch')
        require(not self.loading['failed_attempts'], 'firmware download attempt failed')

    def start(self):
        with self.changed:
            self.start_begin = len(self.kernel_rows)
        super().start()

    def observe_marker(self):
        self.wait_event(lambda: marker_evidence(self.kernel_rows[self.start_begin:]) is not None)
        with self.changed:
            evidence = marker_evidence(self.kernel_rows[self.start_begin:])
        evidence.update(cookie=self.first_frame(), candidate_srcversion=self.candidate_src,
                        boot=self.baseline_state['boot'], **self.identity)
        self.save('marker.json', evidence)
        return evidence


def execute(io):
    result = dict(status='FAILED', native_stop=False, restored=False, execution_status='NOT_PROVEN')
    removed = False
    uncertain = False
    try:
        io.phase('baseline', 2); io.baseline()
        io.forward_budget(1); io.phase('unload-original', 1); io.remove(); removed = True
        io.forward_budget(2); io.phase('load-candidate', 2); io.load_candidate()
        result['loading'] = io.loading
        io.forward_budget(1); io.phase('publisher-ready', 1); io.launch()
        io.forward_budget(2); io.phase('start-first-frame', 2); io.start(); result['first_frame'] = True
        io.forward_budget(1); io.phase('firmware-marker', 1)
        result['marker'] = io.observe_marker(); result['execution_status'] = 'PASS'
    except Uncertain as error:
        uncertain = True; result['error'] = str(error)
    except Exception as error:
        result['error'] = str(error)
    finally:
        safe = not uncertain
        if io.publisher is not None and safe:
            try:
                io.phase('native-stop', 2); io.stop(); result['native_stop'] = True
            except Exception as error:
                safe = False; result['stop_error'] = str(error)
        if removed and safe:
            try:
                if io.loaded():
                    io.phase('unload-candidate', 1); io.remove()
                io.phase('restore-original', 2); io.restore()
                io.phase('readback', 1); io.readback(); result['restored'] = True
            except Exception as error:
                safe = False; result['restore_error'] = str(error)
        try:
            io.phase('close', 1); io.close(); result['capture_complete'] = True
        except Exception as error:
            result['capture_error'] = str(error)
        result['retain_device'] = not safe
        if not safe or (result.get('first_frame') and result['execution_status'] != 'PASS'):
            result['status'] = 'INCONCLUSIVE'
        elif all(result.get(k) for k in ('first_frame', 'native_stop', 'restored', 'capture_complete')) and result['execution_status'] == 'PASS':
            result['status'] = 'PASS'
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--execute', required=True)
    args = parser.parse_args()
    require(os.geteuid() == 0, 'root required')
    config = json.loads(args.manifest.read_text())
    require(args.execute == config['run_id'], 'run ID mismatch')
    require(config.get('publisher_binary') == 'publish-once', 'publication-only binary required')
    root = args.manifest.resolve().parent
    with (root/'attempt.json').open('x') as output:
        json.dump({'run_id': args.execute}, output)
    io = MarkerSession(root, args.execute, config)
    handshake(io)
    require(io.deadline-time.monotonic() <= 14, 'window exceeds marker budget')
    result = execute(io)
    result.update(io.identity)
    io.save('result.json', result); io.log(event='result', **result)
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())

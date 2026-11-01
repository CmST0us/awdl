"""Shared iPad test destinations, process cleanup and collector shutdown."""
import json
import os
from pathlib import Path
import plistlib
import signal
import subprocess
import time
from app_identity import APP_BUNDLE, RUNNER_BUNDLE
from device_processes import owned_processes


def destination_spec(template, destination, run_id):
    doc=plistlib.loads(Path(template).read_bytes())
    target=doc['AwareLabUITests']
    for key in ('TestBundlePath','TestHostPath','UITargetAppPath'):target.pop(key,None)
    target.update(UseDestinationArtifacts=True,TestHostBundleIdentifier=RUNNER_BUNDLE,
                  TestBundleDestinationRelativePath='__TESTHOST__/PlugIns/AwareLabUITests.xctest',UITargetAppBundleIdentifier=APP_BUNDLE)
    target.setdefault('EnvironmentVariables',{}).update(AWARE_BATCH_ID=run_id)
    doc={k:v for k,v in doc.items() if k in ('AwareLabUITests','__xctestrun_metadata__')}
    Path(destination).write_bytes(plistlib.dumps(doc))


def cleanup_ipad(udid, out, deadline, record):
    """Terminate only this project's App/runner and confirm absence."""
    def run(label, args):
        remaining=deadline-time.monotonic()
        if remaining<=0:raise TimeoutError('iPad cleanup budget exhausted')
        argv=['xcrun','devicectl','device',*args,'--device',udid]
        record('cleanup-command',name=label,argv=argv)
        with (out/(label+'.log')).open('w') as log:
            subprocess.run(argv,stdout=log,stderr=subprocess.STDOUT,timeout=min(3,remaining),check=True)
    def processes(label):
        path=out/(label+'.json')
        run(label,['info','processes','--search','AwareLab','--json-output',str(path)])
        rows=json.loads(path.read_text())['result']['runningProcesses']
        return owned_processes(rows, apps)
    try:
        app_path=out/'cleanup-apps.json'
        run('cleanup-apps',['info','apps','--search',APP_BUNDLE,'--json-output',str(app_path)])
        apps=json.loads(app_path.read_text())['result']['apps']
        active=processes('cleanup-list')
        if not active:return True
        for process in active:
            try:
                run('terminate-'+str(process['processIdentifier']),['process','terminate','--pid',str(process['processIdentifier'])])
            except subprocess.CalledProcessError as error:
                record('cleanup-terminate-result',pid=process['processIdentifier'],returncode=error.returncode)
        return not processes('cleanup-readback')
    except Exception as error:
        record('cleanup-failed',error=str(error))
        return False


def close_collector(proc, reader, deadline, record):
    """Request a record-boundary close; retain failed output on the existing limit."""
    forced = False
    try:
        proc.stdin.write('STOP\n');proc.stdin.flush()
        remaining = deadline-time.monotonic()
        if remaining <= 0:raise subprocess.TimeoutExpired('collector STOP', 0)
        proc.wait(timeout=remaining)
        reader.join(timeout=max(0, deadline-time.monotonic()))
        if reader.is_alive():raise subprocess.TimeoutExpired('collector EOF', 0)
    except (BrokenPipeError, subprocess.TimeoutExpired) as error:
        record('collector-close-failed', error=str(error))
        forced = True
        if proc.poll() is None:os.killpg(proc.pid, signal.SIGTERM)
    return forced



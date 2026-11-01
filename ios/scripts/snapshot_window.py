#!/usr/bin/env python3
"""Read one cached pairing snapshot using installed artifacts in a bounded window."""
from app_identity import APP_BUNDLE, RUNNER_BUNDLE
import argparse
import json
import os
from pathlib import Path
import plistlib
import signal
import subprocess
import time
from publisher_observation.device_processes import owned_processes

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
            subprocess.run(argv,stdout=log,stderr=subprocess.STDOUT,timeout=min(2,remaining),check=True)
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
            run('terminate-'+str(process['processIdentifier']),['process','terminate','--pid',str(process['processIdentifier'])])
        return not processes('cleanup-readback')
    except Exception as error:
        record('cleanup-failed',error=str(error))
        return False



def connection_readback(udid,out,deadline,record):
    remaining=deadline-time.monotonic()
    if remaining<=0:
        record('connection-readback-failed',error='cleanup budget exhausted')
        return False
    argv=['xcrun','devicectl','device','info','details','--device',udid,'--json-output',str(out/'connection.json')]
    record('cleanup-command',name='connection',argv=argv)
    try:
        with (out/'connection.log').open('w') as log:
            subprocess.run(argv,stdout=log,stderr=subprocess.STDOUT,timeout=min(2,remaining),check=True)
        reply=json.loads((out/'connection.json').read_text())
        return (reply['info']['outcome']=='success' and reply['result']['hardwareProperties']['udid']==udid
                and reply['result']['connectionProperties']['tunnelState']=='connected')
    except Exception as error:
        record('connection-readback-failed',error=str(error))
        return False


def capture_result(out, returncode, alive_through_cleanup, forced):
    """Inspect saved JSON records after releasing the device window."""
    path=out/'collector.log'
    valid=invalid=0
    first=last=None
    if path.exists():
        with path.open() as stream:
            for line in stream:
                try:
                    row=json.loads(line)
                    if not isinstance(row,dict) or not isinstance(row.get('message'),str) or not row.get('timestamp'):
                        raise ValueError('not a syslog record')
                    valid+=1
                    first=first or row['timestamp'];last=row['timestamp']
                except (ValueError,TypeError):invalid+=1
    usable=alive_through_cleanup and not forced and returncode in (0,-signal.SIGTERM) and valid>0 and invalid==0
    return dict(returncode=returncode,alive_through_cleanup=alive_through_cleanup,forced_kill=forced,
                valid_records=valid,invalid_records=invalid,first_timestamp=first,last_timestamp=last,
                stderr_path=str(out/'collector.stderr'),usable=usable)


def run(args):
    manifest=json.loads(args.manifest.read_text())
    if manifest.get('installed') is not True:
        raise ValueError('installed App and Runner evidence required')
    installation=json.loads((Path(manifest['installation_evidence'])/'install-1.json').read_text())
    expected_device=installation['result']['deviceIdentifier']
    udid=manifest['udid'];run_id=args.run_id
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    destination_spec(manifest['xctestrun'],out/'destination.xctestrun',run_id)
    commands=[];processes={};handles=[];error=None;test_rc=None;identity=False
    start=time.monotonic();active_end=start+args.active_seconds;end=active_end+3

    def record(event,**fields):
        with (out/'timeline.jsonl').open('a') as f:f.write(json.dumps(dict(event=event,monotonic=time.monotonic(),**fields))+'\n')

    def launch(name,argv):
        stdout=(out/(name+'.log')).open('w');stderr=(out/(name+'.stderr')).open('w');handles.extend((stdout,stderr))
        record('command',name=name,argv=argv)
        proc=subprocess.Popen(argv,stdout=stdout,stderr=stderr,start_new_session=True)
        processes[name]=proc;return proc

    try:
        launch('collector',manifest['collector_argv'])
        argv=['xcrun','devicectl','device','info','apps','--device',udid,'--search','AwareLab','--json-output',str(out/'installed.json')]
        proc=launch('installed',argv);before=time.monotonic();rc=proc.wait(timeout=min(3,active_end-time.monotonic()))
        commands.append(dict(name='installed',rc=rc,elapsed=time.monotonic()-before))
        if rc!=0:raise RuntimeError('installed app query failed')
        actual=json.loads((out/'installed.json').read_text())
        apps=actual['result']['apps']
        bundle_ids={row['bundleIdentifier'] for row in apps}
        wanted={row['bundle'] for row in manifest['signed_artifacts']}
        identity=actual['info']['outcome']=='success' and actual['result']['deviceIdentifier']==expected_device and wanted<=bundle_ids
        if not identity:raise RuntimeError('target device or installed bundle differs')
        if processes['collector'].poll() is not None:raise RuntimeError('collector exited before snapshot')
        argv=['xcodebuild','-xctestrun',str(out/'destination.xctestrun'),'-destination','id='+udid,'-resultBundlePath',str(out/'tests.xcresult'),'-collect-test-diagnostics','never','-parallel-testing-enabled','NO','-only-testing:AwareLabUITests/M5PairingTests/testPairedDeviceSnapshot','test-without-building']
        before=time.monotonic();proc=launch('snapshot',argv)
        try:test_rc=proc.wait(timeout=max(.001,active_end-time.monotonic()))
        except subprocess.TimeoutExpired:test_rc=124
        commands.append(dict(name='snapshot',rc=test_rc,elapsed=time.monotonic()-before))
    except Exception as exc:error=str(exc)
    finally:
        for name,proc in processes.items():
            if name=='collector':continue
            if proc.poll() is None:
                os.killpg(proc.pid,signal.SIGTERM)
                try:proc.wait(timeout=min(.2,max(.001,end-time.monotonic())))
                except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL)
        clean=cleanup_ipad(udid,out,end,record)
        connected=connection_readback(udid,out,end,record)
        collector=processes.get('collector')
        capture_alive=collector is not None and collector.poll() is None
        collector_forced=False
        if capture_alive:
            os.killpg(collector.pid,signal.SIGTERM)
            remaining=end-time.monotonic()
            try:
                if remaining>0:collector.wait(timeout=min(.2,remaining))
                else:
                    collector_forced=True
                    os.killpg(collector.pid,signal.SIGKILL)
            except subprocess.TimeoutExpired:
                collector_forced=True
                os.killpg(collector.pid,signal.SIGKILL)
            remaining=end-time.monotonic()
            if collector.poll() is None and remaining>0:
                try:collector.wait(timeout=min(.2,remaining))
                except subprocess.TimeoutExpired:pass
        for handle in handles:handle.close()
        device_elapsed=time.monotonic()-start
        capture=capture_result(out,collector.poll() if collector else None,capture_alive,collector_forced)
        summary=dict(run_id=run_id,elapsed_seconds=device_elapsed,active_seconds=args.active_seconds,total_budget_seconds=args.active_seconds+3,development_connection=connected,collector=capture,commands=commands,device_and_install_match=identity,snapshot_rc=test_rc,error=error,ipad_clean=clean,device_released=clean,board_operations=0,picker_requests=0,wireless_attempts=0)
        (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary))
    return 0 if test_rc==0 and clean and connected and capture['usable'] else 1


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--active-seconds',type=int,choices=(15,24),default=15)
    parser.add_argument('--manifest',type=Path,required=True)
    parser.add_argument('--run-id',required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if not args.run_id or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in args.run_id):
        parser.error('invalid run ID')
    from publisher_observation.device_lease import DeviceLease
    udid = json.loads(args.manifest.read_text())["udid"]
    with DeviceLease(udid, args.run_id):
        return run(args)

if __name__=='__main__':raise SystemExit(main())

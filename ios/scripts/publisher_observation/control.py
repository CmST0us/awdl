#!/usr/bin/env python3
"""Single owner of the bounded board/iPad discovery window. No installation."""
import argparse
import json
import os
from pathlib import Path
import plistlib
import queue
import re
import signal
import subprocess
import threading
import time
from device_processes import owned_processes
from gate import Gate, SERVICE, epoch
from driver_events import DriverTrace


def destination_spec(template, destination, run_id):
    doc=plistlib.loads(Path(template).read_bytes())
    target=doc['AwareLabUITests']
    for key in ('TestBundlePath','TestHostPath','UITargetAppPath'):target.pop(key,None)
    target.update(UseDestinationArtifacts=True,TestHostBundleIdentifier='com.eric3u.rtl8852be.AwareLabRTLUITests.xctrunner',
                  TestBundleDestinationRelativePath='__TESTHOST__/PlugIns/AwareLabUITests.xctest',UITargetAppBundleIdentifier='com.eric3u.rtl8852be.AwareLabRTL')
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
        run('cleanup-apps',['info','apps','--search','AwareLabRTL','--json-output',str(app_path)])
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


def execute(config, out, run_id, sudo_password=None):
    out.mkdir(parents=True,exist_ok=False)
    destination_spec(config['xctestrun'],out/'destination.xctestrun',run_id)
    inbox=queue.Queue();procs={};files=[];readers={}
    capture=dict(valid=0,invalid=0,completion_marker=False)
    start=time.monotonic();gate=Gate(run_id,start);driver=DriverTrace()
    stop_sent=False;publisher_launched=False;board_final=None;fresh=None;first_frame=False;go_source=None;host_started=False;budget_sent=False
    def record(event,**fields):
        with (out/'timeline.jsonl').open('a') as f:
            f.write(json.dumps(dict(event=event,run_id=run_id,experiment_id='M5-FIRST-OBS-01',mac_monotonic=time.monotonic(),**fields))+'\n')
    def spawn(name,argv):
        record('command',name=name,argv=argv)
        err=(out/(name+'.stderr')).open('w');files.append(err)
        proc=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,text=True,bufsize=1,start_new_session=True)
        procs[name]=proc
        def reader():
            with (out/(name+'.log')).open('w') as raw:
                for line in proc.stdout:
                    received=time.monotonic();raw.write(line);raw.flush()
                    if name=='collector':
                        try:
                            row=json.loads(line)
                            if row.get('event')=='collector-complete' and row.get('run_id')==run_id:capture['completion_marker']=True
                            elif row.get('event')!='collector-ready':capture['valid']+=1
                        except json.JSONDecodeError:capture['invalid']+=1
                    record('received',source=name,byte_offset=raw.tell(),received=received)
                    inbox.put((name,line,received))
            inbox.put((name,None,time.monotonic()))
        readers[name]=threading.Thread(target=reader,daemon=True)
        readers[name].start()
        return proc
    def board_word(word):
        record('board-command',word=word)
        procs['board'].stdin.write(word+'\n');procs['board'].stdin.flush()
    def stop(reason):
        nonlocal stop_sent
        if stop_sent:return
        stop_sent=True;gate.stopping=True;gate.reason=reason
        record('STOP',reason=reason)
        try:board_word('STOP')
        except (BrokenPipeError,KeyError):pass
    try:
        spawn('collector',config['collector_argv'])
        gate.collector_live=True
        spawn('board',config['board_argv'])
        if sudo_password is not None:
            procs['board'].stdin.write(sudo_password+'\n');procs['board'].stdin.flush()
            sudo_password=None
        argv=['xcodebuild','-xctestrun',str(out/'destination.xctestrun'),'-destination','id='+config['udid'],'-resultBundlePath',str(out/'tests.xcresult'),'-collect-test-diagnostics','never','-parallel-testing-enabled','NO','-only-testing:AwareLabUITests/M5PairingTests/testPublisherDiscoveryObservation','test-without-building']
        spawn('ui',argv)
        while time.monotonic()<start+28:
            now=time.monotonic()
            if not gate.stopping:
                # Both preparations share the same admission deadline.
                gate.collector_live=procs['collector'].poll() is None
                gate.ui_live='ui' not in procs or procs['ui'].poll() is None
                action=gate.decide(now)
                if action=='STOP':stop(gate.reason)
                elif gate.off and gate.search is not None and not publisher_launched:
                    board_word('PUBLISH');publisher_launched=True
                if action=='GO':
                    go_source=gate.chain.latest_source
                    record('OBS_READY',subscription=gate.chain.callback,source_watermark=go_source)
                    board_word('GO')
            cutoff=min(start+28,gate.search+5 if gate.search and not gate.stopping else start+28,start+12 if not gate.go and not gate.stopping else start+28)
            try:name,line,received=inbox.get(timeout=max(0.001,min(5,cutoff-time.monotonic())))
            except queue.Empty:continue
            if line is None:
                record('process-stream-closed',name=name)
                if name=='board':break
                if name in ('ui','collector'):stop(name+' exited')
                continue
            if name=='ui' and 'AWARE_OBSERVER ' in line:
                marker=json.loads(line.split('AWARE_OBSERVER ',1)[1])
                if gate.marker(marker,received) and not gate.stopping:
                    record('observer_search_requested',marker=marker)
            elif name=='collector':
                row=json.loads(line)
                gate.chain.feed(row)
                message=row.get('message','')
                # Source watermark excludes already-buffered earlier iPad records.
                if gate.go and row.get('filename')=='/usr/libexec/wifip2pd' and 'received a new discovery result '+config['nmi'].lower() in message.lower() and SERVICE in message:
                    if epoch(row['timestamp'])>go_source:
                        fresh=row;record('new-discovery-candidate',record=row,received=received,received_after_stop=gate.stopping)
                        stop('new-discovery-candidate')
            elif name=='board':
                if line.startswith('EVENT '):
                    if 'run_id='+run_id+' ' not in line or 'experiment_id=M5-FIRST-OBS-01 ' not in line:continue
                    if ' publication-ready ' in line:gate.host_ready=True
                    if ' publication-start-result ' in line:
                        host_started=' status=0 ' in line
                        if not host_started:stop('START failed')
                    if host_started and driver.first_frame and not first_frame:
                        first_frame=True;record('first-frame-confirmed',start=driver.start,host=line.strip())
                    if ' publication-stop-result ' in line:record('host-stop-result',line=line.strip())
                else:
                    row=json.loads(line)
                    if row.get('event')=='controller-ready' and row.get('run_id')==run_id:
                        if budget_sent or gate.stopping:stop('late or duplicate board readiness')
                        else:
                            remaining=start+28-time.monotonic()
                            board_word('BUDGET '+format(max(0,remaining),'.6f'));budget_sent=True
                    elif row.get('event')=='candidate-off' and row.get('run_id')==run_id and row.get('off') is True:
                        if row.get('nmi')!=config['nmi']:raise ValueError('OFF NMI differs from frozen input')
                        gate.off=True;record('candidate-off',evidence=row)
                    elif row.get('event')=='kmsg' and gate.go:
                        text=row.get('record','')
                        driver.feed(text)
                        if host_started and driver.first_frame and not first_frame:
                            first_frame=True;record('first-frame-confirmed',start=driver.start,evidence=row)
                        if any(value in text for value in ('Oops:', 'Kernel panic', 'BUG:', 'firmware crash')):stop('kernel fault')
                    elif row.get('event')=='board-final':board_final=row;break
                    elif row.get('event') in ('board-error','stop-unconfirmed','restore-error','capture-error'):stop(row['event'])
    except Exception as error:
        record('controller-error',error=str(error));stop(str(error))
    finally:
        # Never kill a remote module syscall or infer STOP from killing its SSH transport.
        if not stop_sent:stop('controller completion')
        # Continue consuming remote completion after a local parser/transport error.
        while board_final is None and 'board' in procs and procs['board'].poll() is None and time.monotonic()<start+28:
            try:name,line,received=inbox.get(timeout=min(5,max(0.001,start+28-time.monotonic())))
            except queue.Empty:continue
            if name!='board':continue
            if line is None:break
            try:
                row=json.loads(line)
                if row.get('event')=='board-final':board_final=row
            except json.JSONDecodeError:pass
        for name in ('ui',):
            proc=procs.get(name)
            if proc and proc.poll() is None:os.killpg(proc.pid,signal.SIGTERM)
        ipad_clean=cleanup_ipad(config['udid'],out,start+30,record) if 'ui' in procs else True
        collector=procs.get('collector')
        capture['alive_through_cleanup']=bool(collector and collector.poll() is None and readers['collector'].is_alive())
        capture['forced_close']=False
        if collector:
            capture['forced_close']=close_collector(collector,readers['collector'],
                min(start+30,time.monotonic()+1),record)
        capture['returncode']=collector.poll() if collector else None
        capture['complete']=bool(collector and not readers['collector'].is_alive() and capture['alive_through_cleanup'] and capture['valid'] and not capture['invalid'] and capture['completion_marker'] and not capture['forced_close'] and capture['returncode']==0)
        for file in files:file.close()
        record('result',fresh_discovery_candidate=fresh is not None,first_frame=first_frame,board_final=board_final,
               status='OBSERVED' if fresh and first_frame and board_final and board_final.get('restored') and ipad_clean and capture['complete'] else 'INCONCLUSIVE',
               device_released=bool(ipad_clean and board_final and board_final.get('restored')),
               ipad_clean=ipad_clean,collector=capture,note='New-discovery source-clock association requires evidence review; OBSERVED is not PASS')
    return 0 if fresh and first_frame and board_final and board_final.get('restored') and ipad_clean and capture['complete'] else 1


def main():
    p=argparse.ArgumentParser();p.add_argument('--config',type=Path,required=True);p.add_argument('--run-id',required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--password-fd',type=int,required=True);p.add_argument('--execute',action='store_true');a=p.parse_args()
    c=json.loads(a.config.read_text())
    if not a.execute:p.error('execution flag required; prepare/review does not operate devices')
    if not re.fullmatch(r'[A-Za-z0-9_-]+',a.run_id) or c.get('run_id')!=a.run_id:
        p.error('run identity differs from prepared manifest')
    if c.get('installed') is not True or c.get('board_inputs_prepared') is not True:
        p.error('both device inputs must be prepared')
    if c.get('candidate_srcversion')!='F47818D5261A5EF89B6C3D3' or c.get('board_metadata',{}).get('candidate',{}).get('srcversion')!=c['candidate_srcversion']:
        p.error('candidate differs from approved scan input')
    if c.get('remote_root')!='/tmp/M5-FIRST-OBS-01-'+a.run_id:
        p.error('remote input directory differs from this run')
    if a.output.exists():p.error('output already exists; no retry')
    with os.fdopen(a.password_fd) as source:secret=source.readline().rstrip('\n')
    return execute(c,a.output.resolve(),a.run_id,sudo_password=secret)
if __name__=='__main__':raise SystemExit(main())

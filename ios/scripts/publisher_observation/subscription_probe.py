#!/usr/bin/env python3
"""One iPad search with simultaneous all-process and source-PID log streams."""
import argparse
import json
import os
import plistlib
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import queue
import re
import signal
import subprocess
import threading
import time

from device_control import cleanup_ipad, close_collector, destination_spec
from gate import BUNDLE, SERVICE, epoch

DAEMON='/usr/libexec/deviceaccessd'


class DaemonChain:
    def __init__(self):
        self.request=None
        self.callback=None
        self.asid=None
        self.subscribe_id=None
        self.ambiguous=False

    def feed(self, row, search_start):
        if search_start is None or row.get('filename')!=DAEMON or not row.get('timestamp'):
            return
        stamp=epoch(row['timestamp'])
        if stamp<search_start:return
        msg=row.get('message','')
        if 'DADiscovery' in msg and BUNDLE in msg and 'WF-Sn '+SERVICE in msg and ('Activate:' in msg or 'Discovery add start:' in msg):
            session=re.search(r'asID[ =]([^, )]+)',msg)
            if session:
                asid=session.group(1)
                if self.asid and asid!=self.asid:self.ambiguous=True
                self.asid=asid;self.request=row
        if self.request and stamp>=epoch(self.request['timestamp']) and '[WiFi] Subscriber started' in msg:
            match=re.search(r'SubscribeID=(\d+), Service='+re.escape(SERVICE),msg)
            if match:
                if self.subscribe_id and match.group(1)!=self.subscribe_id:self.ambiguous=True
                self.subscribe_id=match.group(1);self.callback=row

    @property
    def ready(self):
        return bool(self.request and self.callback and not self.ambiguous)


def comparison(full, targeted, comparable):
    if not comparable:return 'INCONCLUSIVE'
    if targeted.ready and not full.ready:return 'ALL_STREAM_MISSING_CHAIN'
    if full.ready and targeted.ready and (full.asid,full.subscribe_id)==(targeted.asid,targeted.subscribe_id):return 'BOTH_CHAINS_OBSERVED'
    return 'UNRESOLVED'


def close_streams(procs, readers, deadline, record):
    """Both STOP requests and EOF joins share one existing close budget."""
    forced={}
    def close(name):
        forced[name]=close_collector(procs[name],readers[name],deadline,record)
    closers=[threading.Thread(target=close,args=(name,)) for name in procs]
    for thread in closers:thread.start()
    for thread in closers:thread.join(timeout=max(0,deadline-time.monotonic()))
    return {name:forced.get(name,True) for name in procs}


def execute(config, out, pairing_pin=None):
    pairing=pairing_pin is not None
    default_activity,default_total=(24,27) if pairing else (12,15)
    activity_seconds=float(config.get('activity_seconds',default_activity))
    total_seconds=float(config.get('total_seconds',default_total))
    readiness_seconds=float(config.get('readiness_seconds',1))
    if not 0<readiness_seconds<=min(5,activity_seconds):
        raise ValueError('require 0 < readiness_seconds <= min(5, activity_seconds)')
    if not 0<activity_seconds<total_seconds<=30:
        raise ValueError('require 0 < activity_seconds < total_seconds <= 30')
    out.mkdir(parents=True,exist_ok=False)
    run_id=config['run_id']
    destination_spec(config['xctestrun'],out/'destination.xctestrun',run_id)
    if pairing:
        if not re.fullmatch(r'[0-9]{6}',pairing_pin):raise ValueError('six-digit pairing PIN required')
        spec=out/'destination.xctestrun'
        os.chmod(spec,0o600)
        doc=plistlib.loads(spec.read_bytes())
        doc['AwareLabUITests']['EnvironmentVariables']['AWARE_M5_PIN']=pairing_pin
        spec.write_bytes(plistlib.dumps(doc))
    app_run_id=run_id+('-pairing' if pairing else '-discovery')
    inbox=queue.Queue();procs={};readers={};stats={};files=[]
    chains={name:DaemonChain() for name in ('all','pid')}
    t0=time.monotonic();active_end=t0+activity_seconds;deadline=t0+total_seconds
    pairing_result=None
    search_start=None;daemon_pid=config['deviceaccessd_pid'];search=None;reason='activity deadline';error=None
    ready={'all':False,'pid':False};pending={'all':[],'pid':[]}
    rx_proc=None;rx_done=False;rx_reader=None
    auth_proc=None;auth_done=False;auth_reader=None;pin_typed=False
    timeline_lock=threading.Lock()
    def record(event,**fields):
        with timeline_lock,(out/'timeline.jsonl').open('a') as f:
            f.write(json.dumps(dict(event=event,run_id=run_id,mac_monotonic=time.monotonic(),**fields))+'\n')
    def spawn(name,argv,requested_pid=None):
        record('command',name=name,argv=argv,requested_pid=requested_pid)
        err=(out/(name+'.stderr')).open('w');files.append(err)
        proc=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,
                              text=True,bufsize=1,start_new_session=True)
        procs[name]=proc
        stats[name]=dict(valid=0,invalid=0,completion=False,requested_pid=requested_pid,source_pid_mismatch=False)
        def read():
            with (out/(name+'.log')).open('w') as raw:
                for line in proc.stdout:
                    received=time.monotonic();raw.write(line);raw.flush()
                    position=raw.tell();row=None
                    if name!='ui':
                        try:
                            row=json.loads(line)
                            if row.get('event')=='collector-complete':
                                stats[name]['completion']=row.get('run_id')==run_id and row.get('requested_pid')==requested_pid
                            elif row.get('event')!='collector-ready':
                                stats[name]['valid']+=1
                                if requested_pid!=-1 and row.get('pid')!=requested_pid:stats[name]['source_pid_mismatch']=True
                        except ValueError:stats[name]['invalid']+=1
                    record('received',source=name,byte_offset=position,received=received)
                    inbox.put((name,line,row,received,position))
            inbox.put((name,None,None,time.monotonic(),None))
        readers[name]=threading.Thread(target=read,daemon=True);readers[name].start()
    def collector_argv(pid):
        return [config['python'],config['collector'],'--udid',config['udid'],'--run-id',run_id,'--pid',str(pid)]
    def feed_chain(name,row,received,position):
        if search_start is None:
            if row.get('filename')==DAEMON:pending[name].append((row,received,position))
            return
        before=chains[name].ready
        chains[name].feed(row,search_start)
        if chains[name].ready and not before:
            record('daemon-chain-ready',source=name,received=received,byte_offset=position,
                   during_activity=received<=active_end,request=chains[name].request,callback=chains[name].callback)
    def accept(name,line,row,received,position,active):
        nonlocal search_start,daemon_pid,search,rx_done,pairing_result,auth_done,pin_typed
        if name=='auth_rx':
            auth_done=True;record('auth-capture-complete',returncode=auth_proc.poll());return
        if name=='rx':
            rx_done=True;record('rx-capture-complete',returncode=rx_proc.poll());return
        if line is None:
            if active:raise RuntimeError(name+' stream closed before cleanup')
            return
        if name=='ui':
            if 'AWARE_OBSERVER ' in line:
                try:marker=json.loads(line.split('AWARE_OBSERVER ',1)[1])
                except ValueError:
                    if active:raise
                    record('ui-tail-incomplete',byte_offset=position);return
                if marker.get('run_id')==app_run_id and marker.get('experiment_id')==run_id and marker.get('source')=='xctest.physical' and marker.get('service')==SERVICE and marker.get('event','').startswith('pairing_'):
                    record(marker['event'],marker=marker,received=received)
                    if marker['event']=='pairing_observed':pairing_result=marker
                    if marker['event']=='pairing_pin_typed':pin_typed=True
                if marker.get('run_id')==app_run_id and marker.get('experiment_id')==run_id and marker.get('event')=='observer_search_triggered' and marker.get('service')==SERVICE:
                    boundary=marker.get('search_started_epoch')
                    if marker.get('source')!='xctest.physical' or not isinstance(boundary,(int,float)) or boundary<=0:
                        raise RuntimeError('search marker lacks device search-start time')
                    search=marker;search_start=boundary
                    record('observer_search_triggered',marker=marker,received=received)
                    for source,items in pending.items():
                        for old_row,old_received,old_position in items:feed_chain(source,old_row,old_received,old_position)
                        items.clear()
            return
        if row is None:
            if active:raise RuntimeError(name+' invalid JSON')
            return
        if row.get('event')=='collector-ready':
            if row.get('run_id')!=run_id or row.get('requested_pid')!=stats[name]['requested_pid']:raise RuntimeError('collector readiness identity mismatch')
            ready[name]=True;record('source-ready',source=name,received=received,requested_pid=row['requested_pid']);return
        if row.get('event')=='collector-complete':
            if active:raise RuntimeError(name+' collector stopped during observation')
            return
        msg=row.get('message','');stamp=row.get('timestamp')
        if name=='all' and row.get('filename')==DAEMON and row.get('pid')!=daemon_pid:
            raise RuntimeError('deviceaccessd PID changed; no restart')
        feed_chain(name,row,received,position)
    record('window',activity_seconds=activity_seconds,total_seconds=total_seconds,readiness_seconds=readiness_seconds)
    try:
        spawn('all',collector_argv(-1),-1)
        spawn('pid',collector_argv(daemon_pid),daemon_pid)
        ui_argv=['xcodebuild','-xctestrun',str(out/'destination.xctestrun'),'-destination','id='+config['udid'],
                    '-resultBundlePath',str(out/'tests.xcresult'),'-collect-test-diagnostics','never',
                    '-parallel-testing-enabled','NO','-only-testing:AwareLabUITests/M5PairingTests/'+('testControlledPairing' if pairing else 'testPublisherDiscoveryObservation'),'test-without-building']
        while time.monotonic()<active_end:
            if 'ui' not in procs:
                if time.monotonic()>=t0+readiness_seconds:raise RuntimeError(f'dual stream readiness exceeded {readiness_seconds:g} seconds; UI not started')
                if all(ready.values()):spawn('ui',ui_argv)
            if any(proc.poll() is not None for proc in procs.values()):raise RuntimeError('observation process exited')
            if any(s['source_pid_mismatch'] for s in stats.values()):raise RuntimeError('source-PID stream returned another process')
            if search and chains['pid'].ready and config.get('rx_capture_argv') and rx_proc is None:
                raw=(out/'rx-frames.jsonl').open('w');os.chmod(raw.name,0o600)
                err=(out/'rx.stderr').open('w');files.extend((raw,err))
                record('rx-capture-start',argv=config['rx_capture_argv'])
                rx_proc=subprocess.Popen(config['rx_capture_argv'],stdout=raw,stderr=err,start_new_session=True)
                def wait_rx():
                    try:
                        rx_proc.wait(timeout=min(5,max(.001,active_end-time.monotonic())))
                    except subprocess.TimeoutExpired:
                        if rx_proc.poll() is None:os.killpg(rx_proc.pid,signal.SIGTERM)
                        try:rx_proc.wait(timeout=min(1,max(.001,deadline-time.monotonic())))
                        except subprocess.TimeoutExpired:pass
                    inbox.put(('rx','complete',None,time.monotonic(),None))
                rx_reader=threading.Thread(target=wait_rx,daemon=True);rx_reader.start()
            if pin_typed and config.get('auth_capture_argv') and auth_proc is None:
                raw=(out/'auth-frames.jsonl').open('w');os.chmod(raw.name,0o600)
                err=(out/'auth.stderr').open('w');files.extend((raw,err))
                record('auth-capture-start',argv=config['auth_capture_argv'])
                auth_proc=subprocess.Popen(config['auth_capture_argv'],stdout=raw,stderr=err,start_new_session=True)
                def wait_auth():
                    try:
                        auth_proc.wait(timeout=min(5,max(.001,active_end-time.monotonic())))
                    except subprocess.TimeoutExpired:
                        if auth_proc.poll() is None:os.killpg(auth_proc.pid,signal.SIGTERM)
                        try:auth_proc.wait(timeout=min(1,max(.001,deadline-time.monotonic())))
                        except subprocess.TimeoutExpired:pass
                    inbox.put(('auth_rx','complete',None,time.monotonic(),None))
                auth_reader=threading.Thread(target=wait_auth,daemon=True);auth_reader.start()
            if config.get('stop_after_auth_rx') and auth_done:
                reason='authentication capture complete';break
            if pairing and pairing_result:
                reason='pairing UI terminal';break
            if not pairing and search and all(c.ready for c in chains.values()) and (not config.get('rx_capture_argv') or rx_done):
                reason='both chains observed';break
            try:item=inbox.get(timeout=min(1,max(.001,(active_end if 'ui' in procs else t0+readiness_seconds)-time.monotonic())))
            except queue.Empty:continue
            accept(*item,active=True)
    except Exception as exc:error=str(exc);reason='observation failure';record('error',error=error)
    finally:
        if auth_proc is not None and auth_proc.poll() is None:
            os.killpg(auth_proc.pid,signal.SIGTERM)
        if auth_reader is not None:auth_reader.join(timeout=min(1,max(0,deadline-time.monotonic())))
        if rx_proc is not None and rx_proc.poll() is None:
            os.killpg(rx_proc.pid,signal.SIGTERM)
        if rx_reader is not None:rx_reader.join(timeout=min(1,max(0,deadline-time.monotonic())))
        activity_snapshot=dict(search_marker_seen=search is not None,pid_identified=daemon_pid is not None,
                               chains_ready={n:c.ready for n,c in chains.items()})
        record('activity-end',reason=reason,**activity_snapshot)
        if 'ui' in procs and procs['ui'].poll() is None:os.killpg(procs['ui'].pid,signal.SIGTERM)
        clean=cleanup_ipad(config['udid'],out,min(deadline,t0+total_seconds-1),record) if 'ui' in procs else True
        collectors={name:proc for name,proc in procs.items() if name!='ui'}
        alive={name:proc.poll() is None and readers[name].is_alive() for name,proc in collectors.items()}
        forced=close_streams(collectors,readers,min(deadline,time.monotonic()+1),record)
        while True:
            try:item=inbox.get_nowait()
            except queue.Empty:break
            try:accept(*item,active=False)
            except Exception as exc:error=error or str(exc);record('post-activity-error',error=str(exc))
        for name,proc in collectors.items():
            s=stats[name];s.update(alive_through_cleanup=alive[name],forced=forced[name],returncode=proc.poll(),eof=not readers[name].is_alive())
            s['complete']=bool(s['valid'] and not s['invalid'] and s['completion'] and s['eof'] and alive[name] and not s['forced'] and s['returncode']==0 and not s['source_pid_mismatch'])
        comparable=bool(not error and activity_snapshot['search_marker_seen'] and daemon_pid is not None and clean and len(collectors)==2 and all(stats[n]['complete'] for n in collectors))
        outcome=comparison(chains['all'],chains['pid'],comparable)
        result=dict(run_id=run_id,status=outcome,reason=reason,error=error,elapsed_seconds=time.monotonic()-t0,
                    rx_capture=dict(requested=bool(config.get('rx_capture_argv')),started=rx_proc is not None,complete=rx_done,returncode=rx_proc.poll() if rx_proc else None),
                    auth_capture=dict(requested=bool(config.get('auth_capture_argv')),started=auth_proc is not None,complete=auth_done,returncode=auth_proc.poll() if auth_proc else None),
                    source_ready=ready,ui_started='ui' in procs,activity_end_state=activity_snapshot,search_source_epoch=search_start,deviceaccessd_pid=daemon_pid,search_marker=search,
                    chains={n:dict(ready=c.ready,**c.__dict__) for n,c in chains.items()},
                    collectors={n:stats[n] for n in collectors},ipad_clean=clean,device_released=clean,
                    inherited_admission_failures=config.get('inherited_admission_failures'),
                    reached_stage='authentication capture' if auth_done and auth_proc.poll()==0 else ('system subscription comparison' if search else 'UI admission'),
                    wireless_start_attempts=None if rx_proc is not None else 0,
                    automatic_retry=False,pairing_observation=pairing_result)
        (out/'summary.json').write_text(json.dumps(result,indent=2)+'\n');record('result',**{k:v for k,v in result.items() if k!='run_id'})
        for f in files:f.close()
        if pairing:(out/'destination.xctestrun').unlink(missing_ok=True)
    rx_ok=not config.get('rx_capture_argv') or (rx_done and rx_proc is not None and rx_proc.poll()==0)
    auth_ok=auth_done and auth_proc is not None and auth_proc.poll()==0
    ui_ok=auth_ok if config.get('stop_after_auth_rx') else (not pairing or bool(pairing_result and pairing_result.get('authorized') and pairing_result.get('paired_device_ids')))
    return 0 if comparable and rx_ok and ui_ok and outcome in ('ALL_STREAM_MISSING_CHAIN','BOTH_CHAINS_OBSERVED') else 1


def main():
    p=argparse.ArgumentParser();p.add_argument('--config',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--execute',action='store_true');a=p.parse_args()
    if not a.execute:p.error('allocated device window required')
    config=json.loads(a.config.read_text())
    if not re.fullmatch(r'[A-Za-z0-9_-]+',config['run_id']) or config.get('installed') is not True:p.error('new run and installed inputs required')
    if config.get('pid_prepared') is not True or not isinstance(config.get('deviceaccessd_pid'),int):p.error('same-run current PID preparation required')
    return execute(config,a.output.resolve())


if __name__=='__main__':raise SystemExit(main())

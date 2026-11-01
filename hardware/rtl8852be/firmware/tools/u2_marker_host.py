#!/usr/bin/env python3
"""运行一次已授权的 U2 marker 窗口，SSH 上限16秒。"""
import argparse
import json
from pathlib import Path
import queue
import os
import shlex
import re
import subprocess
import threading
import time


def remaining_budget(start, now):
    return max(0, 14-(now-start))



def main():
    p=argparse.ArgumentParser();p.add_argument('--manifest',type=Path,required=True);p.add_argument('--password-fd',type=int,required=True);a=p.parse_args()
    config=json.loads(a.manifest.read_text());a.execute=config['run_id']
    if not re.fullmatch(r'[A-Za-z0-9_-]+',a.execute):raise ValueError('invalid run ID')
    with os.fdopen(os.dup(a.password_fd)) as stream:secret=stream.readline().rstrip('\n')
    if not secret:raise ValueError('empty credential FD')
    root=Path(config['evidence_root'])/'load'
    root.mkdir(parents=True,exist_ok=False)
    remote=config['remote_directory']
    session=config.get('session','igtk_session.py')
    if session != 'u2_marker_session.py':raise ValueError('unsupported board session')
    command="exec sudo -k -S -p '' -- "+shlex.join(['python3','-u',remote+'/'+session,'--manifest',remote+'/manifest.json','--execute',a.execute])
    argv=['ssh','-o','BatchMode=yes','-o','ControlMaster=no','-o','ControlPath=none','-o','ConnectTimeout=2',config['ssh_host'],command]
    result={'run_id':a.execute,'argv':argv,'budget_seconds':16,'budget_sent':False}
    inbox=queue.Queue()
    start=time.monotonic();end=start+16
    with (root/'stderr.txt').open('w') as err:
        proc=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,text=True)
        result['pid']=proc.pid
        def reader():
            with (root/'events.jsonl').open('w') as out:
                for line in proc.stdout:
                    out.write(line);out.flush();inbox.put(line)
            inbox.put(None)
        thread=threading.Thread(target=reader,daemon=True);thread.start()
        try:
            proc.stdin.write(secret+'\n');proc.stdin.flush();secret=None
            while True:
                left=end-time.monotonic()
                if left<=0:raise TimeoutError('SSH total window expired')
                line=inbox.get(timeout=left)
                if line is None:break
                try:row=json.loads(line)
                except ValueError:continue
                if row.get('event')=='controller-ready' and row.get('run_id')==a.execute and row.get('experiment_id')==config['experiment_id'] and row.get('direction_id')==config['direction_id'] and not result['budget_sent']:
                    remaining=remaining_budget(start,time.monotonic())
                    if remaining<=0:raise TimeoutError('no remote budget remaining')
                    proc.stdin.write('BUDGET '+format(remaining,'.6f')+'\n');proc.stdin.flush();proc.stdin.close()
                    result.update(budget_sent=True,remaining_sent=remaining)
            result['rc']=proc.wait(timeout=max(.001,end-time.monotonic()))
        except (TimeoutError,queue.Empty,subprocess.TimeoutExpired,BrokenPipeError) as error:
            result.update(rc=None,error=type(error).__name__,remote_state='unknown; retain device; no further module operations')
            proc.kill()
        result['elapsed_seconds']=time.monotonic()-start
    (root/'host-result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
    return 0 if result.get('rc')==0 else 1


if __name__=='__main__':raise SystemExit(main())

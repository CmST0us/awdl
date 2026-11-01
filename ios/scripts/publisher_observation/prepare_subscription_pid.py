#!/usr/bin/env python3
"""Read deviceaccessd's current PID once; no App launch or search."""
import argparse
import json
from pathlib import Path
import subprocess
import time


def main():
    p=argparse.ArgumentParser();p.add_argument('--config',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    config=json.loads(a.config.read_text());a.output.mkdir(parents=True,exist_ok=False)
    output=a.output.resolve();processes=output/'processes.json'
    argv=['xcrun','devicectl','device','info','processes','--device',config['udid'],'--search','deviceaccessd','--json-output',str(processes)]
    result=dict(run_id=config['run_id'],budget_seconds=5,argv=argv,status='FAILED',device_released=True)
    start=time.monotonic()
    config['pid_prepared']=False
    try:
        with (output/'stdout.log').open('w') as stdout,(output/'stderr.log').open('w') as stderr:
            completed=subprocess.run(argv,stdout=stdout,stderr=stderr,timeout=5,check=True)
        rows=json.loads(processes.read_text())['result']['runningProcesses']
        matching=[r for r in rows if r.get('executable','').rsplit('/',1)[-1]=='deviceaccessd']
        if len(matching)!=1:raise RuntimeError('one current deviceaccessd process required')
        pid=matching[0]['processIdentifier']
        if not isinstance(pid,int) or pid<=0:raise RuntimeError('invalid current PID')
        result.update(status='PASS',pid=pid,rc=completed.returncode)
        config.update(deviceaccessd_pid=pid,pid_prepared=True,pid_preparation_evidence=str(output))
    except Exception as error:result['error']=str(error)
    result['elapsed_seconds']=time.monotonic()-start
    (output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    a.config.write_text(json.dumps(config,indent=2)+'\n')
    print(json.dumps(result,indent=2))
    return 0 if result['status']=='PASS' else 1


if __name__=='__main__':raise SystemExit(main())

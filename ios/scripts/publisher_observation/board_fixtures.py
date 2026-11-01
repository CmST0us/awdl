"""Exercise board pipe/cleanup sequencing with local Board and publisher substitutes."""
import json
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import unittest

HERE=Path(__file__).parent.resolve()
BOARD_STUB='''
import json,time
from types import SimpleNamespace
class Uncertain(Exception):pass
class Board:
 def __init__(self,root,run_id,config):
  self.root=root;self.deadline=time.monotonic()+30;self.collector=None;self.capture_error=None;self.present=True
 def phase(self,name,seconds):pass
 def mark(self,name):
  with (self.root/'calls.jsonl').open('a') as f:f.write(json.dumps(name)+'\\n')
 def baseline(self):
  self.mark('baseline');self.candidate_src='F47818D5261A5EF89B6C3D3'
  self.baseline_state={'wireless':{'wlan0':{'address':'test-nmi'}}}
  (self.root/'kernel.jsonl').write_text('');self.collector=SimpleNamespace(poll=lambda:None)
 def forward_budget(self,seconds):
  if self.deadline-time.monotonic()<seconds+11:raise RuntimeError('budget')
 def remove(self):self.mark('remove');self.present=False
 def load_candidate(self):self.mark('load');self.present=True
 def caps(self):self.mark('caps')
 def loaded(self):return self.present
 def restore(self):self.mark('restore');self.present=True
 def readback(self):self.mark('readback')
 def close(self):self.mark('close')
'''
HOST_STUB='''
import json,sys
from pathlib import Path
root=Path(__file__).parent
run=sys.argv[sys.argv.index('--run-id')+1]
def event(name):print('EVENT 1 '+name+' run_id='+run+' experiment_id=M5-FIRST-OBS-01 direction_id=M5-FIRST-DISCOVERY',flush=True)
def marker(text):
 with (root/'kernel.jsonl').open('a') as f:f.write(json.dumps({'record':text})+'\\n')
event('publication-ready')
for line in sys.stdin:
 if line.strip()=='GO':
  marker('WA_PROBE host-start result=0 cookie=8 cleanup=0 held=0 channel=6 ns=1')
  event('publication-start-result status=0')
 elif line.strip()=='STOP':
  marker('WA_PROBE host-stop result=0 held=0 fw=0 ports=0 mgmt=0 role=0 channel=0 ns=2')
  event('publication-stop-result status=0');break
'''

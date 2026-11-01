"""Interpret the driver's existing same-cookie START/SDF and cleanup records."""
import re

class DriverTrace:
    def __init__(self):
        self.start=None
        self.submits={}
        self.completions={}
        self.stopped=False

    def feed(self, text):
        match=re.search(r'WA_PROBE (host-start|host-stop|sdf-submit|sdf-done) (.*)',text)
        if not match:return
        event,body=match.groups()
        values=dict(re.findall(r'(\w+)=(-?\d+)',body))
        values={key:int(value) for key,value in values.items()}
        if event=='host-start':self.start=values
        elif event=='host-stop':
            self.stopped=all(values.get(key)==0 for key in ('result','held','fw','ports','mgmt','role','channel'))
        elif 'cookie' in values:
            (self.submits if event=='sdf-submit' else self.completions)[values['cookie']]=values

    @property
    def first_frame(self):
        start=self.start or {}
        if start.get('result')!=0 or start.get('channel')!=6:return False
        cookie=start.get('cookie')
        submit=self.submits.get(cookie,{})
        done=self.completions.get(cookie,{})
        return submit.get('result')==0 and submit.get('len',0)>0 and submit.get('channel')==start['channel'] and done.get('len')==submit['len'] and done.get('txsts')==0

    @property
    def clean(self):
        start=self.start or {}
        return self.stopped or (start.get('result',0)!=0 and start.get('cleanup')==0 and start.get('held')==0)

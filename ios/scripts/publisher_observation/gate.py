"""One-run discovery admission and shared-deadline decisions."""
import re
from datetime import datetime
from zoneinfo import ZoneInfo

from service_contract import SERVICE
BUNDLE = 'com.eric3u.rtl8852be.AwareLabRTL'

def epoch(value):
    dt = datetime.fromisoformat(value)
    return dt.replace(tzinfo=ZoneInfo('Asia/Shanghai')).timestamp() if dt.tzinfo is None else dt.timestamp()

class Subscription:
    def __init__(self, run_id, app_run_id=None):
        self.run_id = run_id
        self.app_run_id = app_run_id or run_id+"-discovery"
        self.asid = None
        self.ambiguous = False
        self.launch = self.da = self.started = self.callback = None
        self.instance = None
        self.latest_source = None

    def feed(self, row):
        msg = row.get('message', '')
        source = row.get('filename', '')
        stamp = row.get('timestamp')
        if not stamp:
            return
        ts = epoch(stamp)
        self.latest_source = max(self.latest_source or ts, ts)
        if source.endswith('/AwareLab.app/AwareLab') and f'run_id={self.app_run_id} action=launch ' in msg:
            self.launch = row
        if self.launch is None or ts < epoch(self.launch['timestamp']):
            return
        if source == '/usr/libexec/deviceaccessd' and 'DADiscovery' in msg and BUNDLE in msg and f'WF-Sn {SERVICE}' in msg:
            if 'Activate:' in msg or 'Discovery add start:' in msg:
                session = re.search(r'asID[ =]([^, )]+)', msg)
                if session:
                    if self.asid and self.asid != session.group(1):
                        self.ambiguous = True
                    self.asid = session.group(1)
                self.da = row
        if self.da is None or ts < epoch(self.da['timestamp']):
            return
        match = re.search(rf"NANSubscribe \[(\d+)\] '{re.escape(SERVICE)}' was started", msg)
        if source == '/usr/libexec/wifip2pd' and match:
            if self.instance and self.instance != match.group(1):
                self.ambiguous = True
            self.instance = match.group(1)
            self.started = row
        if source == '/usr/libexec/deviceaccessd' and self.started and ts >= epoch(self.started['timestamp']):
            if '[WiFi] Subscriber started' in msg and f'SubscribeID={self.instance}, Service={SERVICE}' in msg:
                self.callback = row

    @property
    def ready(self):
        return not self.ambiguous and all(x is not None for x in (self.launch, self.da, self.started, self.callback))

class Gate:
    def __init__(self, run_id, start):
        self.run_id = run_id
        self.start = start
        self.search = None
        self.off = self.host_ready = self.collector_live = self.ui_live = False
        self.go = self.stopping = False
        self.reason = None
        self.chain = Subscription(run_id)

    def marker(self, row, now):
        if row.get('event') == 'observer_search_requested' and row.get('run_id') == self.run_id+'-discovery' and row.get('experiment_id') == self.run_id and row.get('service') == SERVICE and self.search is None:
            self.search = now
            return True
        return False

    def decide(self, now):
        if self.stopping:
            return None
        unhealthy = not self.collector_live or not self.ui_live
        expired = (self.search is not None and now >= self.search+5) or now >= self.start+12 and not self.go
        if unhealthy or expired:
            self.reason = 'source-exited' if unhealthy else 'deadline'
            self.stopping = True
            return 'STOP'
        if not self.go and self.search is not None and self.off and self.host_ready and self.chain.ready:
            # Driver START scope (2 s) plus first-frame completion (1 s), within the shared window.
            if self.search+5-now < 3 or self.start+30-now < 16:
                self.reason = 'insufficient-budget'
                self.stopping = True
                return 'STOP'
            self.go = True
            return 'GO'
        return None

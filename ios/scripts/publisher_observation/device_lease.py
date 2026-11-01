"""Shared iPhone/iPad ownership for cooperating Wi-Fi Aware test entrypoints."""
import fcntl
import json
import os
from pathlib import Path


class DeviceLease:
    def __init__(self, udid, run_id, directory=None):
        root = Path(directory) if directory else Path.home() / 'Library/Caches/wifi-aware-device-leases'
        root.mkdir(parents=True, exist_ok=True)
        self.file = (root / (udid + '.lock')).open('a+')
        try:
            fcntl.flock(self.file, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            self.file.seek(0)
            owner = self.file.read()
            self.file.close()
            raise RuntimeError('Device is already leased: ' + owner)
        self.file.seek(0)
        self.file.truncate()
        json.dump({'pid': os.getpid(), 'run_id': run_id}, self.file)
        self.file.flush()

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.file.close()

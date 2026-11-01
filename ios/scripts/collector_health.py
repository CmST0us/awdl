#!/usr/bin/env python3
"""Bounded health check for the device syslog collector.

The collector may need several seconds before it writes its first record.
Waiting for a fixed delay misclassifies slow starts as failures, so poll until
the process is alive and the log file is non-empty, or until a deadline.
"""
from __future__ import annotations

import subprocess
import time
from pathlib import Path

DEFAULT_TIMEOUT_SECONDS = 7.0
DEFAULT_INTERVAL_SECONDS = 0.5


def wait_for_collector(process, log_path, timeout=DEFAULT_TIMEOUT_SECONDS,
                       interval=DEFAULT_INTERVAL_SECONDS):
    """Return health details for a running collector.

    Healthy means the process is still running and the log file holds at least
    one byte. A process exit or the timeout always returns healthy=False with
    the last observed size; callers keep their original BLOCKED semantics.
    """
    log_path = Path(log_path)
    started = time.monotonic()
    size = 0
    while True:
        try:
            size = log_path.stat().st_size
        except OSError:
            size = 0
        elapsed = time.monotonic() - started
        exited = process.poll() is not None
        if exited or size > 0 or elapsed >= timeout:
            return {
                "healthy": size > 0 and not exited,
                "waitSeconds": round(elapsed, 3),
                "finalSizeBytes": size,
                "processExited": exited,
                "timeoutSeconds": timeout,
            }
        remaining = max(timeout - elapsed, 0.01)
        try:
            process.wait(timeout=min(interval, remaining))
        except subprocess.TimeoutExpired:
            pass

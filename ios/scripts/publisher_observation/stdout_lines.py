"""Serialize controller, capture and publisher records on the shared SSH stream."""
import sys
import threading

_lock=threading.Lock()

def print_line(message, *, end='\n', flush=False):
    with _lock:
        sys.stdout.write(str(message)+end)
        if flush:sys.stdout.flush()

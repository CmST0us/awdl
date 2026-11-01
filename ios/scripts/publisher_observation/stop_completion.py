"""Join HOST completion and delivered kernel cleanup within one STOP deadline."""
import queue
import time


def wait_for_stop(host, inbox, board, trace, attempted, host_stop, host_eof,
                  deadline, record_host, record_exit):
    while time.monotonic()<deadline:
        if board.capture_error or board.collector.poll() is not None:
            return False,host_stop,host_eof
        if host.poll() is not None and host_stop and (not attempted or trace.clean):
            return True,host_stop,host_eof
        try:kind,value=inbox.get(timeout=max(0.001,deadline-time.monotonic()))
        except queue.Empty:break
        if kind=='capture':
            if value['event'] in ('capture-error','capture-complete'):
                return False,host_stop,host_eof
            if attempted:trace.feed(value.get('record',''))
        elif kind=='host':
            if value is None:
                host_eof=True
                host.wait(timeout=max(0.001,min(0.2,deadline-time.monotonic())))
                record_exit(host.returncode)
            else:
                record_host(value)
                host_stop=host_stop or ' publication-stop-result status=0 ' in value
    return False,host_stop,host_eof

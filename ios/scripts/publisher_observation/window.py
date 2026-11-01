"""Transfer the remaining window without comparing different monotonic clocks."""
def remote_deadline(ready_at, remaining, received_at):
    if not 0 < remaining <= 28:
        raise ValueError('invalid remaining window')
    deadline=ready_at+remaining
    if deadline<=received_at:
        raise TimeoutError('remaining window expired in transit')
    return deadline

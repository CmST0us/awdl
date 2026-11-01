#!/usr/bin/env python3
"""Decode the U2 diagnostic writes to register 0xc604 from kernel JSONL."""
import argparse
import json
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernel', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    pattern = re.compile(r'func: (241|242|243|244), line: (-?\d+), ret = (-?\d+)')
    widths = {241: 8, 242: 16, 244: 32}
    events = []
    parents = []
    for line in args.kernel.read_text().splitlines():
        row = json.loads(line)
        match = pattern.search(row.get('record', ''))
        if not match:
            continue
        marker, caller, value = map(int, match.groups())
        if marker == 243:
            parents.append(dict(monotonic_ns=row.get('monotonic_ns'),
                                caller=hex(caller & 0xffffffff), value=hex(value & 0xffffffff)))
            continue
        events.append(dict(monotonic_ns=row.get('monotonic_ns'), width=widths[marker],
                           caller=hex(caller & 0xffffffff), value=hex(value & 0xffffffff)))
    periods = []
    opened = None
    for event in events:
        timestamp = event['monotonic_ns']
        if timestamp is None:
            continue
        if int(event['value'], 16) & 3 == 3:
            if opened is None:
                opened = timestamp
        elif opened is not None:
            periods.append(dict(start_ns=opened, end_ns=timestamp,
                                seconds=(timestamp-opened)/1e9))
            opened = None
    result = dict(register='0xc604', writes=len(events), events=events, parent_events=parents,
                  mode3_intervals=periods, open_mode3_start_ns=opened)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(dict(writes=len(events), callers=sorted({e['caller'] for e in events}))))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Decode U2 native MACID gate and channel-call diagnostics from kernel JSONL."""
import argparse
import json
from pathlib import Path
import re


def decode_records(rows):
    events = []
    pattern = re.compile(r"func: (245|246|249), line: (-?\d+), ret = (-?\d+)")
    for row in rows:
        match = pattern.search(row.get("record", ""))
        if not match:
            continue
        marker, value, tsf = (int(part) for part in match.groups())
        value &= 0xffffffff
        tsf &= 0xffffffff
        event = dict(tsf_low=tsf, phase_us=tsf % 524288,
                     monotonic_ns=row.get("monotonic_ns"))
        header = row.get("record", "").split(';', 1)[0].split(',')
        if len(header) >= 3 and header[2].isdigit():
            event['kernel_us'] = int(header[2])
        if marker in (246, 249):
            event.update(kind="gate", macid=value & 255, reason=(35 + ((value >> 12) & 1)) if marker == 249 else value >> 16,
                         requested=((value >> 9) & 1) | (((value >> 8) & 1) << 1),
                         actual=(value >> 10) & 3)
            if marker == 249:
                event["sequence"] = value >> 13
        else:
            event.update(kind="channel", primary=value & 255,
                         center=(value >> 8) & 255, width=(value >> 16) & 255,
                         status=value >> 24)
            event["valid_tuple"] = event["width"] <= 2 and 0 < event["center"] <= 177 and event["primary"] <= 177
        events.append(event)
    return events


def decode(path):
    return decode_records(json.loads(line) for line in path.read_text().splitlines())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    events = decode(args.kernel)
    gates = [event for event in events if event["kind"] == "gate"]
    channels = [event for event in events if event["kind"] == "channel"]
    summary = dict(gates=len(gates), gate_mismatches=sum(
        event["requested"] != event["actual"] for event in gates),
        channel_calls=len(channels), invalid_channel_tuples=sum(
            not event["valid_tuple"] for event in channels))
    args.output.write_text(json.dumps(dict(summary=summary, events=events), indent=2)+"\n")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()

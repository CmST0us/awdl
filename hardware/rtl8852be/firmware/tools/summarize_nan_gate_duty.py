#!/usr/bin/env python3
"""Measure native gate readback intervals within each active NDI lifecycle."""
import argparse
import json
from pathlib import Path
import re
from read_nan_schedule_trace import decode, decode_records


def merge_replies(gates, reply_events):
    merged = []
    for before, after in zip(gates, gates[1:]):
        merged.append(before)
        span = (after['tsf_low']-before['tsf_low']) & 0xffffffff
        if span >= 0x80000000:
            raise ValueError('TSF discontinuity; do not merge across reset')
        found = {}
        for event in reply_events:
            delta = (event['tsf_low']-before['tsf_low']) & 0xffffffff
            if event['macid'] == before['macid'] and 0 < delta < span:
                key = (delta, event['actual'], event['requested'], event['reason'], event.get('sequence', -1))
                found[key] = dict(event, kernel_us=before['kernel_us'] +
                                  (after['kernel_us']-before['kernel_us'])*delta/span,
                                  kernel_time_estimated=True)
        merged.extend(found[key] for key in sorted(found))
    if gates:
        merged.append(gates[-1])
    return merged


def continuity(before, after, events):
    """Require every global gate sequence between two MACID observations."""
    if 'sequence' not in before or 'sequence' not in after:
        return 'UNSEQUENCED'
    mask = (1 << 19)-1
    count = (after['sequence']-before['sequence']) & mask
    if not 0 < count < (1 << 18):
        return 'NONMONOTONIC_SEQUENCE'
    span = (after['tsf_low']-before['tsf_low']) & 0xffffffff
    seen = {(event['sequence']-before['sequence']) & mask for event in events
            if event.get('kind') == 'gate' and 'sequence' in event
            and ((event['tsf_low']-before['tsf_low']) & 0xffffffff) <= span
            and 0 < ((event['sequence']-before['sequence']) & mask) <= count}
    return 'COMPLETE' if len(seen) == count else 'MISSING_EVENTS'


def summarize(path, reads=()):
    lifecycles = []
    snapshots = []
    active = None
    for line in path.read_text().splitlines():
        record = json.loads(line).get('record', '')
        if ';' not in record:
            continue
        header, message = record.split(';', 1)
        now = int(header.split(',')[2])
        snapshot = re.search(r'WA_NAN_GATE macid=(\d+) sleep=([0-9a-fA-F]+) pause=([0-9a-fA-F]+)', message)
        if snapshot:
            macid = int(snapshot[1])
            actual = ((int(snapshot[2], 16) >> (macid % 32)) & 1) | (((int(snapshot[3], 16) >> (macid % 32)) & 1) << 1)
            snapshots.append(dict(kernel_us=now, macid=macid, actual=actual))
        ready = re.search(r'WA_PROBE ndi-ready .*macid=(\d+)', message)
        if ready:
            active = dict(begin_us=now, macid=int(ready[1]))
        elif 'WA_PROBE ndi-removed ' in message and active is not None:
            lifecycles.append(dict(active, end_us=now))
            active = None
    reply_events = []
    for read in reads:
        for line in read.read_text().splitlines():
            row = json.loads(line)
            for raw in row.get('raw', '').splitlines():
                for event in decode_records([dict(record=raw)]):
                    if event['kind'] == 'gate':
                        reply_events.append(dict(event, source=str(read),
                                                 read_sequence=row['sequence']))
    events = decode(path)
    results = []
    for lifecycle in lifecycles:
        next_begin = min((r['begin_us'] for r in lifecycles
                          if r['begin_us'] > lifecycle['end_us']), default=float('inf'))
        candidates = [e for e in events if e['kind'] == 'gate'
                      and e['macid'] == lifecycle['macid']
                      and lifecycle['begin_us'] <= e['kernel_us'] < next_begin]
        anchors = [e for e in candidates if e['kernel_us'] <= lifecycle['end_us']]
        following = next((e for e in candidates if e['kernel_us'] > lifecycle['end_us']), None)
        if anchors and following and ((following['tsf_low']-anchors[-1]['tsf_low']) & 0xffffffff) < 0x80000000:
            anchors.append(following)
        # Replies can cover the tail of an NDI whose next kernel anchor is
        # emitted during cleanup. Merge before clipping to the lifecycle.
        gates = [e for e in merge_replies(anchors, reply_events)
                 if lifecycle['begin_us'] <= e['kernel_us'] <= lifecycle['end_us']]
        kernel_gate_count = sum(not e.get('kernel_time_estimated') for e in gates)
        intervals = []
        for before, after in zip(gates, gates[1:]):
            elapsed = (after['tsf_low']-before['tsf_low']) & 0xffffffff
            if elapsed >= 0x80000000:
                raise ValueError('TSF discontinuity; do not integrate across reset')
            intervals.append(dict(begin_kernel_us=before['kernel_us'],
                                  end_kernel_us=after['kernel_us'],
                                  duration_us=elapsed, actual=before['actual'],
                                  continuity=continuity(before, after, events+reply_events),
                                  estimated_boundary=bool(before.get('kernel_time_estimated') or after.get('kernel_time_estimated'))))
        total = sum(i['duration_us'] for i in intervals)
        complete = [i for i in intervals if i['continuity'] == 'COMPLETE']
        covered = sum(i['duration_us'] for i in complete)
        opened = sum(i['duration_us'] for i in complete if i['actual'] == 0)
        contradictions = []
        for interval in intervals:
            for sample in snapshots:
                if (sample['macid'] == lifecycle['macid']
                        and interval['begin_kernel_us'] < sample['kernel_us'] < interval['end_kernel_us']
                        and sample['actual'] != interval['actual']):
                    contradictions.append(dict(sample, inferred_actual=interval['actual'],
                                               interval_begin_us=interval['begin_kernel_us'],
                                               interval_end_us=interval['end_kernel_us']))
        results.append(dict(lifecycle, snapshot_contradictions=contradictions,
                            reply_gate_events=len(gates)-kernel_gate_count,
                            interpretation='CONTRADICTED' if contradictions else 'INCOMPLETE_SEQUENCE' if covered < total else 'NO_SAMPLED_CONTRADICTION', gate_events=len(gates), intervals=intervals,
                            observed_us=total, continuous_us=covered, unknown_us=total-covered,
                            sequence_coverage_percent=100*covered/total if total else None,
                            open_us=opened, closed_us=covered-opened,
                            open_percent=100*opened/covered if covered and not contradictions else None,
                            mismatches=sum(e['requested'] != e['actual'] for e in gates)))
    if not results:
        raise ValueError('no complete NDI lifecycle')
    return dict(source=str(path), lifecycles=results, limitations=[
        'Reply events are ordered by native TSF between kernel anchors; interpolated kernel times only approximate snapshot alignment.',
        'Open percentage covers only sequence-complete intervals and is withheld on snapshot contradiction; unknown intervals are excluded.',
        'Integrates readbacks between captured native DW/FAW gate calls; unobserved writers or lost events are not excluded.',
        'No extrapolation before first or after last gate event; includes active NDI setup/teardown outside file transfer.',
        'TSF is the hardware interval clock; kernel timestamps select lifecycle membership and are not exact radio event times.'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernel', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reads', type=Path, nargs='*', default=[])
    args = parser.parse_args()
    result = summarize(args.kernel, args.reads)
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps([{k: v for k, v in r.items() if k != 'intervals'} for r in result['lifecycles']], indent=2))

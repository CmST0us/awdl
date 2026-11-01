#!/usr/bin/env python3
"""Associate actual TCP receipts with the selected paired device and NDI session."""
import argparse
import json
import re
from pathlib import Path


def validate(app, board, *, run_id, experiment_id, paired_device_id, ndp_session, peer_nmi):
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,100}', run_id):
        raise ValueError('invalid run ID')
    if paired_device_id <= 0 or not ndp_session or not peer_nmi:
        raise ValueError('paired device and board session mapping required')
    app = [e for e in app if e.get('runID') == run_id and e.get('directionID') == 'M5-NAN-DATA']
    board = [e for e in board if e.get('runID') == run_id and e.get('directionID') == 'M5-NAN-DATA']
    def one(events, action, source):
        matches = [e for e in events if e.get('action') == action]
        if len(matches) != 1:
            raise ValueError(f'exactly one {action} required')
        e = matches[0]
        if e.get('source') != source or e.get('experimentID') != experiment_id:
            raise ValueError(f'{action} source/experiment mismatch')
        if not isinstance(e.get('monotonicNS'), int) or not e.get('wallTime'):
            raise ValueError(f'{action} clock missing')
        return e
    path = one(app, 'native_data_native_path_ready', 'ipad.app')
    ack = one(app, 'native_data_ack_received', 'ipad.app')
    hello = one(board, 'native_data_hello_received', 'board.first_packet')
    def fields(event):
        return dict(part.strip().split('=', 1) for part in event.get('detail', '').split(';') if '=' in part)
    p, a = fields(path), fields(ack)
    if p.get('pairedDeviceID') != str(paired_device_id) or a.get('pairedDeviceID') != str(paired_device_id):
        raise ValueError('Wi-Fi Aware paired ID mismatch')
    if not p.get('local') or not p.get('remote') or p.get('local') == 'nil' or p.get('remote') == 'nil':
        raise ValueError('native path endpoints missing')
    if ack['monotonicNS'] < path['monotonicNS']:
        raise ValueError('ACK precedes native path')
    if a.get('messageID') != '1' or a.get('bytes') != str(len(f'ACK {run_id} 1\n')):
        raise ValueError('ACK identity/byte count mismatch')
    if hello.get('messageID') != '1' or hello.get('bytes') != len(f'HELLO {run_id} 1\n'):
        raise ValueError('HELLO identity/byte count mismatch')
    if hello.get('ndpSession') != ndp_session or hello.get('peerNMI') != peer_nmi:
        raise ValueError('board NDP/peer mapping mismatch')
    if not hello.get('interface') or not hello.get('localIPv6') or not hello.get('peerIPv6'):
        raise ValueError('board interface/endpoints missing')
    if any(e.get('action') in ('native_data_failed', 'native_data_cancelled') for e in app):
        raise ValueError('app run contains failure/cancellation')
    return {'runID': run_id, 'experimentID': experiment_id, 'messageID': '1',
            'pairedDeviceID': paired_device_id, 'ndpSession': ndp_session, 'peerNMI': peer_nmi,
            'helloBytes': hello['bytes'], 'ackBytes': int(a['bytes']),
            'status': 'PASS', 'scope': 'correlated application receipts',
            'securityAndNDIProofRequired': True}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--app-events', type=Path, required=True)
    p.add_argument('--board-events', type=Path, required=True)
    p.add_argument('--run-id', required=True)
    p.add_argument('--experiment-id', required=True)
    p.add_argument('--paired-device-id', type=int, required=True)
    p.add_argument('--ndp-session', required=True)
    p.add_argument('--peer-nmi', required=True)
    a = p.parse_args()
    try:
        read = lambda path: [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
        result = validate(read(a.app_events), read(a.board_events), run_id=a.run_id,
                          experiment_id=a.experiment_id, paired_device_id=a.paired_device_id,
                          ndp_session=a.ndp_session, peer_nmi=a.peer_nmi)
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(json.dumps({'status': 'FAIL', 'error': str(error)}))
        return 1
    print(json.dumps(result))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

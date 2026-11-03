#!/usr/bin/env python3
"""在真机开启 AirDrop、触发板端发送并自动接受指定文件。"""
import argparse
from datetime import datetime
import json
from pathlib import Path
import plistlib
import queue
import shlex
import signal
import subprocess
import threading
import time
import tomllib

from publisher_observation.device_lease import DeviceLease

ROOT = Path(__file__).resolve().parents[2]


def stop(process):
    if process is not None and process.poll() is None:
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=ROOT / 'config/local/airdrop-uitest.toml')
    parser.add_argument('--file', help='覆盖配置中的目标板文件绝对路径')
    args = parser.parse_args()
    cfg = tomllib.loads(args.config.read_text())
    remote_file = args.file or cfg['remote_file']
    for name in ('step', 'request', 'build', 'runner', 'transfer'):
        value = cfg[f'{name}_timeout_seconds']
        if not isinstance(value, int) or value <= 0:
            parser.error(f'{name}_timeout_seconds 必须是正整数秒')
    if not remote_file.startswith('/') or not cfg['remote_lease'].startswith('/'):
        parser.error('remote_file 与 remote_lease 必须是目标板绝对路径')
    run_id = datetime.now().strftime('airdrop-ui-%Y%m%d-%H%M%S')
    out = ROOT / '.private/artifacts' / run_id
    out.mkdir(parents=True)
    derived = ROOT / '.build/ios-lab'
    ssh = ['ssh', '-T', '-o', 'BatchMode=yes']
    for option in cfg.get('ssh_options', []):
        ssh += ['-o', option]
    ssh.append(cfg['ssh_target'])
    events = queue.Queue()
    processes = {}
    readers = []
    ui_events = []
    transfer_events = []
    errors = []

    def start(name, command):
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, bufsize=1)
        processes[name] = process

        def read():
            with (out / f'{name}.log').open('w') as log:
                for line in process.stdout:
                    log.write(line)
                    log.flush()
                    events.put((name, line))
            events.put((name, None))

        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        readers.append(reader)
        return process

    result = {'device_udid': cfg['device_udid'], 'file': remote_file, 'ui': ui_events,
              'transfer': transfer_events, 'errors': errors}
    injected = None
    with DeviceLease(cfg['device_udid'], run_id):
        try:
            build = ['xcodebuild', '-project', str(ROOT / 'ios/AwareLab.xcodeproj'),
                     '-scheme', 'AwareLab', '-configuration', 'Debug',
                     '-destination', 'id=' + cfg['device_udid'], '-derivedDataPath', str(derived),
                     'DEVELOPMENT_TEAM=' + cfg['development_team'], 'build-for-testing']
            with (out / 'build.log').open('w') as log:
                process = subprocess.Popen(build, stdout=log, stderr=subprocess.STDOUT)
                processes['build'] = process
                if process.wait(timeout=cfg['build_timeout_seconds']) != 0:
                    raise RuntimeError('真机测试构建失败，见 build.log')
            products = derived / 'Build/Products'
            template = max(products.glob('AwareLab_*.xctestrun'), key=lambda p: p.stat().st_mtime)
            spec = plistlib.loads(template.read_bytes())
            spec['AwareLabUITests']['EnvironmentVariables'].update({
                'AIRDROP_SENDER_NAME': cfg['sender_name'],
                'AIRDROP_FILENAME': Path(remote_file).name,
                'AIRDROP_STEP_TIMEOUT': str(cfg['step_timeout_seconds']),
                'AIRDROP_REQUEST_TIMEOUT': str(cfg['request_timeout_seconds']),
            })
            injected = products / (run_id + '.xctestrun')
            injected.write_bytes(plistlib.dumps(spec))
            test = start('ui', ['xcodebuild', '-xctestrun', str(injected), '-destination',
                'id=' + cfg['device_udid'], '-derivedDataPath', str(derived), '-resultBundlePath',
                str(out / 'ui.xcresult'), '-parallel-testing-enabled', 'NO',
                '-only-testing:AwareLabUITests/AirDropTests/testReceiveFile', 'test-without-building'])
            ui_deadline = time.monotonic() + cfg['runner_timeout_seconds']
            transfer_deadline = None
            ended = set()
            while True:
                now = time.monotonic()
                if test.poll() is None and now > ui_deadline:
                    raise TimeoutError('真机测试超过 runner_timeout_seconds')
                sender = processes.get('sender')
                if sender is not None and sender.poll() is None and now > transfer_deadline:
                    raise TimeoutError('板端发送超过 transfer_timeout_seconds')
                if sender is not None and 'sender' in ended and sender.poll() not in (None, 0):
                    raise RuntimeError('板端发送失败，见 sender.log')
                if 'ui' in ended and test.poll() is not None:
                    if test.returncode != 0:
                        raise RuntimeError('系统界面自动化失败，见 ui.log 和 ui.xcresult')
                    if sender is None:
                        raise RuntimeError('测试结束前未确认 AirDrop 已开启')
                    if 'sender' in ended and sender.poll() is not None:
                        break
                try:
                    source, line = events.get(timeout=0.5)
                except queue.Empty:
                    continue
                if line is None:
                    ended.add(source)
                    continue
                if source == 'ui' and 'AIRDROP_UI ' in line:
                    event = json.loads(line.split('AIRDROP_UI ', 1)[1])
                    ui_events.append(event)
                    print(json.dumps(event, ensure_ascii=False), flush=True)
                    if event['event'] == 'ready' and sender is None:
                        command = ['flock', '-n', '-E', '75', cfg['remote_lease'],
                                   'airdrop', 'send', cfg['receiver_name'], remote_file]
                        start('sender', ssh + [shlex.join(command)])
                        transfer_deadline = time.monotonic() + cfg['transfer_timeout_seconds']
                elif source == 'sender' and line.startswith('{'):
                    event = json.loads(line)
                    transfer_events.append(event)
                    print(json.dumps(event, ensure_ascii=False), flush=True)
            if sender.returncode != 0 or not any(e.get('event') == 'delivered' for e in transfer_events):
                raise RuntimeError('板端未确认文件交付，见 sender.log')
            if not any(e['event'] == 'accepted' for e in ui_events):
                raise RuntimeError('UI 测试未确认点击接受')
            result['ok'] = True
        except (Exception, KeyboardInterrupt) as error:
            errors.append(str(error) or '操作已取消')
            result['ok'] = False
        finally:
            sender = processes.get('sender')
            if sender is not None and sender.poll() is None:
                try:
                    with (out / 'stop.log').open('w') as log:
                        rc = subprocess.run(ssh + ['airdrop stop'], stdout=log,
                                            stderr=subprocess.STDOUT, timeout=10).returncode
                    if rc != 0:
                        errors.append('板端取消未成功，需确认 AirDrop 资源状态')
                except subprocess.TimeoutExpired:
                    errors.append('板端取消超时，需确认 AirDrop 资源状态')
            for process in processes.values():
                stop(process)
            for reader in readers:
                reader.join(timeout=5)
            if injected is not None:
                injected.unlink(missing_ok=True)
            result['exit_codes'] = {name: process.returncode for name, process in processes.items()}
            (out / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2))
    print(f'结果：{out / "result.json"}', flush=True)
    return 0 if result['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

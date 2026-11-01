#!/usr/bin/env python3
"""Physical-device tests, bounded subprocesses, USB records, and raw evidence."""
import argparse, datetime, json, os, pathlib, re, shutil, signal, subprocess, time, plistlib, sys
ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from collector_health import DEFAULT_TIMEOUT_SECONDS, wait_for_collector
from pin_safety import redact_pin, xcresult_contains
from service_contract import SERVICE
p = argparse.ArgumentParser()
p.add_argument('--run-id', required=True)
p.add_argument('--profile', choices=('g1','m4','m4gate','m5'), default='g1')
p.add_argument('--only', default=None)
p.add_argument('--target-name', default=None)
p.add_argument('--target-timeout', type=int, default=None, help='per-transition completion fallback, capped at 30 s')
p.add_argument('--timeout', type=int, default=30, help='subprocess completion fallback, capped at 30 s')
p.add_argument('--repeat', type=int, choices=range(1,11), default=1)
p.add_argument('--approval', default=os.environ.get('AWARE_LONG_WAIT_APPROVED', ''), help='user approval source for multi-round observation')
p.add_argument('--gate-ttl', action='store_true', help='m4gate single-cycle TTL observation, capped at 30 s')
p.add_argument('--pin', default=None, help='6-digit board PIN for --profile m5 Stage A; never written to evidence')
p.add_argument('--skip-collector', action='store_true', help='continue when the system log collector stays silent; results are marked INCONCLUSIVE')
a = p.parse_args()
approval = a.approval.strip()
if a.gate_ttl and a.profile != 'm4gate':
    p.error('--gate-ttl is only used by --profile m4gate')
if a.gate_ttl and a.repeat != 1:
    p.error('--gate-ttl measures one cycle; use --repeat 1')
if a.pin is not None and not re.fullmatch(r'[0-9]{6}', a.pin):
    p.error('--pin must be exactly 6 digits')
if a.pin is not None and a.profile != 'm5':
    p.error('--pin is only used by --profile m5')
if a.profile == 'm5' and a.pin and not a.only:
    a.only = 'AwareLabUITests/M5PairingTests/testStageAPairing'
a.only = a.only or {'m4gate':'AwareLabUITests/M4DiscoveryTests/testGateObservation','m4':'AwareLabUITests/M4DiscoveryTests/testTargetDiscovery','m5':'AwareLabUITests/M5PairingTests/testAccessoryPickerObservation'}.get(a.profile, 'AwareLabUITests/DiscoveryTests/testDiscoveryProbe')
a.target_name = a.target_name or ('ROCK5B-M5' if a.profile == 'm5' else 'ROCK5B-M4')
stage_a = a.profile == 'm5' and 'testStageAPairing' in a.only
if stage_a and a.pin is None:
    p.error('--pin is required for the M5 Stage A pairing test')
if stage_a and a.repeat != 1:
    p.error('Stage A runs one cold pairing per invocation; do not use --repeat')
if a.pin is not None and not stage_a:
    p.error('--pin is only used by the M5 Stage A pairing test')
if not a.run_id or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in a.run_id):
    p.error('invalid run id')
if a.target_timeout is None:
    a.target_timeout = 5
max_target_timeout = 5 if a.profile == 'm5' else 30
if not 5 <= a.target_timeout <= max_target_timeout:
    p.error(f'target timeout must be within 5..{max_target_timeout} seconds')
if a.profile == 'm5' and a.target_timeout > 5:
    p.error('M5 target wait is capped at 5 s; longer needs a measured basis and approval')
if stage_a and a.timeout > 30:
    p.error('M5 Stage A overall fallback must be <=30 s')
if not 1 <= a.timeout <= 30:
    p.error('timeout must be within 1..30 seconds')
if a.repeat > 1 and not approval:
    p.error('multi-round observation exceeds 30 s; approval required')
config = {}
for line in pathlib.Path(os.environ.get('AWARE_LOCAL_ENV', ROOT/'.private/ios/local.env')).read_text().splitlines():
    if line.strip() and not line.startswith('#'):
        k, v = line.split('=', 1); config[k] = v.strip().strip('"').strip("'")
udid = config['AWARE_DEVICE_UDID']
derived_data = pathlib.Path(config.get('AWARE_DERIVED_DATA', ROOT/'.private/ios/DerivedData')).resolve()
from publisher_observation.device_lease import DeviceLease
lease = DeviceLease(udid, a.run_id)
out = ROOT/'.private/artifacts'/a.run_id
out.mkdir(parents=True, exist_ok=False)
manifest = {'run_id': a.run_id, 'source': 'mac.orchestrator', 'started_at': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'monotonic_ns': time.monotonic_ns(), 'only': a.only, 'physical_device': True, 'wireless_verified': False, 'profile': a.profile}
manifest['repeat'] = a.repeat
manifest['pin_injected'] = a.pin is not None
manifest['stage_a'] = stage_a
if approval:
    manifest['long_wait_approval'] = approval
manifest['pin_safety'] = {'injected': a.pin is not None, 'redaction_files': 0, 'xcresult_removed': False, 'failure_screenshot_withheld': stage_a}
if a.profile in ('m4', 'm4gate', 'm5'):
    manifest['target_name'] = a.target_name
    manifest['target_timeout'] = a.target_timeout
manifest['xcode_version'] = subprocess.check_output(['xcodebuild','-version'],text=True,timeout=5).strip()
manifest['clock_note'] = 'Mac and device monotonic domains are not synchronized; correlate exact runID and wallTime only'
manifest['system_log_loss'] = 'unknown; missing required run IDs fails validation'
(out/'manifest.json').write_text(json.dumps(manifest, indent=2))
def run(cmd, name, timeout=5, extra_env=None):
    remaining = (window_end if cleaning_up else active_end) - time.monotonic()
    if remaining <= 0.5:
        return 124
    timeout = min(timeout, remaining - 0.5)
    env = None
    if extra_env:
        env = dict(os.environ); env.update(extra_env)
    with (out/(name+'.log')).open('w') as log:
        proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, start_new_session=True, env=env)
        try: rc = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGTERM)
            try: proc.wait(timeout=0.25)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait(timeout=0.25)
            rc = 124
    (out/(name+'-command.json')).write_text(json.dumps({'argv':cmd,'returncode':rc},indent=2))
    return rc
def stop_collector():
    global collector_stopped
    if collector_stopped:
        return
    collector_stopped = True
    if collector.poll() is None:
        os.killpg(collector.pid, signal.SIGTERM)
        try:
            collector.wait(timeout=max(0.001, min(0.25, window_end-time.monotonic())))
        except subprocess.TimeoutExpired:
            os.killpg(collector.pid, signal.SIGKILL)
            collector.wait(timeout=max(0.001, min(0.25, window_end-time.monotonic())))


def cleanup_device():
    global cleaning_up
    if 'device_clean' in manifest:
        return
    cleaning_up = True
    try:
        from snapshot_window import cleanup_ipad
        def record(event, **fields):
            manifest.setdefault('cleanup_events', []).append(dict(event=event, **fields))
        manifest['device_clean'] = cleanup_ipad(udid, out, window_end, record)
    except Exception as exc:
        manifest['cleanup_error'] = str(exc)
        manifest['device_clean'] = False
    finally:
        cleaning_up = False


window_start = time.monotonic()
active_end = window_start + 27
window_end = window_start + 30
cleaning_up = False
log_file = (out/'system.ndjson').open('w')
log_err = (out/'system.stderr').open('w')
collector = subprocess.Popen(['uvx','--from','pymobiledevice3==11.41.2','pymobiledevice3','syslog','live','--udid',udid,'--format','json'],stdout=log_file,stderr=log_err,start_new_session=True)
passed = False
collector_stopped = False
injected = None
collector_health = {'healthy': False, 'waitSeconds': 0.0, 'finalSizeBytes': 0, 'processExited': False, 'timeoutSeconds': DEFAULT_TIMEOUT_SECONDS}
try:
    collector_health = wait_for_collector(collector, out/'system.ndjson')
    manifest['collector_health'] = collector_health
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2))
    if not collector_health['healthy'] and not a.skip_collector:
        raise RuntimeError('System log collector not healthy; no test executed')
    products = derived_data/'Build/Products'
    template = next(products.glob('AwareLab_*.xctestrun'))
    spec = plistlib.loads(template.read_bytes())
    injected_env = {'AWARE_BATCH_ID':a.run_id,'AWARE_REPEAT_COUNT':str(a.repeat),'AWARE_RUN_TIMEOUT':str(a.timeout)}
    if approval:
        injected_env['AWARE_LONG_WAIT_APPROVED'] = approval
    if a.profile in ('m4', 'm4gate'):
        injected_env.update({'AWARE_MODE':'m4','AWARE_TARGET_NAME':a.target_name,'AWARE_TARGET_TIMEOUT':str(a.target_timeout)})
    if a.profile == 'm5':
        injected_env.update({'AWARE_MODE':'m5','AWARE_TARGET_NAME':a.target_name,'AWARE_TARGET_TIMEOUT':str(a.target_timeout),'AWARE_DISCOVERY_TIMEOUT':str(min(30, a.target_timeout))})
    if a.profile == 'm4gate':
        injected_env.update({'AWARE_GATE_COUNT':str(a.repeat),'AWARE_GATE_TIMEOUT':str(a.target_timeout)})
        if a.gate_ttl:
            injected_env['AWARE_GATE_TTL'] = '1'
    for target in ['AwareLabTests', 'AwareLabUITests']:
        spec[target]['EnvironmentVariables'].update(injected_env)
    if a.pin is not None:
        spec['AwareLabUITests']['EnvironmentVariables']['AWARE_M5_PIN'] = a.pin
    # Preserve __TESTROOT__ relative paths by keeping the injected file beside products.
    injected = products/(a.run_id+'.xctestrun')
    injected.write_bytes(plistlib.dumps(spec))
    cmd = ['xcodebuild','-xctestrun',str(injected),'-destination','id='+udid,'-derivedDataPath',str(derived_data),'-resultBundlePath',str(out/'tests.xcresult'),'-collect-test-diagnostics','never','-parallel-testing-enabled','NO','-maximum-test-execution-time-allowance',str(max(30, a.timeout)),'-test-timeouts-enabled','YES','-only-testing:'+a.only,'test-without-building']
    rc = run(cmd,'test',a.timeout)
    if injected is not None:
        try:
            injected.unlink()
            manifest['pin_safety']['xctestrun_deleted'] = True
        except OSError:
            manifest['pin_safety']['xctestrun_deleted'] = False
        injected = None
    if rc and not stage_a:
        run(['xcrun','devicectl','device','capture','screenshot','--device',udid,'--destination',str(out/'failure.png')],'screenshot')
    export_rc = run(['xcrun','devicectl','device','copy','from','--device',udid,'--domain-type','appDataContainer','--domain-identifier','com.eric3u.rtl8852be.AwareLabRTL','--source','Documents/Runs','--destination',str(out/'app-runs')],'export')
    run(['xcrun','xcresulttool','export','attachments','--path',str(out/'tests.xcresult'),'--output-path',str(out/'attachments')],'attachments')
    cleanup_device()
    if a.profile == 'm5':
        stop_collector()
    if stage_a:
        redacted = redact_pin(out, a.pin)
        manifest['pin_safety']['redaction_files'] = redacted
        if xcresult_contains(out/'tests.xcresult', a.pin):
            shutil.rmtree(out/'tests.xcresult')
            manifest['pin_safety']['xcresult_removed'] = True
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2))
    valid = []
    errors = []
    m4_runs = []
    m5_runs = []
    expected_run_ids = [f'{a.run_id}-{i:02d}' for i in range(1,a.repeat+1)]
    if a.profile == 'm4gate':
        check_rc = run([sys.executable, str(ROOT/'ios/scripts/check_gate_evidence.py'), '--artifact-dir', str(out), '--run-id', a.run_id + '-gate', '--count', str(a.repeat)], 'gate-validation', 5)
        if (out/'gate-validation.json').exists():
            validation = json.loads((out/'gate-validation.json').read_text())
            if not validation.get('valid'):
                errors.extend(validation.get('errors', []))
            else:
                valid = ['gate']
        else:
            errors.append({'error':'gate validator produced no report'})
        if check_rc:
            errors.append({'error':'gate validation failed','returncode':check_rc})
    elif a.profile == 'm4':
        check_rc = run([sys.executable, str(ROOT/'ios/scripts/check_m4_evidence.py'), '--artifact-dir', str(out), '--run-id', a.run_id, '--target-name', a.target_name, '--repeat', str(a.repeat)], 'm4-validation', 5)
        validation_path = out/'m4-validation.json'
        if validation_path.exists():
            validation = json.loads(validation_path.read_text())
            errors.extend(validation['errors'])
            valid = validation['validatedAppRuns']
            m4_runs = validation['runs']
        else:
            errors.append({'error':'M4 evidence validator produced no report'})
        if check_rc:
            errors.append({'error':'M4 evidence validation failed','returncode':check_rc})
        parse_cmd = [sys.executable, str(ROOT/'ios/scripts/parse_nan_log.py'), '--input', str(out/'system.ndjson'), '--output', str(out/'nan-summary.json'), '--target-name', a.target_name, '--service', SERVICE]
        for run_id in expected_run_ids:
            parse_cmd += ['--run-id', run_id]
        parse_rc = run(parse_cmd, 'nan-summary', 30)
        if parse_rc:
            errors.append({'error':'NAN summary parser failed','returncode':parse_rc})
        (out/'m4-evidence.json').write_text(json.dumps({'schema':'awarelab.m4-evidence','schemaVersion':1,'run_id':a.run_id,'profile':'m4','target_name':a.target_name,'service':SERVICE,'runs':m4_runs,'nan_summary':'nan-summary.json','generated_at':datetime.datetime.now(datetime.timezone.utc).isoformat()},indent=2))
    elif a.profile == 'm5':
        check_rc = run([sys.executable, str(ROOT/'ios/scripts/check_m5_evidence.py'), '--artifact-dir', str(out), '--run-id', a.run_id, '--target-name', a.target_name, '--repeat', str(a.repeat)], 'm5-validation', 5, {'AWARE_M5_PIN': a.pin} if a.pin else None)
        validation_path = out/'m5-validation.json'
        validation = {}
        if validation_path.exists():
            validation = json.loads(validation_path.read_text())
            errors.extend(validation['errors'])
            valid = validation['validatedAppRuns']
            m5_runs = validation['runs']
        else:
            errors.append({'error':'M5 evidence validator produced no report'})
        if check_rc:
            errors.append({'error':'M5 evidence validation failed','returncode':check_rc})
        parse_cmd = [sys.executable, str(ROOT/'ios/scripts/parse_nan_log.py'), '--input', str(out/'system.ndjson'), '--output', str(out/'nan-summary.json'), '--target-name', a.target_name, '--service', SERVICE]
        for run_id in expected_run_ids:
            parse_cmd += ['--run-id', run_id]
        parse_rc = run(parse_cmd, 'nan-summary', 30)
        if parse_rc:
            errors.append({'error':'NAN summary parser failed','returncode':parse_rc})
        (out/'m5-evidence.json').write_text(json.dumps({'schema':'awarelab.m5-evidence','schemaVersion':1,'run_id':a.run_id,'profile':'m5','stage_a':stage_a,'pin_injected':a.pin is not None,'pin_handling':'system_picker','pin_safety':manifest['pin_safety'],'pin_leak_check':validation.get('pinLeakCheck'),'runs':m5_runs,'nan_summary':'nan-summary.json','generated_at':datetime.datetime.now(datetime.timezone.utc).isoformat()},indent=2))
    elif 'testDiscoveryProbe' in a.only:
        attachment_names = []
        attachments_path = out/'attachments'/'manifest.json'
        if attachments_path.exists():
            for group in json.loads(attachments_path.read_text()):
                attachment_names.extend(item['suggestedHumanReadableName'] for item in group['attachments'])
        def seen_run_ids(expected):
            seen = set()
            with (out/'system.ndjson').open() as system_stream:
                for line in system_stream:
                    seen.update(run_id for run_id in expected if run_id in line)
            return seen
        expected_ids = {f'{a.run_id}-{i:02d}' for i in range(1,a.repeat+1)}
        seen_ids = seen_run_ids(expected_ids)
        for index in range(1, a.repeat+1):
            run_id = f'{a.run_id}-{index:02d}'
            folder = out/'app-runs'/run_id
            try:
                events = [json.loads(line) for line in (folder/'events.jsonl').read_text().splitlines()]
                conf = json.loads((folder/'configuration.json').read_text())
                result = json.loads((folder/'result.json').read_text())
                assert conf['runID'] == run_id and conf['experimentID'] == a.run_id
                assert all(e['runID'] == run_id and e['experimentID'] == a.run_id for e in events)
                assert [e['sequence'] for e in events] == list(range(1,len(events)+1))
                assert [e['monotonicNS'] for e in events] == sorted(e['monotonicNS'] for e in events)
                assert [e['action'] for e in events] == ['launch','capability_check','discovery_requested','discovery_container_closed']
                assert events[-1]['state'] == 'cancelled' and result['state'] == 'cancelled'
                assert result['wirelessVerified'] is False and result['runID'] == run_id
                assert run_id in seen_ids
                names = attachment_names
                assert any(run_id+'-ui-result' in n for n in names)
                assert any(run_id+'-system-discovery' in n for n in names)
                valid.append(run_id)
            except (OSError, ValueError, KeyError, AssertionError, TypeError) as exc:
                errors.append({'run_id':run_id,'error':str(exc) or 'evidence assertion failed'})
    if collector.poll() is not None and not collector_stopped:
        errors.append({'error':'System collector exited before test completion'})
    passed = rc == 0 and export_rc == 0 and not errors and collector_health['healthy'] and manifest.get('device_clean', False)
    scope = 'M4 gate observation UI only' if a.profile == 'm4gate' else 'M4 discovery UI only' if a.profile == 'm4' else ('M5 Stage A pairing UI only' if stage_a else 'M5 pairing UI only') if a.profile == 'm5' else ('UI only' if 'AwareLabUITests' in a.only else 'unit tests only')
    result_payload = {'status':('INCONCLUSIVE' if not collector_health['healthy'] else 'PASS' if passed else 'FAIL'),'test_returncode':rc,'export_returncode':export_rc,'scope':scope,'profile':a.profile,'wireless_verified':False,'stage_a':stage_a,'pin_injected':a.pin is not None,'pin_safety':manifest['pin_safety'],'validated_app_runs':valid,'validation_errors':errors,'collector_health':collector_health}
    if a.profile == 'm4':
        result_payload['nan_summary'] = 'nan-summary.json'
        result_payload['m4_evidence'] = 'm4-evidence.json'
    if a.profile == 'm5':
        result_payload['nan_summary'] = 'nan-summary.json'
        result_payload['m5_evidence'] = 'm5-evidence.json'
    (out/'result.json').write_text(json.dumps(result_payload,indent=2))
except Exception as exc:
    (out/'result.json').write_text(json.dumps({'status':'BLOCKED','error':str(exc),'collector_health':collector_health,'wireless_verified':False},indent=2))
finally:
    cleaning_up = True
    # Remove the transient xctestrun first so an injected PIN does not outlive the run.
    if injected is not None:
        try:
            injected.unlink()
            manifest['pin_safety']['xctestrun_deleted'] = True
        except OSError:
            manifest['pin_safety']['xctestrun_deleted'] = False
    cleanup_device()
    if not manifest.get('device_clean', False):
        passed = False
    try:
        stop_collector()
    except Exception as exc:
        manifest['collector_stop_error'] = str(exc)
    try:
        log_file.close(); log_err.close()
    except OSError:
        pass
    try:
        manifest['window_elapsed_seconds'] = time.monotonic() - window_start
        manifest['system_log_bytes_at_exit'] = (out/'system.ndjson').stat().st_size
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2))
    except OSError:
        pass
lease.__exit__()
print(out)
sys.exit(0 if passed else 1)

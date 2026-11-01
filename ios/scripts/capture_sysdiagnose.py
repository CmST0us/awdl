#!/usr/bin/env python3
"""One explicitly selected USB request; no retries, no fallback, no deletion."""
import argparse, asyncio, datetime, hashlib, importlib.metadata, json, os, pathlib, time, traceback
from pymobiledevice3.remote.userspace_tunnel import UserspaceRsdTunnel
from pymobiledevice3.remote.core_device.diagnostics_service import DiagnosticsServiceService
p = argparse.ArgumentParser()
p.add_argument('--udid', required=True)
p.add_argument('--output', type=pathlib.Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
started = time.monotonic()
phase = 'preflight'
transferred = 0
invocations = 0

def event(action, **fields):
    record = {'run_id': a.output.name, 'direction_id': 'IOS-SYSDIAGNOSE', 'source': 'mac.pymobiledevice3', 'action': action, 'phase': phase, 'wall_time': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'monotonic_ns':time.monotonic_ns(), 'elapsed_s':time.monotonic()-started, **fields}
    with (a.output/'actions.jsonl').open('a') as stream: stream.write(json.dumps(record,default=str)+'\n')
    print(json.dumps(record,default=str), flush=True)

async def heartbeat():
    while True:
        await asyncio.sleep(30)
        event('progress',bytes_received=transferred,invocations=invocations)

async def capture():
    global phase, transferred, invocations
    assert importlib.metadata.version('pymobiledevice3') == '11.41.2'
    event('start',package_version='11.41.2',total_budget_s=900,full_logs=True,autopair=False,fallback=False)
    tunnel = UserspaceRsdTunnel(serial=a.udid, autopair=False, remotepairing_fallback=False)
    try:
        phase = 'rsd_connect'
        rsd = await asyncio.wait_for(tunnel.aopen(),60)
        assert rsd.udid == a.udid, 'RSD device identity mismatch'
        event('rsd_connected',services=rsd.peer_info['Services'],product_type=rsd.product_type,os_version=rsd.product_version,os_build=rsd.product_build_version,identity_matches=True)
        assert DiagnosticsServiceService.SERVICE_NAME in rsd.peer_info['Services'], 'diagnostic service absent'
        phase = 'service_connect'
        service = DiagnosticsServiceService(rsd)
        try:
            await asyncio.wait_for(service.connect(),60)
            event('service_connected')
            phase = 'capture'
            invocations += 1
            event('capture_invoke',api_call_count=invocations,device_received='unknown',dry_run=False)
            response = await asyncio.wait_for(service.capture_sysdiagnose(False),600)
            name = response.preferred_filename
            assert name == pathlib.Path(name).name and name not in ('','.','..'), 'unsafe filename'
            assert isinstance(response.file_size,int) and response.file_size > 0, 'invalid expected length'
            event('capture_response',filename=name,expected_length=response.file_size)
            phase = 'download'
            partial = a.output/(name+'.partial')
            digest = hashlib.sha256()
            with partial.open('xb') as destination:
                async for chunk in response.generator:
                    transferred += len(chunk)
                    assert transferred <= response.file_size, 'stream exceeded expected length'
                    destination.write(chunk); digest.update(chunk)
                destination.flush(); os.fsync(destination.fileno())
            assert transferred == response.file_size, 'short stream'
            final = a.output/name
            partial.rename(final)
            phase = 'download_complete'
            event('archive_downloaded',archive=name,bytes_received=transferred,sha256=digest.hexdigest())
            (a.output/'download.json').write_text(json.dumps({'status':'DOWNLOADED_NOT_YET_VALIDATED','archive':name,'expected_length':response.file_size,'actual_length':transferred,'sha256':digest.hexdigest(),'invocations':invocations},indent=2))
        except BaseException as exc:
            event('operation_error',error_type=type(exc).__name__,error=str(exc),code=getattr(exc,'code',None),user_info=getattr(exc,'user_info',None))
            raise
        finally:
            try: await asyncio.wait_for(service.close(),10)
            except BaseException as exc: event('service_cleanup_error',error=str(exc))
    finally:
        try: await asyncio.wait_for(tunnel.aclose(),10)
        except BaseException as exc: event('tunnel_cleanup_error',error=str(exc))
        event('client_closed',device_capture_may_continue=phase not in ('download_complete',))

async def main():
    progress = asyncio.create_task(heartbeat())
    try:
        async with asyncio.timeout(900): await capture()
    except BaseException as exc:
        event('failure',error_type=type(exc).__name__,error=str(exc),traceback=traceback.format_exc(),invocations=invocations,bytes_received=transferred)
        (a.output/'result.json').write_text(json.dumps({'status':'FAIL','api_call_count':invocations,'phase':phase,'error_type':type(exc).__name__,'error':str(exc)},indent=2))
        raise
    finally:
        progress.cancel()
        await asyncio.gather(progress,return_exceptions=True)
asyncio.run(main())

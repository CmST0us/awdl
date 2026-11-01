#!/usr/bin/env python3
"""Stream iPad JSON records; STOP closes at a complete output boundary."""
import argparse
import asyncio
from contextlib import asynccontextmanager, suppress
import json
import plistlib
import struct
import sys
import time


def phase(event, **fields):
    print(json.dumps(dict(event=event,monotonic=time.monotonic(),**fields)),file=sys.stderr,flush=True)


def disconnect_transport(connection, label):
    """STOP ends local capture; no peer handshake is needed after the read is cancelled."""
    if connection is not None and connection.writer is not None:
        phase('transport-disconnect', connection=label)
        connection.writer.transport.abort()


async def copy_records(records, stop, emit):
    """Cancel only the pending read; synchronous emit finishes before STOP runs."""
    pending = None
    try:
        while not stop.done():
            pending = asyncio.create_task(anext(records))
            done, _ = await asyncio.wait((pending, stop), return_when=asyncio.FIRST_COMPLETED)
            if pending in done:
                # Preserve an already decoded record even when STOP arrives together.
                emit(pending.result())
                pending = None
            if stop in done:
                stop.result()
                break
        stop.result()
    finally:
        if pending is not None:
            phase('read-cancel-requested')
            pending.cancel()
            with suppress(asyncio.CancelledError):
                await pending
            phase('read-cancelled')
        await records.aclose()
        phase('records-closed')


async def source_records(service, pid, ready):
    """The library's activity-stream protocol, with readiness at its accepted reply."""
    from pymobiledevice3.services.os_trace import (
        OS_TRACE_RELAY_MESSAGE_FILTER_ALL, OS_TRACE_RELAY_STREAM_FLAGS_DEFAULT,
        parse_syslog_entry,
    )
    await service.connect()
    wire=service.service
    await wire.send_plist(dict(Request='StartActivity',MessageFilter=OS_TRACE_RELAY_MESSAGE_FILTER_ALL,
                               Pid=pid,StreamFlags=OS_TRACE_RELAY_STREAM_FLAGS_DEFAULT))
    size_width=struct.unpack('<I',await wire.recvall(4))[0]
    size=int.from_bytes(await wire.recvall(size_width),'little')
    response=plistlib.loads(await wire.recvall(size))
    if response.get('Status')!='RequestSuccessful':raise RuntimeError('activity stream was not accepted')
    ready()
    while True:
        if await wire.recvall(1)!=b'\x02':raise RuntimeError('invalid activity stream record')
        size=struct.unpack('<I',await wire.recvall(4))[0]
        yield parse_syslog_entry(await wire.recvall(size))


@asynccontextmanager
async def open_provider(udid, transport):
    if transport == 'coredevice':
        from pymobiledevice3.remote import native_tunnel
        from native_transport import configure_native
        configure_native(native_tunnel)
        tunnel = native_tunnel.NativeRemotedTunnel(serial=udid)
        try:
            async with asyncio.timeout(5):
                provider = await tunnel.aopen()
            if provider.udid != udid:
                raise RuntimeError('CoreDevice returned a different device')
            yield provider
        finally:
            if tunnel.rsd is not None:
                if tunnel.rsd.lockdown is not None:
                    disconnect_transport(tunnel.rsd.lockdown.service, 'lockdown')
                writer = tunnel.rsd.service._writer
                if writer is not None:
                    writer.transport.abort()
            await asyncio.wait_for(tunnel.aclose(), 1)
    else:
        from pymobiledevice3.lockdown import create_using_usbmux
        async with asyncio.timeout(5):
            provider = await create_using_usbmux(serial=udid, autopair=False)
        try:
            yield provider
        finally:
            phase('lockdown-close-start')
            disconnect_transport(provider.service, 'lockdown')
            await provider.close()
            phase('lockdown-close-end')


async def collect(udid, run_id, pid=-1, transport='usb'):
    from pymobiledevice3.cli.syslog import format_json_line
    from pymobiledevice3.services.os_trace import OsTraceService

    loop = asyncio.get_running_loop()
    stop = loop.create_future()

    def control():
        line = sys.stdin.readline()
        loop.remove_reader(sys.stdin.fileno())
        if line.strip() == 'STOP':
            phase('stop-received')
            stop.set_result(None)
        else:
            stop.set_exception(RuntimeError('collector control disconnected or invalid'))

    loop.add_reader(sys.stdin.fileno(), control)
    try:
        async with open_provider(udid, transport) as lockdown:
            service=OsTraceService(lockdown=lockdown)
            try:
                ready=lambda: print(json.dumps(dict(event='collector-ready',run_id=run_id,requested_pid=pid,transport=transport)),flush=True)
                await copy_records(source_records(service,pid,ready), stop,
                                   lambda row: print(format_json_line(row), flush=True))
            finally:
                phase('service-close-start')
                disconnect_transport(service._service, 'activity')
                await service.close()
                phase('service-close-end')
        phase('complete-output')
        print(json.dumps(dict(event='collector-complete', run_id=run_id, requested_pid=pid)), flush=True)
    finally:
        loop.remove_reader(sys.stdin.fileno())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--udid', required=True)
    parser.add_argument('--run-id', required=True)
    parser.add_argument('--pid', type=int, default=-1)
    parser.add_argument('--transport', choices=('usb', 'coredevice'), default='usb')
    args = parser.parse_args()
    if args.pid != -1 and args.pid <= 0:parser.error('PID must be positive or -1')
    asyncio.run(collect(args.udid, args.run_id, args.pid, args.transport))


if __name__ == '__main__':
    main()

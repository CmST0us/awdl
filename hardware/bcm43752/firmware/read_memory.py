#!/usr/bin/env python3
"""Take read-only BCM43752 A2 evidence while all interfaces on its PHY are down."""

import argparse
import ctypes as C
import fcntl
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import sys

from inspect_firmware import FW_SHA256


class WlIoctl(C.Structure):
    _fields_ = [("cmd", C.c_uint32), ("buf", C.c_void_p), ("length", C.c_uint32),
                ("set", C.c_uint8), ("used", C.c_uint32), ("needed", C.c_uint32)]


class IfReq(C.Structure):
    _fields_ = [("name", C.c_char * 16), ("data", C.c_void_p), ("padding", C.c_char * 16)]


def read_iovar(sock, interface, name, payload=b"", *, host=False):
    buffer = C.create_string_buffer(8192)
    request = name.encode("ascii") + b"\0" + payload
    if len(request) > len(buffer):
        raise ValueError("iovar request too large")
    C.memmove(buffer, request, len(request))
    # DHD's private ioctl ABI reads a selector immediately after wl_ioctl_t.
    raw = C.create_string_buffer(C.sizeof(WlIoctl) + 4)
    C.c_uint32.from_buffer(raw, C.sizeof(WlIoctl)).value = 0x00444944 if host else 0
    command = WlIoctl.from_buffer(raw)
    command.cmd = 2 if host else 262
    command.buf = C.addressof(buffer)
    command.length = len(buffer)
    command.set = 0
    ifreq = IfReq()
    ifreq.name = interface.encode("ascii")
    ifreq.data = C.addressof(raw)
    libc = C.CDLL(None, use_errno=True)
    if libc.ioctl(sock.fileno(), 0x89F0, C.byref(ifreq)) != 0:
        code = C.get_errno()
        raise OSError(code, f"GET {name}: {os.strerror(code)}")
    return buffer.raw


def require_interfaces_down(interface):
    net = Path("/sys/class/net")
    selected = net / interface
    phy = (selected / "phy80211").resolve(strict=True)
    pci = phy / "device"
    if (pci / "vendor").read_text().strip() != "0x14e4" or (pci / "device").read_text().strip() != "0x449d":
        raise ValueError("interface is not PCI 14e4:449d")
    for entry in net.iterdir():
        if (entry / "phy80211").exists() and (entry / "phy80211").resolve() == phy:
            if int((entry / "flags").read_text().strip(), 16) & 1:
                raise ValueError(f"{entry.name} is UP; stop its owner and bring it down before reading")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interface", required=True, help="existing BCM43752 interface, 1–15 ASCII bytes")
    parser.add_argument("--firmware", required=True, type=Path, help="original A2 firmware file to identity-check")
    parser.add_argument("--output", required=True, type=Path, help="new private evidence directory; must not exist")
    parser.add_argument("--lease-fd", type=int, help="optional inherited descriptor for the existing device lease")
    args = parser.parse_args()
    lease = None
    try:
        if sys.platform != "linux" or C.sizeof(WlIoctl) != 32 or C.sizeof(IfReq) != 40:
            raise ValueError("requires the Linux 64-bit DHD private ioctl ABI")
        if not 0 < len(args.interface.encode("ascii")) < 16 or "/" in args.interface:
            raise ValueError("invalid interface name")
        if hashlib.sha256(args.firmware.read_bytes()).hexdigest() != FW_SHA256:
            raise ValueError("firmware SHA-256 differs from the supported A2 image")
        lock = Path("/run/lock/wifi-aware-device.lock")
        if args.lease_fd is None:
            lease = os.open(lock, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
        else:
            lease = os.dup(args.lease_fd)
            own, expected = os.fstat(lease), lock.stat()
            if (own.st_dev, own.st_ino) != (expected.st_dev, expected.st_ino):
                raise ValueError("inherited descriptor does not reference the shared device lease")
        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        require_interfaces_down(args.interface)
        os.umask(0o077)
        args.output.mkdir(mode=0o700, parents=False, exist_ok=False)
        result = {"interface": args.interface, "firmware_sha256": FW_SHA256, "queries": {}, "regions": {}}
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            for name, payload in [("ver", b""), ("cap", b""),
                                  ("nan", struct.pack("<HBBHHIH2x", 0x8000, 1, 0, 0, 6, 1, 0))]:
                try:
                    data = read_iovar(sock, args.interface, name, payload)
                    result["queries"][name] = {"reply_hex": data[:64].hex()} if name == "nan" else {
                        "text": data.split(b"\0", 1)[0].decode("utf-8", errors="replace")}
                except OSError as exc:
                    result["queries"][name] = {"errno": exc.errno, "error": str(exc)}
            for name, start, size in [("low-memory", 0, 0x160000), ("ram", 0x170000, 0x140000)]:
                require_interfaces_down(args.interface)
                region = bytearray()
                for address in range(start, start + size, 4096):
                    count = min(4096, start + size - address)
                    region.extend(read_iovar(sock, args.interface, "membytes",
                                             struct.pack("<II", address, count), host=True)[:count])
                (args.output / (name + ".bin")).write_bytes(region)
                result["regions"][name] = {"start": hex(start), "bytes": size,
                                           "sha256": hashlib.sha256(region).hexdigest()}
        require_interfaces_down(args.interface)
        (args.output / "evidence.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))
    except (OSError, ValueError, UnicodeError) as exc:
        parser.exit(1, f"snapshot failed: {exc}\n")
    finally:
        if lease is not None:
            os.close(lease)  # Do not unlock a descriptor still owned by the parent operation.


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""AirDrop sender advertisement on an idle Linux HCI adapter (normal LE commands)."""
import fcntl
import ctypes
import os
import signal
import socket
import struct


class Advertisement:
    def __init__(self, index=0):
        self.index = index
        self.sock = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI)
        self.advertising = False

    def command(self, opcode, data):
        self.sock.send(struct.pack('<BHB', 1, opcode, len(data))+data)
        while True:
            packet = self.sock.recv(260)
            if len(packet) >= 7 and packet[1] == 0x0e and struct.unpack_from('<H', packet, 4)[0] == opcode:
                if packet[6]:
                    raise OSError(f'HCI command {opcode:04x}: status {packet[6]:02x}')
                return
            if len(packet) >= 7 and packet[1] == 0x0f and struct.unpack_from('<H', packet, 5)[0] == opcode and packet[3]:
                raise OSError(f'HCI command {opcode:04x}: status {packet[3]:02x}')

    def __enter__(self):
        info = bytearray(92)
        struct.pack_into('<H', info, 0, self.index)
        fcntl.ioctl(self.sock, 0x800448d3, info, True)  # HCIGETDEVINFO
        if struct.unpack_from('<I', info, 16)[0] & 1:
            self.sock.close()
            raise RuntimeError('HCI adapter is already in use; use a dedicated idle adapter')
        try:
            # HCI_CHANNEL_USER atomically claims the idle controller. The
            # kernel removes it from management while this socket owns it,
            # so BlueZ and kernel initialization cannot interleave commands.
            # CPython's HCI address tuple omits hci_channel; pass Linux's
            # sockaddr_hci directly instead of silently binding RAW.
            address = struct.pack('HHH', socket.AF_BLUETOOTH, self.index, 1)
            libc = ctypes.CDLL(None, use_errno=True)
            libc.bind.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint]
            libc.bind.restype = ctypes.c_int
            if libc.bind(self.sock.fileno(), address, len(address)):
                error = ctypes.get_errno()
                raise OSError(error, os.strerror(error))
            self.sock.settimeout(3)
            # Reset the idle controller before selecting legacy LE advertising.
            self.command(0x0c03, b'')
            # 100 ms, connectable undirected, public address, all three channels.
            self.command(0x2006, struct.pack('<HHBBB6sBB', 160, 160, 0, 0, 0, bytes(6), 7, 0))
            manufacturer = bytes.fromhex('4c00051240') + os.urandom(3) + bytes(4) + bytes([3]) + bytes(9)
            data = bytes([2, 1, 6, len(manufacturer)+1, 0xff]) + manufacturer
            self.command(0x2008, bytes([len(data)]) + data.ljust(31, b'\0'))
            self.advertising = True
            self.command(0x200a, bytes([1]))
            print('AIRDROP_BLE_READY', flush=True)
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *args):
        # Closing a user channel powers down the controller and returns it
        # to management, including when setup or advertising fails.
        try:
            if self.advertising:
                self.command(0x200a, bytes([0]))
                self.advertising = False
        finally:
            self.sock.close()


if __name__ == '__main__':
    import threading
    done = threading.Event()
    for signum in (signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, lambda *_: done.set())
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--index", type=int, required=True)
    args = parser.parse_args()
    with Advertisement(args.index):
        done.wait()

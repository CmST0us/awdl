#!/usr/bin/env python3
"""Inspect the fixed BCM43752 A2 image and an optional read-only device snapshot."""

import argparse
import hashlib
import json
from pathlib import Path
import struct


FW_SHA256 = "6a2dbe01e72221defba91a52e158768d973a3c85ca2d881c924379e35ad36b23"
RAM_BASE = 0x170000
RAM_SIZE = 0x140000
PATCH_START = 0x253D24
PATCH_END = 0x25478C
UNIMPL = 0x1A5689
TAIL_CALLS = (0x20832, 0x33672, 0x33A42)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def thumb_bl(data, address, *, link=True):
    """Decode a Thumb BL, or its B.W tail-call encoding when link is false."""
    first, second = struct.unpack_from("<HH", data, address)
    expected = 0xD000 if link else 0x9000
    if first & 0xF800 != 0xF000 or second & 0xD000 != expected:
        raise ValueError(f"expected Thumb {'BL' if link else 'B.W'} at {address:#x}")
    sign = (first >> 10) & 1
    i1 = 1 ^ ((second >> 13) & 1) ^ sign
    i2 = 1 ^ ((second >> 11) & 1) ^ sign
    offset = ((sign << 24) | (i1 << 23) | (i2 << 22)
              | ((first & 0x3FF) << 12) | ((second & 0x7FF) << 1))
    if sign:
        offset -= 1 << 25
    return address + 4 + offset


def inspect(firmware, low_memory=None):
    if sha256(firmware) != FW_SHA256:
        raise ValueError("unsupported image SHA-256; A2 addresses must not be used on another build")
    records = []
    for address in range(PATCH_START, PATCH_END, 8):
        target, source = struct.unpack_from("<II", firmware, address - RAM_BASE)
        if target + 8 > RAM_BASE or not RAM_BASE <= source <= RAM_BASE + len(firmware) - 8:
            raise ValueError(f"invalid flashpatch record at {address:#x}")
        record = {"target": hex(target), "source": hex(source)}
        if low_memory is not None:
            record["live_matches"] = low_memory[target:target + 8] == firmware[source - RAM_BASE:source - RAM_BASE + 8]
        records.append(record)
    result = {
        "firmware_sha256": sha256(firmware),
        "firmware_bytes": len(firmware),
        "ram_base": hex(RAM_BASE), "ram_bytes": RAM_SIZE,
        "flashpatch_table_start": hex(PATCH_START),
        "flashpatch_table_end_exclusive": hex(PATCH_END),
        "flashpatch_count": len(records),
        "nan_runtime_support": "not established by static inspection",
    }
    if low_memory is None:
        return result
    if len(low_memory) not in (0x160000, 0x170000):
        raise ValueError("ROM snapshot must cover [0, 0x160000); old 0x170000-byte captures are accepted offline only")
    mismatches = [record for record in records if not record["live_matches"]]
    if mismatches:
        raise ValueError(f"live flashpatch mismatch: {mismatches}")
    result["low_memory_sha256"] = sha256(low_memory)
    result["live_flashpatch_matches"] = len(records)
    markers = [b"nan_fsm_timer\0", b"nan_avail_update_chanscheds_to_max_period\0",
               b"NAN PMK Name\0", b"NAN Pairwise key expansion\0"]
    result["rom_strings"] = {}
    for marker in markers:
        offset = low_memory.find(marker)
        if offset < 0:
            raise ValueError(f"expected ROM marker missing: {marker!r}")
        result["rom_strings"][marker[:-1].decode()] = hex(offset)
    # These call sites are individually disassembled A2 instructions, not B0 map addresses.
    calls = [0x1DB9C, 0x1E322, 0x1E5AE, 0x1E5BA, 0x1E696, 0x1E7C6,
             0x1EFD0, 0x1F09C, 0x1F1E4, 0x1F1F8, 0x1F200, 0x1F206,
             # DAM schedule construction/commit, not just its three outer callers.
             0x20186, 0x203E4, 0x207E8, 0x20832, 0x20A84,
             0x20B3C, 0x20B4A, 0x20C0C, 0x20EB2, 0x2174A,
             0x21842, 0x21A06, 0x21D7E, 0x21F44,
             0x26A22, 0x26A2A, 0x26B34, 0x28612, 0x2861A,
             0x28A12, 0x28A52, 0x28FBE,
             0x28CC4, 0x28CD2, 0x28DB0, 0x20B56, 0x21C18, 0x33AE2, 0x2E7FE,
             0x3216E, 0x33672, 0x33B08, 0x33B3E,
             0x37F00, 0x38210, 0x2288E, 0x3700A, 0x33A42, 0x2F874,
             0x133ACC]
    result["unimplemented_nan_callbacks"] = []
    for caller in calls:
        trampoline = thumb_bl(low_memory, caller, link=caller not in TAIL_CALLS)
        if not 0 <= trampoline <= len(low_memory) - 16:
            raise ValueError(f"invalid ROM trampoline at {trampoline:#x}")
        if low_memory[trampoline:trampoline + 10] != bytes.fromhex("10b5024c2468019410bd"):
            raise ValueError(f"unexpected ROM trampoline at {trampoline:#x}")
        slot, = struct.unpack_from("<I", low_memory, trampoline + 12)
        if not RAM_BASE <= slot <= RAM_BASE + len(firmware) - 4:
            raise ValueError(f"invalid RAM callback slot at {slot:#x}")
        target, = struct.unpack_from("<I", firmware, slot - RAM_BASE)
        if target != UNIMPL:
            raise ValueError(f"callback {slot:#x} no longer points to the expected UNIMPL handler")
        result["unimplemented_nan_callbacks"].append({
            "caller": hex(caller), "trampoline": hex(trampoline),
            "slot": hex(slot), "target": hex(target),
        })
    result["limitation"] = "NAN ROM calls missing RAM callbacks; enabling a capability bit cannot restore these functions"
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=Path, help="unmodified 18.35.387.23.146 A2 image")
    parser.add_argument("--low-memory", type=Path, help="optional raw snapshot of addresses 0 through 0x15ffff")
    args = parser.parse_args()
    try:
        print(json.dumps(inspect(args.firmware.read_bytes(),
                                 args.low_memory.read_bytes() if args.low_memory else None), indent=2))
    except (OSError, ValueError, struct.error) as exc:
        parser.exit(1, f"inspection failed: {exc}\n")


if __name__ == "__main__":
    main()

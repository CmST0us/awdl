#!/usr/bin/env python3
"""Build the A2 NAN firmware restoration candidate; not yet a firmware release."""

import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import struct
import subprocess

from inspect_firmware import (FW_SHA256, RAM_BASE, UNIMPL, TAIL_CALLS, inspect, thumb_bl,
                              PATCH_START as ORIGINAL_FP_START,
                              PATCH_END as ORIGINAL_FP_END)


# Fixed addresses for 18.35.387.23.146, not operator configuration.
ROM_SHA256 = "f2c9e3249ad09f846c58585d08997282ff895b196ef32c60c3a9b9d17baaa1d6"
# The original low heap starts at 0x170405. Reserve its final 56 KiB;
# native hnd_image_info and MPU splitting exclude this same checked range.
# Flashpatch replacement data keeps its separate hardware-fixed 0x180000 base.
PATCH_BASE = 0x172000
PATCH_END = 0x180000
HEAP_END_LITERAL = 0x1A62B8
EXEC_START_LITERAL = 0x1A627C
FP_DATA_START = 0x180000
FP_DATA_END = 0x181000
# Each replacement fits inside one hardware 8-byte block. The tail hook must
# preserve LR because it replaces a function entry, rather than a BL caller.
ROM_HOOKS = (
    (0x128590, "awdl_group_ring_state", "tail", bytes.fromhex("13b50e4c")),
    (0x22722, "awdl_nan_channel_close", "tail", bytes.fromhex("d7f87806")),
    (0x22760, "awdl_nan_channel", "call", 0x1278B4),
    (0x11B284, "awdl_action_header", "tail", bytes.fromhex("2de9f04f")),
    (0x119300, "nan_station_rx_lookup", "call", 0x123DC0),
    (0x11A26A, "nan_station_rx_lookup", "call", 0x123DC0),
    (0x1277C0, "slotted_bss_start_pre_close_timer", "tail", bytes.fromhex("f8b50568")),
    (0x127968, "slotted_bss_stop_pre_close_timer", "tail", bytes.fromhex("426808b5")),
    (0x19C00, "nan_group_bip_mic", "tail", bytes.fromhex("7fb5057c")),
    (0x2B5F0, "nan_group_beacon_copy", "tail", bytes.fromhex("2de9f341")),
    (0x2F9E4, "nan_group_beacon_rx", "tail", bytes.fromhex("2de9f04f")),
    (0x19D60, "nan_group_bip_tx", "tail", bytes.fromhex("2de9f34d")),
    (0xF059C, "nan_group_mfp_rx", "tail", bytes.fromhex("2de9f04f")),
    (0x2F8C4, "nan_host_rx_action", "tail", bytes.fromhex("2de9f04f")),
    (0x29518, "nan_host_dw_start", "call", 0x2B728),
    (0x37CA8, "nan_scheduler_mac_event", "call", 0x2E4D8),
    (0x2B5AC, "nan_mac_merge_candidate", "tail", bytes.fromhex("38b5036a")),
    (0x1FD58, "nan_bss_prepare_enable", "tail", bytes.fromhex("2de9f04f")),
    (0x1E464, "nan_bss_events", "tail", bytes.fromhex("2de9f347")),
    (0x1F040, "nan_common_enable", "tail", bytes.fromhex("01292de9")),
    (0x2E87C, "nan_mac_stop_bss", "tail", bytes.fromhex("0b68d0f8")),
    (0x1EFB4, "nan_common_disable", "tail", bytes.fromhex("01292de9")),
    (0x2EEF4, "nan_mac_enable_active", "tail", bytes.fromhex("2de9f743")),
    (0x39A3C, "nan_security_ext_iv", "tail", bytes.fromhex("83689879")),
    (0x2EDF0, "nan_mac_disable_active", "tail", bytes.fromhex("70b54c69")),
    (0x2C7E4, "nan_msch_unregister_owned", "call", 0xF7114),
    (0x2E1E2, "nan_msch_unregister_owned", "call", 0xF7114),
    (0x1DB90, "nan_initialize", "call", 0x1E764),
    (0x1FEEC, "nan_mac_refresh_beacons", "tail", bytes.fromhex("38b50c46")),
    (0x2E728, "nan_mac_prepare_bsscfg", "tail", bytes.fromhex("2de9f047")),
    (0x2E838, "nan_mac_remove_bsscfg", "tail", bytes.fromhex("38b50021")),
    (0x127080, "nan_slotted_release_bss", "call", 0x1263BC),
    (0x308A0, "nan_availability_deinitialize", "tail", bytes.fromhex("70b5cc6c")),
    (0x38354, "nan_scheduler_register", "tail", bytes.fromhex("2de9f04f")),
    (0xF87DA, "nan_msch_trace_allocate", "tail", bytes.fromhex("4ff40170")),
    (0x21B74, "nan_dam_disable", "tail", bytes.fromhex("70b5cd6d")),
    (0x216B0, "nan_dam_update_peer_ndl_slots", "tail", bytes.fromhex("2de9f04f")),
    (0x20AB8, "nan_dam_generate_proposal", "tail", bytes.fromhex("2de9f74f")),
    (0x31D88, "nan_dam_resize_map", "tail", bytes.fromhex("2de9f04f")),
    (0x21E24, "nan_dam_release_session", "tail", bytes.fromhex("13b5b1b1")),
    (0x21B48, "nan_dam_deactivate_complete", "tail", bytes.fromhex("38b50c46")),
    (0x34278, "nan_dam_deactivate_ndl", "tail", bytes.fromhex("38b50446")),
    (0x21CDC, "nan_dam_parse_peer_schedule", "tail", bytes.fromhex("2de9f74f")),
    (0x21E68, "nan_dam_process_counter", "tail", bytes.fromhex("2de9f04f")),
    (0x227E4, "nan_data_create", "tail", bytes.fromhex("2de9f743")),
    (0x1EC62, "nan_data_interface_info", "call", 0x128460),
    (0x228C4, "nan_data_remove", "tail", bytes.fromhex("f8b50d46")),
    (0x229A4, "nan_data_deinitialize", "tail", bytes.fromhex("70b5c1b1")),
    (0x370CC, "nan_peer_deinitialize", "tail", bytes.fromhex("086c10b5")),
    (0x342D0, "nan_ndl_deinitialize", "tail", bytes.fromhex("70b58c6b")),
    (0x357A0, "nan_ndp_deinitialize", "tail", bytes.fromhex("38b54c6b")),
    (0x37214, "nan_peer_release", "tail", bytes.fromhex("f7b54e7a")),
    (0x36F8C, "nan_peer_disable", "tail", bytes.fromhex("38b50d46")),
    (0x34C84, "nan_ndp_entry_allocate", "tail", bytes.fromhex("2de9f843")),
    (0x35978, "nan_ndp_entry_release", "tail", bytes.fromhex("70b591f8")),
    (0x25D3C, "nan_discovery_bloom_create", "call", 0x97B0),
    (0x1E64C, "nan_deinitialize", "tail", bytes.fromhex("70b50646")),
    (0x28CBC, "nan_fsm_deinitialize", "tail", bytes.fromhex("70b50c4e")),
    (0x28E08, "nan_fsm_initialize", "tail", bytes.fromhex("f8b50746")),
)
# Native RAM entries have direct callers as well as ROM trampolines.
# nan_group.c preserves their overwritten prologues for non-NAN callers.
RAM_KEY_HOOKS = (
    (0x1F06C4, "awdl_data_first_flow", bytes.fromhex("b6f842e0")),
    (0x1F0706, "awdl_data_next_flow", bytes.fromhex("b1f84230")),
    (0x1F071C, "awdl_data_first_sequence", bytes.fromhex("b6f85a30")),
    (0x1F0730, "awdl_data_next_sequence", bytes.fromhex("b1f85a30")),
    (0x1F774E, "awdl_data_headroom_gate", bytes.fromhex("5f2b04d9")),
    (0x1F8794, "nan_station_tx_gate", bytes.fromhex("736f052b")),
    (0x1880B0, "nan_station_rx_key", bytes.fromhex("b24b2de9f04f")),
    (0x1A9798, "nan_group_sendmgmt", bytes.fromhex("2de9f04f")),
    (0x187688, "nan_group_bss_key", bytes.fromhex("f8b520b319b34569")),
    (0x188FCC, "nan_group_scb_key", bytes.fromhex("2de9f04f")),
    (0x1876F2, "nan_group_tx_id", bytes.fromhex("8b6c10b5")),
)
RAM_CALL_HOOKS = ((0x1F8880, "awdl_data_prepare", 0x1F7718),
                  (0x1F8A34, "awdl_data_prepare_next_gate", 0x1F276C),
                  (0x1F51D6, "nan_station_rx_lookup", 0x123DC0),
                  (0x1F78C2, "awdl_data_encapsulate", 0x1EE06E),
                  (0x1F1B9A, "awdl_data_dma", 0x1ED804))
# Verification, diagnostic count, and hardware programming use these bounds.
# The original table's reclaim/image-info bounds deliberately remain unchanged.
FP_TABLE_REFERENCES = ((0x21258C, 0x212590), (0x212658, 0x21265C),
                       (0x21271C, 0x212720))
CALLBACKS = {
    0x1812EC: ("nan_statistics", 0x1E322, 0x151070),
    0x1812E4: ("nan_modules_enable", 0x1F09C, 0x151050),
    0x1812E8: ("nan_modules_disable", 0x1EFD0, 0x151060),
    0x1812DC: ("nan_modules_initialize", 0x1E7C6, 0x151030),
    0x1812E0: ("nan_modules_deinitialize", 0x1E696, 0x151040),
    0x1812D0: ("nan_vendor_attributes", 0x1E5BA, 0x151000),
    0x1812D4: ("nan_vendor_attribute_count", 0x1E5AE, 0x151010),
    0x1812F0: ("nan_disc_frm_ctx_sz_get", 0x1F1E4, 0x151080),
    0x1812F4: ("nan_disc_frm_ctx_init", 0x1F1F8, 0x151090),
    0x1812F8: ("nan_ndpe_frame_events", 0x26A22, 0x1510A0),
    0x1812FC: ("nan_ndpe_fsm_handlers", 0x26B34, 0x1510B0),
    0x181300: ("nan_ndpe_frame_event_count", 0x26A2A, 0x1510C0),
    0x181304: ("nan_ndpe_frame_attribute_count", 0x2861A, 0x1510D0),
    0x181308: ("nan_ndpe_frame_attributes", 0x28612, 0x1510E0),
    0x18130C: ("nan_fsm_frm_ctx_sz_get", 0x28DB0, 0x1510F0),
    0x181310: ("nan_fsm_res_ctx_cubby_sz_get", 0x28A12, 0x151100),
    0x181314: ("nan_fsm_implementation_count", 0x28CC4, 0x151110),
    0x181318: ("nan_fsm_implementation", 0x28CD2, 0x151120),
    0x18131C: ("nan_mac_frm_ctx_sz_get", 0x1F200, 0x151130),
    0x181320: ("nan_mac_frm_ctx_init", 0x1F206, 0x151140),
    0x181A7C: ("slotted_bss_register_callbacks", 0x2E7FE, 0x152EB0),
    0x1816EC: ("nan_scheduler_slot_start", 0x37F00, 0x152070),
    0x1816F0: ("nan_scheduler_unregister", 0x38210, 0x152080),
    0x1816E8: ("nan_data_register_callbacks", 0x2288E, 0x152060),
    0x1818C8: ("bsscfg_now", 0x3700A, 0x1527E0),
    0x1816E0: ("nan_dam_deactivate", 0x33A42, 0x152040),
    0x1816A0: ("nan_dam_compatible_with_bt", 0x20EB2, 0x151F40),
    0x1816A4: ("nan_dam_valid_chanspec", 0x203E4, 0x151F50),
    0x18169C: ("nan_dam_update_ndc_entry", 0x21F44, 0x151F30),
    0x181698: ("nan_dam_update_social_channels", 0x21C18, 0x151F20),
    0x1816A8: ("nan_dam_update_schedule", 0x20186, 0x151F60),
    0x1816AC: ("nan_dam_update_peer_ndl_slot", 0x2174A, 0x151F70),
    0x1816B0: ("nan_dam_update_for_excursion", 0x21842, 0x151F80),
    0x1816B8: ("nan_dam_watchdog_restore_slots", 0x21A06, 0x151FA0),
    0x1816C4: ("nan_dam_proposal_from_committed", 0x20B3C, 0x151FD0),
    0x1816C8: ("nan_dam_proposal_from_potential", 0x20B4A, 0x151FE0),
    0x1816CC: ("nan_dam_response_from_type", 0x20A84, 0x151FF0),
    0x181690: ("nan_dam_proposal_ndc_slots", 0x20B56, 0x151F00),
    0x181694: ("nan_dam_response_ndc_slots", 0x20C0C, 0x151F10),
    0x1816E4: ("nan_dam_resize_schedule", 0x3216E, 0x152050),
    0x1816D8: ("nan_dam_process_response", 0x33B3E, 0x152020),
    0x1816DC: ("nan_dam_activate", 0x33672, 0x152030),
    0x1816C0: ("nan_dam_check_peer_crb", 0x21D7E, 0x151FC0),
    0x1816D0: ("nan_dam_get_proposal", 0x33AE2, 0x152000),
    0x1816D4: ("nan_dam_process_request", 0x33B08, 0x152010),
    0x1818D0: ("nan_tx_packet_header", 0x2F874, 0x152800),
}

# The slotted-BSS data stop path reaches this ROM trampoline. Original A2
# RAM replacements for 0x134548/0x134b28 already use the same real helper,
# but the ROM callback was left at UNIMPL. Reconnect it without duplicating
# the chip's FIFO mask policy or bypassing actual queue synchronization.
NATIVE_CALLBACKS = {
    0x1813E0: ("native_tx_fifo_mask", 0x133ACC, 0x151440,
               0x1E931A, bytes.fromhex("40f2ff307047")),
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def checked_range(data, address, size):
    offset = address - RAM_BASE
    if offset < 0 or offset + size > len(data):
        raise ValueError(f"image range out of bounds: {address:#x}+{size:#x}")
    return data[offset:offset + size]


def encode_thumb_bl(address, target):
    offset = target - address - 4
    if offset & 1 or not -(1 << 24) <= offset < (1 << 24):
        raise ValueError("Thumb BL target out of range or unaligned")
    offset &= (1 << 25) - 1
    sign = offset >> 24
    j1 = 1 ^ ((offset >> 23) & 1) ^ sign
    j2 = 1 ^ ((offset >> 22) & 1) ^ sign
    return struct.pack("<HH", 0xF000 | (sign << 10) | ((offset >> 12) & 0x3FF),
                       0xD000 | (j1 << 13) | (j2 << 11) | ((offset >> 1) & 0x7FF))


def build(firmware_path, rom_path, output, prefix):
    firmware, rom = firmware_path.read_bytes(), rom_path.read_bytes()
    # Require the exact valid snapshot, including its original flashpatches.
    if digest(rom) != ROM_SHA256:
        raise ValueError("unsupported ROM snapshot hash")
    inspection = inspect(firmware, rom)
    for slot, (_, caller, trampoline) in CALLBACKS.items():
        if thumb_bl(rom, caller, link=caller not in TAIL_CALLS) != trampoline:
            raise ValueError(f"changed ROM caller at {caller:#x}")
        if struct.unpack_from("<I", rom, trampoline + 12)[0] != slot:
            raise ValueError(f"changed trampoline slot at {trampoline:#x}")
        if checked_range(firmware, slot, 4) != struct.pack("<I", UNIMPL):
            raise ValueError(f"changed callback at {slot:#x}")
    for slot, (_, caller, trampoline, target, expected) in NATIVE_CALLBACKS.items():
        if (thumb_bl(rom, caller) != trampoline or
                struct.unpack_from("<I", rom, trampoline + 12)[0] != slot or
                checked_range(firmware, target, len(expected)) != expected):
            raise ValueError(f"changed native callback binding at {slot:#x}")
    for literal in (HEAP_END_LITERAL, EXEC_START_LITERAL):
        if checked_range(firmware, literal, 4) != struct.pack("<I", PATCH_END):
            raise ValueError(f"changed memory boundary at {literal:#x}")
    if any(checked_range(firmware, PATCH_BASE, PATCH_END - PATCH_BASE)):
        raise ValueError("payload storage is not the expected zero-filled image region")

    project = Path(__file__).resolve().parents[3]
    output = output.resolve()
    if not output.is_relative_to(project / ".build"):
        raise ValueError("expanded build output must be below this project's .build")
    if output.exists():
        raise ValueError("output already exists; choose a new build directory")
    tools = {}
    for name in ("gcc", "nm", "readelf", "objcopy", "objdump"):
        tool = shutil.which(prefix + name)
        if not tool:
            raise ValueError(f"missing tool: {prefix + name}")
        tools[name] = tool
    output.mkdir(parents=True)
    source_hashes = {}
    for name in ("native_calls.h", "runtime.c", "frame_contexts.c", "nan_fsm.c", "nan_fsm.h", "nan_commands.c", "nan_commands.h", "nan_initialize.c", "nan_initialize.h",
                 "nan_mac.c", "nan_mac.h", "nan_bss.c", "nan_availability.c", "nan_availability.h",
                 "nan_vendor_attributes.c", "nan_dam.c", "nan_dam.h", "nan_dam_channels.c", "nan_dam_ndc.c", "nan_dam_slots.c", "nan_dam_proposal.c", "nan_dam_resize.c", "nan_dam_negotiation.c", "nan_dam_activation.c", "nan_dam_peer.c",
                 "nan_scheduler.c", "nan_scheduler.h", "nan_schedule.c", "nan_schedule.h", "nan_data.c", "nan_data.h",
                 "nan_peer.c", "nan_peer.h",
                 "nan_link.c", "nan_link.h",
                 "nan_discovery.c", "nan_discovery.h",
                 "nan_attach.c", "nan_active.c", "nan_bss_active.c", "nan_host.c", "nan_host.h",
                 "nan_station.c", "nan_station.h", "nan_group.c", "nan_group.h", "../include/nan_host_abi.h",
                 "slotted_bss.c", "awdl_session.c", "awdl_session.h",
                 "awdl_command.c", "awdl_command.h",
                 "awdl_event.c", "awdl_event.h",
                 "awdl_tx.c", "awdl_tx.h",
                 "awdl_schedule.c", "awdl_schedule.h",
                 "awdl_peer.c", "awdl_peer.h",
                 "awdl_data.c",
                 "awdl_native.c", "awdl_native.h", "awdl_timing.c", "awdl_timing.h",
                 "../include/awdl_host_abi.h", "../include/awdl_frame.h"):
        data = (Path(__file__).parent / name).read_bytes()
        source_hashes[name] = digest(data)
        (output / Path(name).name).write_bytes(data)
    native_symbols = "\n".join(
        f".global native_0x{address}\n.type native_0x{address}, %function\n.thumb_set native_0x{address}, 0x{address}"
        for address in re.findall(r"extern unsigned char native_0x([0-9a-f]+)\[\];",
                                  (output / "native_calls.h").read_text()))
    (output / "native_calls.S").write_text(".syntax unified\n.thumb\n" + native_symbols + "\n")
    linker = output / "nan_patch.ld"
    linker.write_text(f"""OUTPUT_FORMAT(elf32-littlearm)
OUTPUT_ARCH(arm)
SECTIONS {{
  . = 0x{PATCH_BASE:x};
  .text : {{ *(.text*) *(.rodata*) }}
  .forbidden_data : {{ *(.data*) *(.bss*) *(COMMON) *(.got*) }}
  /DISCARD/ : {{ *(.comment) *(.ARM.exidx*) *(.ARM.extab*) }}
  ASSERT(SIZEOF(.forbidden_data) == 0, "patch must not use mutable static storage")
  ASSERT(. <= 0x{PATCH_END:x}, "patch exceeds reserved memory")
}}
""")
    commands = []

    def run(name, *args):
        command = [tools[name], *map(str, args)]
        commands.append(command)
        return subprocess.run(command, cwd=output, check=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    compiler = run("gcc", "--version").stdout.splitlines()[0]
    elf = output / "nan_patch.elf"
    # Firmware enters through patched pointers/branches, which have no ELF
    # relocation back to their targets. Root every such entry explicitly
    # before discarding unreachable sections; native callbacks referenced
    # by our C tables remain reachable through their ordinary relocations.
    entrypoints = sorted(
        {"nan_wlc_attach_modules", "nan_host_rx_auth", "nan_station_tx_owner", "awdl_data_headroom", "awdl_data_fragment_field", "awdl_data_prepare_next", "awdl_peer_bar_complete", "memcpy", "memset"}
        | {row[1] for row in ROM_HOOKS}
        | {row[1] for row in RAM_KEY_HOOKS}
        | {row[1] for row in RAM_CALL_HOOKS}
        | {row[0] for row in CALLBACKS.values()}
    )
    # Reduce the combined NAN/AWDL image using cross-module optimization;
    # its actual fit remains enforced by the linker and Flashpatch table
    # check. The checked native low-heap/execute boundary reserves 56 KiB;
    # both literals move together, leaving [0x170405,0x172000) for the
    # original low-heap arena. No other heap or RAM region is repurposed.
    # Naked native-entry trampolines carry noipa on their definitions so
    # interprocedural optimization cannot rewrite their register contracts.
    run("gcc", "-std=c23", "-march=armv7-r", "-mthumb", "-mfloat-abi=soft", "-Oz",
        "-flto", "-flto-partition=one",
        "-ffunction-sections", "-fdata-sections",
        "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-pic",
        "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-fno-common",
        "-Wall", "-Wextra", "-Werror", "-nostdlib", "-nostartfiles",
        "-Wl,--build-id=none", "-Wl,--no-undefined", "-Wl,--fatal-warnings",
        "-Wl,--gc-sections", *["-Wl,--undefined=" + name for name in entrypoints],
        "-Wl,-T," + str(linker), "-Wl,-Map," + str(output / "nan_patch.map"),
        "awdl_session.c", "awdl_native.c", "awdl_timing.c", "awdl_command.c", "awdl_event.c", "awdl_tx.c", "awdl_schedule.c", "awdl_peer.c", "awdl_data.c",
        "native_calls.S", "runtime.c", "frame_contexts.c", "nan_fsm.c", "nan_commands.c", "nan_initialize.c", "nan_mac.c",
        "nan_bss.c", "nan_availability.c", "nan_vendor_attributes.c", "nan_dam.c", "nan_dam_channels.c", "nan_dam_ndc.c", "nan_dam_slots.c", "nan_dam_proposal.c", "nan_dam_resize.c", "nan_dam_negotiation.c", "nan_dam_activation.c", "nan_dam_peer.c", "nan_scheduler.c", "nan_data.c",
        "nan_peer.c", "nan_link.c", "nan_discovery.c", "nan_attach.c", "nan_active.c", "nan_bss_active.c", "nan_host.c", "nan_station.c", "nan_group.c", "nan_schedule.c", "slotted_bss.c", "-o", elf)
    if run("nm", "--undefined-only", elf).stdout.strip():
        raise ValueError("payload has unresolved external symbols")
    relocations = run("readelf", "-rW", elf).stdout
    if "There are no relocations" not in relocations:
        raise ValueError("payload retains relocations")
    symbols = {}
    for line in run("nm", "--defined-only", elf).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in ("T", "t"):
            symbols[parts[2]] = int(parts[0], 16)
    payload_path = output / "nan_patch.bin"
    run("objcopy", "-O", "binary", "--only-section=.text", elf, payload_path)
    payload = payload_path.read_bytes()
    if not 0 < len(payload) <= PATCH_END - PATCH_BASE:
        raise ValueError("invalid payload size")
    (output / "nan_patch.disassembly.txt").write_text(run("objdump", "-d", elf).stdout)

    # Extend the existing hardware Flashpatch table. Its data source is an
    # 8-byte slot in the original 0x180000..0x181000 replacement-data pool;
    # the hardware encodes that slot relative to 0x180000, not PATCH_BASE.
    original_table = checked_range(firmware, ORIGINAL_FP_START,
                                   ORIGINAL_FP_END - ORIGINAL_FP_START)
    records = list(struct.iter_unpack("<II", original_table))
    if any(source & 7 or not FP_DATA_START <= source < FP_DATA_END
           for _, source in records):
        raise ValueError("unexpected original Flashpatch data pool")
    fp_source = max(source for _, source in records) + 8
    fp_table = original_table
    hooks = []
    occupied_targets = {target & ~7 for target, _ in records}
    for address, symbol, kind, expected in ROM_HOOKS:
        fp_target = address & ~7
        if (fp_source + 8 > FP_DATA_END or fp_target in occupied_targets or
                address + 4 > fp_target + 8):
            raise ValueError("no unused Flashpatch slot, target conflict or split instruction")
        if kind == "call":
            if thumb_bl(rom, address) != expected:
                raise ValueError(f"changed ROM call at {address:#x}")
        elif rom[address:address + 4] != expected:
            raise ValueError(f"changed ROM function entry at {address:#x}")
        target = symbols[symbol]
        branch = encode_thumb_bl(address, target)
        verification = bytearray(rom)
        verification[address:address + 4] = branch
        if thumb_bl(verification, address) != target:
            raise ValueError(f"invalid ROM hook branch at {address:#x}")
        if kind == "tail":
            first, second = struct.unpack("<HH", branch)
            branch = struct.pack("<HH", first, second & ~0x4000) # B.W, preserve LR
        block = bytearray(rom[fp_target:fp_target + 8])
        block[address - fp_target:address - fp_target + 4] = branch
        hooks.append({"symbol": symbol, "kind": kind, "address": hex(address),
                      "target": hex(fp_target), "source": hex(fp_source),
                      "before": rom[fp_target:fp_target + 8].hex(), "after": bytes(block).hex()})
        fp_table += struct.pack("<II", fp_target, fp_source)
        occupied_targets.add(fp_target)
        fp_source += 8
    fp_table_start = (PATCH_BASE + len(payload) + 7) & ~7
    fp_table_end = fp_table_start + len(fp_table)
    if fp_table_end > PATCH_END:
        raise ValueError("code and Flashpatch table exceed reserved memory")
    patched = bytearray(firmware)
    changes = []

    def replace(address, old, new, purpose):
        if len(old) != len(new) or checked_range(patched, address, len(old)) != old:
            raise ValueError(f"patch preimage mismatch at {address:#x}")
        offset = address - RAM_BASE
        patched[offset:offset + len(new)] = new
        changes.append({"address": hex(address), "before": old.hex(),
                        "after": new.hex(), "purpose": purpose})

    replace(HEAP_END_LITERAL, struct.pack("<I", PATCH_END), struct.pack("<I", PATCH_BASE),
            "exclude patch code from the initial low-memory heap")
    replace(EXEC_START_LITERAL, struct.pack("<I", PATCH_END), struct.pack("<I", PATCH_BASE),
            "extend the existing executable read-only MPU range over the patch")
    # A2 allocates two TX fragment pools at boot (116-byte payload plus
    # 92-byte lbuf). Native 0x1f7718 can additionally allocate a 204-byte
    # header plus a 60-byte lbuf for each outstanding fragment. Keeping
    # the stock 225+225 credits after reserving 56 KiB for this image lets
    # data exhaust the heap used by AWDL control events. Budget three
    # 64-frame windows: two in the shared pool, one in the reserve pool.
    # Native pool initialization/refill and credit accounting still own
    # all packets; do not mutate live freelists or borrow the RX pool.
    # These are capacity changes, not a proof of a hard control-memory
    # reserve: management/event allocations also need runtime acceptance.
    replace(0x211506, bytes.fromhex("e121"), bytes.fromhex("8021"),
            "budget shared TX fragment pool to 128 native credits")
    replace(0x211530, bytes.fromhex("e121"), bytes.fromhex("4021"),
            "budget reserve TX fragment pool to 64 native credits")
    # sendpkt sets tag bit 30 even when prepare_packet allocated a local
    # non-TXFRAG head. The slow builder then emits a long D11 header, but
    # the cache builder used that eligibility bit to select the short size.
    # Read the copied descriptor's actual short bit, as native crypto does:
    # ldrh r3,[r0,#2]; lsls r3,r3,#31, replacing tst r2,#0x40000000.
    # r3 is overwritten by the following IT/MOV pair. Keep r2 intact:
    # 0x1f80f4 still tests its packet-tag A-MSDU bit before rewriting QoS.
    # The following IT NE selects 0x28/0x88 for both MAC-header edits and
    # FrameLen. Preserve packet tags, cache, aggregation and DMA ownership.
    replace(0x1F80A2, bytes.fromhex("12f0804f"), bytes.fromhex("4388db07"),
            "derive cached D11 header size from its physical short-header bit")
    replace(PATCH_BASE, bytes(len(payload)), payload, "NAN Thumb implementations and native command table")
    replace(0x227D64, struct.pack("<I", 0x987F5),
            struct.pack("<I", symbols["awdl_peer_bar_complete"] | 1),
            "finish consumed AWDL BAR callbacks while retaining native handling for other peers")
    # The original RAM call sits across an 8-byte hardware Flashpatch boundary;
    # edit its image bytes directly, preserving all neighboring instructions.
    boot_call = 0x226476
    original_call = checked_range(firmware, boot_call, 4)
    if thumb_bl(original_call, 0) + boot_call != 0x2251B4:
        raise ValueError("changed WLC module attach caller")
    boot_branch = encode_thumb_bl(boot_call, symbols["nan_wlc_attach_modules"])
    if thumb_bl(boot_branch, 0) + boot_call != symbols["nan_wlc_attach_modules"]:
        raise ValueError("invalid WLC module attach branch")
    replace(boot_call, original_call, boot_branch, "attach NAN after original WLC module dependencies")
    auth_entry = 0x1CF5F8
    auth_before = bytes.fromhex("2de9f04f")
    auth_branch = encode_thumb_bl(auth_entry, symbols["nan_host_rx_auth"])
    first, second = struct.unpack("<HH", auth_branch)
    auth_branch = struct.pack("<HH", first, second & ~0x4000)
    replace(auth_entry, auth_before, auth_branch,
            "route NMI authentication to host; original RAM body retained via checked prologue")
    for hook in hooks:
        replace(int(hook["source"], 16), bytes(8), bytes.fromhex(hook["after"]),
                f"{hook['symbol']} {hook['kind']} hook in an unused hardware Flashpatch slot")
    replace(fp_table_start, bytes(len(fp_table)), fp_table,
            "original Flashpatch records plus the NAN hooks")
    for start, end in FP_TABLE_REFERENCES:
        replace(start, struct.pack("<I", ORIGINAL_FP_START), struct.pack("<I", fp_table_start),
                "relocated Flashpatch table start")
        replace(end, struct.pack("<I", ORIGINAL_FP_END), struct.pack("<I", fp_table_end),
                "relocated Flashpatch table end")
    for slot, (name, _, _) in CALLBACKS.items():
        target = symbols.get(name)
        if target is None or not PATCH_BASE <= target < PATCH_BASE + len(payload) or target & 1:
            raise ValueError(f"missing or invalid Thumb function: {name}")
        replace(slot, struct.pack("<I", UNIMPL), struct.pack("<I", target | 1), name)
    for slot, (name, _, _, target, _) in NATIVE_CALLBACKS.items():
        replace(slot, struct.pack("<I", UNIMPL), struct.pack("<I", target | 1), name)
    for address, name, original in RAM_KEY_HOOKS:
        target = symbols.get(name)
        if target is None or not PATCH_BASE <= target < PATCH_BASE + len(payload) or target & 1:
            raise ValueError(f"missing or invalid RAM hook target: {name}")
        branch = bytearray(encode_thumb_bl(address, target))
        half = struct.unpack_from("<H", branch, 2)[0] & ~0x4000
        struct.pack_into("<H", branch, 2, half)  # B.W keeps the caller's LR.
        if thumb_bl(branch, 0, link=False) + address != target:
            raise ValueError(f"invalid RAM hook branch at {address:#x}")
        replacement = bytes(branch) + bytes.fromhex("00bf") * ((len(original) - 4) // 2)
        replace(address, original, replacement, name + " checked native RAM entry")
    for address, name, expected in RAM_CALL_HOOKS:
        original = checked_range(firmware, address, 4)
        if thumb_bl(original, 0) + address != expected:
            raise ValueError(f"changed native RAM call at {address:#x}")
        target = symbols.get(name)
        if target is None or not PATCH_BASE <= target < PATCH_BASE + len(payload) or target & 1:
            raise ValueError(f"missing or invalid RAM call target: {name}")
        branch = encode_thumb_bl(address, target)
        if thumb_bl(branch, 0) + address != target:
            raise ValueError(f"invalid RAM call branch at {address:#x}")
        replace(address, original, branch, name + " checked native RAM call")
    # Everything outside the explicit replacement ranges must remain byte-exact.
    reconstructed = bytearray(patched)
    for change in reversed(changes):
        offset = int(change["address"], 16) - RAM_BASE
        old = bytes.fromhex(change["before"])
        reconstructed[offset:offset + len(old)] = old
    if bytes(reconstructed) != firmware:
        raise ValueError("reverse patch does not reproduce the original image")
    remaining = [entry for entry in inspection["unimplemented_nan_callbacks"]
                 if int(entry["slot"], 16) not in CALLBACKS and
                    int(entry["slot"], 16) not in NATIVE_CALLBACKS]
    manifest = {
        "artifact": "offline-nan-firmware-candidate", "native_nan_ready": False,
        "limitations": ["this build performs no hardware acceptance; runtime results must be bound to the exact output hash",
                        "hostap owns negotiation; autonomous firmware FSM/DAM paths remain disabled and retain the listed missing callbacks",
                        "GCMP hardware-key collision recovery and long-term stability are not verified",
                        "the inspected callback set is not the complete NAN dependency graph"],
        "input_sha256": FW_SHA256, "rom_sha256": ROM_SHA256,
        "source_sha256": source_hashes, "compiler": compiler,
        "linker_entrypoints": entrypoints,
        "functions": {name: hex(address) for name, address in symbols.items()},
        "payload_address": hex(PATCH_BASE), "payload_bytes": len(payload),
        "flashpatch": {"table_start": hex(fp_table_start), "table_end": hex(fp_table_end),
                       "original_records": len(records), "records": len(records) + len(hooks),
                       "hooks": hooks,
                       "table_sha256": digest(fp_table)},
        "reserved_used_bytes": fp_table_end - PATCH_BASE,
        "reserved_bytes": PATCH_END - PATCH_BASE,
        "output_sha256": digest(patched), "output_bytes": len(patched),
        "changes": changes, "remaining_checked_calls": remaining,
        "commands": commands,
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    # Deliberately not the production firmware filename; there is no installer.
    (output / "fw_bcm43752a2_nan.experimental.bin").write_bytes(patched)
    return {key: manifest[key] for key in ("artifact", "native_nan_ready", "payload_bytes",
                                          "reserved_bytes", "output_sha256")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware", type=Path, required=True, help="exact original A2 .146 image")
    parser.add_argument("--rom", type=Path, required=True, help="exact final safe [0, 0x160000) snapshot")
    parser.add_argument("--output", type=Path, required=True, help="new directory under .build")
    parser.add_argument("--tool-prefix", default="arm-none-eabi-", help="compiler/binutils prefix")
    args = parser.parse_args()
    try:
        print(json.dumps(build(args.firmware, args.rom, args.output, args.tool_prefix), indent=2))
    except subprocess.CalledProcessError as exc:
        parser.exit(1, f"build failed: {exc}\n{exc.stdout}{exc.stderr}")
    except (OSError, ValueError, struct.error) as exc:
        parser.exit(1, f"build failed: {exc}\n")


if __name__ == "__main__":
    main()

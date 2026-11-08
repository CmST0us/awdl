#ifndef BCM43752_NAN_HOST_ABI_H
#define BCM43752_NAN_HOST_ABI_H

/* Firmware / DHD / hostap wire contract. All multibyte fields are little
 * endian and accessed by offset, so neither native pointer size nor C packing
 * affects the transport. Version changes require both endpoints to agree.
 * This experimental transport alone does not advertise Wi-Fi Aware support.
 *
 * Header (24 bytes), followed by the complete 24-byte 802.11 management
 * header and its body when frame_length is nonzero:
 *   0: u16 version, 2: u16 kind, 4: u16 total_length, 6: u16 frame_length
 *   8: u32 cookie low, 12: u32 cookie high (opaque host correlation token)
 *  16: u16 Broadcom chanspec, 18: u16 flags, 20: s32 result
 *
 * TX requests: kind=TX, nonzero cookie, exact discovery chanspec, flags=0,
 * result=0. A successful command means queue ownership, not air completion.
 * Cancellation: kind=CANCEL, frame_length/chanspec/flags/result=0; the cookie
 * selects one queued frame. Submitted frames return busy and retain ownership.
 * Events use WLC_E_NAN_CRITICAL (158), reason 128. RX carries header+body and
 * chanspec; cookie/result=0. RX_DECRYPTED is set only after native integrity
 * and replay checks with an owned pairwise key or the sender's IGTK; ordinary
 * RX has flags=0. Verified group RX has its trailing MMIE removed.
 * TX_STATUS carries no frame, the same cookie
 * and requested chanspec, the native status word, and ACK only when present in
 * actual TX status. Negative results denote failure before hardware submission
 * or cancellation; they never claim delivery. Event delivery is not an ACK.
 * Protected TX carries plaintext header/body with FC Protected set and
 * Protected Dual category 9. Unicast requires an owned NMI pairwise key;
 * multicast requires the local IGTK. The request bit selects protection:
 * native BIP clears it on the group frame and appends a fresh MMIE. Exhausted
 * TX PN is rejected both before submission and at actual BIP preparation.
 * Group completion never carries ACK. No plaintext fallback is allowed.
 * Unprotected authentication and Public NAN discovery remain plain.
 * DW_START carries no frame/cookie/flags/result and reports the exact
 * discovery chanspec from a validated native DW callback. The original
 * A2 DW event contains timing and DW count, but no channel identity.
 */
#define BCM_NAN_HOST_VERSION 2
#define BCM_NAN_HOST_HEADER_SIZE 24
#define BCM_NAN_HOST_MAX_BODY 1500
#define BCM_NAN_HOST_MAX_FRAME (24 + BCM_NAN_HOST_MAX_BODY)
#define BCM_NAN_HOST_MAX_MESSAGE (BCM_NAN_HOST_HEADER_SIZE + BCM_NAN_HOST_MAX_FRAME)
#define BCM_NAN_HOST_TX_COMMAND 0xf001
#define BCM_NAN_HOST_CANCEL_COMMAND 0xf002
#define BCM_NAN_HOST_EVENT 158
#define BCM_NAN_HOST_REASON 128
#define BCM_NAN_HOST_TX 1
#define BCM_NAN_HOST_CANCEL 2
#define BCM_NAN_HOST_RX 3
#define BCM_NAN_HOST_TX_STATUS 4
#define BCM_NAN_HOST_DW_START 5
/* Last native beacon PCB released after REMOVE returned BUSY. No key data;
 * zero frame/flags/result, cookie = native key index + 1, current chanspec.
 * The host retries REMOVE; this event alone never releases ownership. */
#define BCM_NAN_HOST_KEY_DRAINED 6
#define BCM_NAN_HOST_ACK 1
#define BCM_NAN_HOST_RX_DECRYPTED 2
#define BCM_NAN_HOST_TX_LIMIT 16

/* NMI station command, 24 bytes: u16 version/op/length/flags at 0/2/4/6,
 * peer MAC at 8, bytes 14..23 zero. ADD is idempotent for the same flags;
 * REMOVE requires flags=0. MFP is the only supported flag. No association,
 * AP/STA mode or data station is created by this command.
 *
 * NMI pairwise key command, 64 bytes: u16 version/op/length/key_length at
 * 0/2/4/6, peer MAC at 8, u8 key_id/algo at 14/15, six-byte little-endian
 * initial receive PN at 16, bytes 22..31 zero, key bytes at 32 (zero-padded).
 * Key ID must be zero. Supported native algorithms are CCMP-128 (4, 16
 * bytes) and GCMP-256 (16, 32 bytes); REMOVE has zero algo/length/PN/key.
 * A failed install retains cleanup ownership; explicit REMOVE must succeed
 * before another install. Reinstallation never silently resets packet PN.
 * Both mutations refuse queued/in-flight frames to that peer. Native key
 * status is checked after the IOVAR; a successful command does not establish
 * hardware encryption or peer authentication. Keys are never returned.
 */
#define BCM_NAN_HOST_STATION_COMMAND 0xf003
#define BCM_NAN_HOST_KEY_COMMAND 0xf004
#define BCM_NAN_HOST_STATION_SIZE 24
#define BCM_NAN_HOST_KEY_SIZE 64
#define BCM_NAN_HOST_ADD 1
#define BCM_NAN_HOST_REMOVE 2
#define BCM_NAN_HOST_MFP 1
#define BCM_NAN_HOST_STATION_LIMIT 16

/* Local/peer schedule commands: fixed 1120 bytes, little endian.
 * u16 version/op/length/flags at 0/2/4/6; flags=0. Peer NMI at 8 (local:
 * all zero), bytes 14..23 zero. ADD replaces a complete committed map.
 * Peer REMOVE has a zero grid and waits for its NDP/SCB/TX owners to drain.
 * Local removal is owned by NAN STOP, not a separate empty schedule.
 * Grid at 24: u16 slot_count=512, period_tu=8192; u8 slot_tu=16, map_id 0..15,
 * sequence 0..255, reserved=0. Followed by 512 u16 native chanspecs and
 * a 64-byte NSS bitmap (bit i: one=2 NSS, zero=1 NSS). A zero chanspec
 * means unavailable and requires its NSS bit to be zero. Nonzero chanspecs
 * are committed 20/40/80 MHz slots. At least one must be committed.
 * Firmware expands this lossless wire format to its four-byte native slots;
 * the larger native grid must not be sent through the A2 control transport.
 * Firmware validates chanspecs against the actual CLM; the host must supply
 * the negotiated channel/NSS. No wildcard, potential or concurrent timeline.
 * The same grid drives native NAN Availability encoding and data scheduling.
 * Command failure never means rollback after an actual MSCH replacement;
 * retry or STOP must complete before reporting the schedule installed.
 */
#define BCM_NAN_HOST_LOCAL_SCHEDULE_COMMAND 0xf005
#define BCM_NAN_HOST_PEER_SCHEDULE_COMMAND 0xf006
#define BCM_NAN_HOST_SCHEDULE_SLOTS 512
#define BCM_NAN_HOST_SCHEDULE_PERIOD_TU 8192
#define BCM_NAN_HOST_SCHEDULE_SLOT_TU 16
#define BCM_NAN_HOST_SCHEDULE_GRID_SIZE (8 + 4 * BCM_NAN_HOST_SCHEDULE_SLOTS)
#define BCM_NAN_HOST_SCHEDULE_NSS_OFFSET (8 + 2 * BCM_NAN_HOST_SCHEDULE_SLOTS)
#define BCM_NAN_HOST_SCHEDULE_WIRE_GRID_SIZE (BCM_NAN_HOST_SCHEDULE_NSS_OFFSET + BCM_NAN_HOST_SCHEDULE_SLOTS / 8)
#define BCM_NAN_HOST_SCHEDULE_SIZE (24 + BCM_NAN_HOST_SCHEDULE_WIRE_GRID_SIZE)

/* Negotiated NDP station, 32-byte header plus a complete peer capability IE
 * stream (max 512 bytes). u16 version/op/length/flags at 0/2/4/6; peer NMI,
 * local NDI and peer NDI MACs at 8/14/20; nonzero u8 NDP ID at 26; byte 27
 * zero; u16 IE bytes at 28, bytes 30..31 zero. ADD requires MFP=1 and one
 * HT Capability IE; optional VHT/HE remain native PHY inputs. All three
 * addresses are unicast and distinct. Local NDI must already exist and the
 * peer must own a committed schedule. One NDP/SCB per peer is supported.
 * ADD is not a replacement: REMOVE must finish before another ADD. A failed
 * native create retains cleanup ownership. REMOVE repeats the identities,
 * flags=0 and no IEs; it waits for actual SCB/TX references and key deletion.
 * AUTHORIZE=3 and DEAUTHORIZE=4 use the same identities, flags=0 and no
 * IEs. AUTHORIZE requires an active NDP, native authenticated/associated MFP
 * SCB, usable RX/TX pairwise key and local TX GTK; all installed peer GTKs
 * must be usable. A peer GTK is optional when none was negotiated. The host
 * issues AUTHORIZE only after NDP confirmation and all negotiated key SETs
 * succeed. DEAUTHORIZE clears the native data-port state before waiting for
 * queued references. Key/station removal and STOP also revoke it first.
 * This binds a host-negotiated NDP; it does not run a firmware handshake.
 * GET_INACTIVITY=5 uses GET with those same identities, flags=0 and no IEs.
 * It returns exactly one little-endian u32: seconds since the native SCB's
 * last received data packet, using the firmware clock and unsigned wrap.
 * Missing, replaced or closing owners return an error, never an idle value.
 * The host's existing NDL policy owns expiry; this query changes no state.
 * NDI pairwise key: the same 64-byte key payload followed by local NDI MAC
 * at 64 and ten zero bytes. Peer address at 8 is the peer NDI, not its NMI.
 * Pairwise installation keeps the data port unauthorized. Group keys use
 * the separate command below. Only the explicit AUTHORIZE opens the port.
 */
#define BCM_NAN_HOST_DATA_STATION_COMMAND 0xf007
#define BCM_NAN_HOST_DATA_KEY_COMMAND 0xf008
#define BCM_NAN_HOST_DATA_STATION_SIZE 32
#define BCM_NAN_HOST_DATA_IE_MAX 512
#define BCM_NAN_HOST_DATA_KEY_SIZE 80
#define BCM_NAN_HOST_AUTHORIZE 3
#define BCM_NAN_HOST_DEAUTHORIZE 4
#define BCM_NAN_HOST_GET_INACTIVITY 5

/* Group key command, 64 bytes: u16 version/op/total/key_length at 0/2/4/6;
 * peer MAC at 8 (all zero for local TX), u8 key_id/algo at 14/15; six-byte
 * little-endian initial PN at 16; bytes 22..23 zero; local NMI/NDI at 24;
 * direction at 30 (1=local TX, 2=peer RX), byte 31 zero; key at 32, padded
 * to 32 bytes. ADD=1; REMOVE=2 and GET_SEQUENCE=3 require zero length,
 * algo, PN and key. GET_SEQUENCE uses GET, returns exactly six PN bytes;
 * mutations use SET. A group key never authorizes a data station.
 * ID 0..3: local NDI, CCMP-128 (4, 16 bytes) or GCMP-256 (16, 32 bytes).
 * ID 4/5 IGTK and 6/7 BIGTK: local NMI, BIP-CMAC-128 (13, 16 bytes) or
 * BIP-GMAC-256 (19, 32 bytes). RX requires an already owned MFP station.
 * Local and peer keys with the same ID have independent native objects/PN.
 * Only one local TX key per class (GTK/IGTK/BIGTK); peer RX may hold both
 * rotation IDs. REMOVE must complete before replacing any existing key.
 * Partial programming retains cleanup ownership and is never selected.
 * Key owners block station/NDI deletion; STOP drains keys before stations.
 * IGTK serves native group management TX/RX; BIGTK protects each new beacon
 * copy and verifies known peer beacons before native cluster processing.
 */
#define BCM_NAN_HOST_GROUP_KEY_COMMAND 0xf009
#define BCM_NAN_HOST_GROUP_KEY_SIZE 64
#define BCM_NAN_HOST_GROUP_KEY_LIMIT 64
#define BCM_NAN_HOST_GET_SEQUENCE 3

#endif

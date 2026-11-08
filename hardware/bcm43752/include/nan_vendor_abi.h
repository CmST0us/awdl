#ifndef BCM43752_NAN_VENDOR_ABI_H
#define BCM43752_NAN_VENDOR_ABI_H

/* Linux DHD / hostap transport, little endian, byte offsets rather than
 * packed C structs. No pointers, credentials or driver capabilities on wire.
 * NL80211_CMD_VENDOR uses the Broadcom OUI and this project's subcommand.
 * cfg80211's normal privileged command checks apply.
 *
 * Request (8 bytes + input): u16 version, command ID, operation, output limit.
 * Operation is GET or SET; SET requires output limit zero. GET limits the
 * returned command payload to 0..2048 bytes. Input is at most 2048 bytes.
 * Only one native version-2 NAN command is issued; SET's actual ioctl result
 * is returned, not an unchanged batch request mistaken for a reply.
 *
 * Reply (12 bytes + output): u16 version, command ID, output length, reserved
 * zero; s32 native firmware/IOVAR result at +8. Nonzero result has no output.
 * SET never returns input or key bytes. A zero result for TX only means the
 * firmware accepted queue ownership; actual completion is a separate event.
 * Netlink errors describe transport/validation failure, not firmware success.
 *
 * Event (20 bytes + native payload): u16 version, total length; u32 native
 * WLC event type, reason and status at +4/+8/+12; u8 firmware ifidx, bsscfgidx
 * at +16/+17, two reserved zero bytes. WLC fields are converted from network
 * order; payload is unchanged. Reason 128 payload uses nan_host_abi.h.
 * Event native payload is bounded to 4096 bytes; no fabricated completion.
 */
#define BCM_NAN_VENDOR_OUI 0x001018
#define BCM_NAN_VENDOR_SUBCMD 0x5741
#define BCM_NAN_VENDOR_VERSION 1
#define BCM_NAN_VENDOR_GET 1
#define BCM_NAN_VENDOR_SET 2
/* NDI uses operation 3, with command CREATE/DELETE/QUERY/QUIESCE/RESUME below. All
 * carry exactly 24 input bytes: MAC[6], reserved zero[2], Linux name[16].
 * CREATE requires a terminated exact name (no '%' template); DELETE/QUERY
 * accept an empty name or the exact retained name. Output limit must be 40.
 * The 40-byte status is returned even after a failed native operation:
 * u32 flags, Linux ifindex; u8 firmware ifidx/bssidx, MAC[6], name[16];
 * s32 last native IOVAR/ring result, s32 last host operation result.
 * Outer reply result zero means status available, NOT interface completion.
 * CREATED requires ADD_SEEN + REGISTERED, and no QUIESCING/DELETE_SENT/GONE/CONFLICT.
 * GONE requires the actual IF_DEL and successful Linux removal. A worker
 * finishes Linux registration/removal after a real firmware completion;
 * QUERY returns the retained snapshot and never retries native mutations.
 * A repeated CREATE returns its retained owner. DELETE retries an explicit
 * IOVAR failure, but an accepted outstanding delete is never reissued.
 * Tombstones remain queryable until reused by a later explicit CREATE.
 * QUIESCE stops new data and waits for real TX/PCIe ring completions (at
 * most 5 seconds for each phase, failure only). Use it before native peer,
 * key and NDP cleanup; DELETE also requires this phase. QUIESCED means no
 * host TX or active ring remains, not that the firmware BSS has been freed.
 * RESUME requires that same live registered owner, QUIESCED, no deletion,
 * and no remaining TX/ring ownership. It reopens host data after peer/key
 * cleanup while retaining other NDPs on the shared NDI. It never creates a
 * BSS or authorizes a station. Failed cleanup must not call RESUME.
 * Native WLC_E_IF events use the ordinary 20-byte envelope, reason=opcode,
 * status=0 or positive errno, payload[28]: native IF record[5], reserved[1],
 * MAC[6], firmware name[16]. They signal QUERY; no OS completion is invented.
 * Driver event type 0x80000001 reports an actual PCIe flow response, with
 * reason 1=create or 2=delete, native completion status, and payload MAC[6]
 * plus u16 ring ID. This also wakes host cleanup after a late completion.
 * Type 0x80000002 reports completed Linux register/remove work, reason
 * 1=register or 2=remove, status=positive errno or zero, payload=status[40].
 * It reports a real operation result, including failures, not a timer tick.
 */
#define BCM_NAN_VENDOR_NDI 3
#define BCM_NAN_NDI_CREATE 1
#define BCM_NAN_NDI_DELETE 2
#define BCM_NAN_NDI_QUERY 3
#define BCM_NAN_NDI_QUIESCE 4
#define BCM_NAN_NDI_RESUME 5
#define BCM_NAN_NDI_REQUEST_SIZE 24
#define BCM_NAN_NDI_STATUS_SIZE 40
#define BCM_NAN_NDI_IF_EVENT_SIZE 28
#define BCM_NAN_NDI_FLOW_EVENT 0x80000001u
#define BCM_NAN_NDI_HOST_EVENT 0x80000002u
#define BCM_NAN_NDI_OWNED 0x0001
#define BCM_NAN_NDI_INDEX_KNOWN 0x0002
#define BCM_NAN_NDI_ADD_SEEN 0x0004
#define BCM_NAN_NDI_REGISTERED 0x0008
#define BCM_NAN_NDI_DELETE_SENT 0x0010
#define BCM_NAN_NDI_DEL_SEEN 0x0020
#define BCM_NAN_NDI_GONE 0x0040
#define BCM_NAN_NDI_CONFLICT 0x0080
#define BCM_NAN_NDI_ORPHAN 0x0100
#define BCM_NAN_NDI_QUIESCING 0x0200
#define BCM_NAN_NDI_QUIESCED 0x0400
#define BCM_NAN_VENDOR_REQUEST_SIZE 8
#define BCM_NAN_VENDOR_REPLY_SIZE 12
#define BCM_NAN_VENDOR_EVENT_SIZE 20
#define BCM_NAN_VENDOR_MAX_DATA 2048
#define BCM_NAN_VENDOR_MAX_EVENT 4096

#endif

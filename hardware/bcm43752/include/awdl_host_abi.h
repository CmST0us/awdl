#ifndef BCM43752_AWDL_HOST_ABI_H
#define BCM43752_AWDL_HOST_ABI_H

/* BCM native AWDL control contract. Uses the existing privileged vendor
 * transport, with separate commands/owners from authenticated NAN NDPs.
 * All fields are little endian and serialized by offset, never native
 * structs. Reserved fields must be zero. Unsupported versions fail.
 *
 * Common header, 16 bytes:
 *   +0 u16 version, +2 u16 operation, +4 u16 total bytes, +6 u16 zero
 *   +8 u64 generation (nonzero, supplied by the host at START).
 * Every operation and event belongs to that exact generation. An old
 * completion must not mutate a new session, even if interface indices or
 * peer addresses have been reused. A failed START retains its generation
 * and any allocated resources until STOP completes.
 *
 * This defines implementation inputs, not an advertised capability. The
 * backend must query implemented firmware support before starting AWDL.
 */
#define BCM_AWDL_ABI_VERSION 1
#define BCM_AWDL_COMMAND 0xf00a
#define BCM_AWDL_EVENT_REASON 129
#define BCM_AWDL_HEADER_SIZE 16
#define BCM_AWDL_START 1
#define BCM_AWDL_STOP 2
#define BCM_AWDL_QUERY 3
#define BCM_AWDL_PEER 4
#define BCM_AWDL_PLAN 5
#define BCM_AWDL_WINDOW 6
#define BCM_AWDL_TX 7
#define BCM_AWDL_CANCEL_TX 8

/* START, 32 bytes: +16 local MAC[6], +22 u16 primary chanspec,
 * +24 u16 secondary chanspec (zero disables following), +26 u16 zero,
 * +28 u32 drain guard in microseconds. Only legal CLM channels within
 * the same 80 MHz block are accepted. Source MAC is an owned native NDI;
 * START neither enables STA/AP/P2P nor creates a monitor interface.
 * STOP/QUERY carry only the common header. STOP closes TX admission first,
 * drains submitted packets and flow rings, unregisters MSCH, removes the
 * peer, and then releases its BSS. Any failure retains actual ownership.
 */
#define BCM_AWDL_START_SIZE 32

/* QUERY response, 64 bytes: common header with QUERY and retained generation;
 * +16 u64 sampled firmware clock in microseconds, +24 u32 owner flags,
 * +28 s32 last failure, +32 u32 queued TX, +36 u32 submitted TX,
 * +40 local MAC[6], +46 peer MAC[6], +52 u16 current chanspec,
 * +54 u16 zero, +56 u32 completed TX, +60 u32 received actions.
 * A zero generation in QUERY requests the current snapshot (including idle)
 * only; all mutations require a nonzero matching generation. Flags describe
 * resources actually owned, including partial START and failed STOP states.
 * Host NDI registration and flow-ring drain remain separately tracked by
 * the existing DHD NDI status API. Firmware idle alone cannot certify that
 * the host netdev/rings have been released.
 */
#define BCM_AWDL_STATUS_SIZE 64
#define BCM_AWDL_OWNS_BSS 0x0001u
#define BCM_AWDL_OWNS_PEER 0x0002u
#define BCM_AWDL_OWNS_MSCH 0x0004u
#define BCM_AWDL_STOPPING 0x0008u
#define BCM_AWDL_ON_CHANNEL 0x0010u
#define BCM_AWDL_WINDOW_OPEN 0x0020u

/* PEER: +16 peer MAC[6], +22 u16 capability IE length, +24 complete IEs.
 * Length zero removes the current peer after its real TX drain. A new
 * peer cannot replace an owned one implicitly. Native HT/VHT negotiation
 * consumes actual peer IEs, never synthetic maximum-rate declarations.
 */
#define BCM_AWDL_PEER_HEADER_SIZE 24
#define BCM_AWDL_PEER_IE_MAX 512

/* PLAN, 64 bytes: +16 u64 firmware clock anchor in microseconds,
 * +24 sixteen u16 chanspecs (zero = unavailable), +56 u16 preceding and
 * +58 u16 following chanspec, +60 u32 zero. Each entry is 64 TU; period
 * is 1024 TU. Host control time is converted using a QUERY clock sample,
 * never wall time. WINDOW, 32 bytes: +16/+24 u64 open/close firmware times
 * in microseconds. Both zero closes the gate; otherwise close > open.
 * A native channel-open event AND an unexpired peer window are required
 * to admit data; command acceptance alone does not open the data path.
 */
#define BCM_AWDL_PLAN_SIZE 64
#define BCM_AWDL_PLAN_ENTRIES 16
#define BCM_AWDL_SLOT_TU 64
#define BCM_AWDL_WINDOW_SIZE 32

/* TX: +16 u64 nonzero cookie, +24 u16 chanspec, +26 u16 frame length,
 * +28 u64 firmware clock origin of the public controller,
 * +36 full 802.11 action frame. Validate source MAC, AWDL BSSID/category
 * and owned channel before allocation. Queue acceptance does not imply
 * transmission. CANCEL_TX contains only header+cookie, and returns busy
 * for a submitted frame until the native completion releases it.
 */
#define BCM_AWDL_TX_HEADER_SIZE 36
#define BCM_AWDL_TX_FRAME_MAX 1524
#define BCM_AWDL_CANCEL_TX_SIZE 24

/* Event header, 40 bytes: +0 version, +2 event kind, +4 total bytes,
 * +6 flags, +8 generation, +16 u64 firmware timestamp (microseconds),
 * +24 u64 cookie, +32 s32 native result, +36 u16 chanspec,
 * +38 u16 following payload bytes. RX payload is the whole action frame.
 * TX_DONE has no payload; ACK is set only from native unicast TX status.
 * STOPPED means all owned resources were released, not just a stop request.
 * Failed cleanup emits ERROR and keeps its generation available to QUERY
 * and repeated STOP. RX timestamps are captured before host event queues.
 */
#define BCM_AWDL_EVENT_HEADER_SIZE 40
#define BCM_AWDL_RX 1
#define BCM_AWDL_TX_DONE 2
#define BCM_AWDL_CHANNEL_OPEN 3
#define BCM_AWDL_CHANNEL_CLOSED 4
#define BCM_AWDL_STOPPED 5
#define BCM_AWDL_ERROR 6
/* Validated native NAN DW start: timestamp and discovery chanspec, no
 * payload/cookie/result. This firmware reserves 16 TU every 512 TU per
 * enabled discovery band (nan_mac configuration). It is a radio absence
 * notification, not an AWDL channel-open completion. */
#define BCM_AWDL_NAN_DW 7
/* Valid native unicast data addressed to this AWDL BSS from an unselected
 * sender. Payload is its six-byte unicast MAC; no cookie/result/channel.
 * This is a selection request, never authorization or a station replacement.
 * The host drains the current connection before selecting the sender. */
#define BCM_AWDL_SENDER 8
#define BCM_AWDL_TX_ACK 1

#endif

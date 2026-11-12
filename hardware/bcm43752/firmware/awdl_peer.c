#include "native_calls.h"
#include "awdl_peer.h"
#include "awdl_native.h"
#include "awdl_schedule.h"
#include "nan_data.h"
#include "nan_group.h"
#include "awdl_frame.h"
#include "awdl_event.h"

#define PTR(b, o) (*(void **)((uint8_t *)(b) + (o)))
#define U8(b, o) (*(uint8_t *)((uint8_t *)(b) + (o)))
#define U16(b, o) (*(uint16_t *)((uint8_t *)(b) + (o)))
#define U32(b, o) (*(uint32_t *)((uint8_t *)(b) + (o)))

/* Share the native byte-copy implementation with the NAN patch instead of
 * spending the fixed executable reservation on per-call byte loops. */
static void copy(void *to, const void *from, unsigned bytes)
{
    ((void *(*)(void *, const void *, unsigned))native_0x151a21)(to, from, bytes);
}

static int equal(const uint8_t *a, const uint8_t *b)
{
    return !((int (*)(const void *, const void *, unsigned))native_0xa7a9)(a, b, 6);
}

static void *lookup(struct awdl_session *s, const uint8_t *address)
{
    return ((void *(*)(void *, void *, const uint8_t *))native_0x123d05)(
        PTR(s->nan, 8), s->bss, address);
}

static int present(struct awdl_session *s)
{
    return s->scb && nan_data_owned_bss(s->nan, s->local, 0) == s->bss &&
        lookup(s, s->peer) == s->scb && PTR(s->scb, 0x0c) == s->bss;
}

void awdl_peer_bar_complete(void *wlc, unsigned status, void *ini)
{
    void *wrapper = PTR(wlc, 0x520);
    struct awdl_session *s = awdl_session_get(wrapper ? PTR(wrapper, 4) : 0);
    /* Registered packet callbacks are consumed unconditionally by 0x1f233e.
     * In particular, packet free (0x1869f0) invokes them with status zero
     * before releasing memory. The native BAR callback returns early for
     * these statuses, leaving an owner that can never receive another event.
     * Only finish that consumed reference for our SCB; retry through the
     * normal available-window path, never synthesize an ACK or send here.
     * Direct native calls to 0x987f4 retain their original behavior. */
    if (!(status & 0xf000u) && (!status || (status & 0x1cu)) &&
        s && s->scb && PTR(ini, 0x1c) == s->scb) {
        U8(ini, 0x14) = 0;
        if (U8(ini, 0) == 1)
            U8(ini, 0x16) = 1;
    }
    ((void (*)(void *, unsigned, void *))native_0x987f5)(wlc, status, ini);
}

void awdl_peer_recover(struct awdl_session *s)
{
    /* Called only after owned-channel OPEN succeeded in the serialized
     * firmware context. The session keeps its SCB until peer removal. */
    if (!s->scb || s->peer_closing ||
        !(U32(s->scb, 0) & 0x40000u))
        return;
    void *wlc = PTR(s->nan, 8);
    uint32_t tick = U32(PTR(wlc, 0), 0x28);
    if (s->aggregation_tick == tick)
        return;
    s->aggregation_tick = tick;
    void *ampdu = PTR(wlc, 0x78);
    void **slot = ((void **(*)(void *, unsigned))PTR(ampdu, 4))(
        s->scb, U32(ampdu, 8));
    /* Native 0x986c8 ages every TID, but gates BAR/DELBA recovery on the
     * channel available at that periodic instant. OPEN (0x125644) only
     * wakes data queues. Retry the same recovery on an actual AWDL window,
     * at most once per native watchdog second, without aging it again.
     * Re-read each TID: native helpers own its packets and may free it. */
    for (unsigned tid = 0; tid < 8; ++tid) {
        void *ini = PTR(*slot, 8 + 4 * tid);
        if (!ini)
            continue;
        unsigned state = U8(ini, 0);
        static const void *const handlers[] = {
            native_0x9752d, native_0x97ecd, native_0x981bd, native_0x97575,
        };
        if (state >= 4)
            continue;
        /* State 2 consumes r0..r2 and checks native elapsed time; the
         * extra caller-owned AAPCS arguments are ignored by that entry. */
        void (*recover)(void *, void *, void *, unsigned, unsigned, unsigned) =
            (void *)handlers[state];
        recover(wlc, s->scb, ini, tid, 1, 0);
    }
}

int awdl_peer_available(void *nan, void *scb, unsigned mode, uint16_t *channel)
{
    struct awdl_session *s = awdl_session_get(nan);
    if (!s || (s->scb != scb && PTR(s->bss, 0x38) != scb))
        return ((int (*)(void *, void *, unsigned, uint16_t *))native_0x36f39)(
            nan, scb, mode, channel);
    uint64_t now;
    int error = awdl_native_clock(nan, &now);
    int available = !error && !s->stopping && !s->peer_closing && present(s) &&
        awdl_timing_can_send(&s->timing, now);
    if (channel)
        *channel = available ? s->timing.active : 0;
    if (error)
        s->error = error;
    return available;
}

void awdl_peer_reinit(void *nan, void *bss, void *scb)
{
    struct awdl_session *s = awdl_session_get(nan);
    if (s && bss == s->bss && PTR(bss, 0x38) == scb)
        return;
    if (!s || s->scb != scb) {
        ((void (*)(void *, void *, void *))native_0x3779d)(nan, bss, scb);
        return;
    }
    if (!present(s) || bss != s->bss || !s->peer_ie_length)
        return;
    void *wlc = PTR(nan, 8), *manager = PTR(wlc, 0x678);
    void **slot = ((void **(*)(void *, unsigned))PTR(manager, 0x0c))(
        scb, U32(manager, 0x10));
    unsigned previous = U16(*slot, 4);
    unsigned channel = U16(PTR(bss, 0x98), 0x36);
    /* Both supported sidebands share the same VHT80 center and PHY
     * capabilities. Full 0x127cd4 re-negotiates HT/VHT/HE and resets rate
     * history through 0x1247c8 -> 0x110228 on every switch. Native saved
     * rate contexts are optional and absent on this slotted SCB.
     * Retain its channel and module updates; initial/other PHY changes
     * still negotiate below. Peer capability changes independently use
     * 0x127e1c in awdl_peer_set, including when the channel is unchanged. */
    if ((previous & ~0x700u) == (channel & ~0x700u)) {
        ((void (*)(void *, void *, unsigned, unsigned))native_0x124671)(
            wlc, scb, previous, channel);
        U16(*slot, 4) = channel;
        ((void (*)(void *, void *, void *, unsigned))native_0x11de55)(
            PTR(wlc, 0x714), bss, scb, 0);
        return;
    }
    /* Native PHY negotiation masks capability bytes in place. Retain the
     * original peer IEs for subsequent channel/rate reinitialization. */
    uint8_t ies[BCM_AWDL_PEER_IE_MAX];
    copy(ies, s->peer_ies, s->peer_ie_length);
    ((void (*)(void *, void *, unsigned, unsigned, unsigned))native_0x127cd5)(
        scb, ies, s->peer_ie_length, channel & 0x3800u, 0);
}

int awdl_peer_set(struct awdl_session *s, const uint8_t *address,
                   const uint8_t *ies, unsigned length)
{
    /* The sole caller is the checked BCM_AWDL_PEER command dispatcher:
     * address, nonzero IE size and the complete IE stream are validated
     * before dispatch. Keep resource/state checks here with their owner. */
    if (!s || s->stopping || s->peer_closing || equal(address, s->local))
        return -2;
    int slot = nan_data_owned_slot(s->nan, s->local);
    if (slot < 0)
        return slot;
    if (s->scb && (!equal(address, s->peer) || !present(s)))
        return -16;
    if (!s->scb && lookup(s, address))
        return -16;
    copy(s->peer_ies, ies, length);
    s->peer_ie_length = length;
    copy(s->peer, address, 6);
    void *wlc = PTR(s->nan, 8);
    if (!s->scb) {
        int error = ((int (*)(void *, void *, const void *, const void *,
                              void **, int, unsigned))native_0x127b55)(
            PTR(wlc, 0x678), s->bss, address, s->local, &s->scb, slot, 1);
        if (error) {
            s->error = error;
            return error;
        }
    }
    if (!present(s))
        return -30;
    uint8_t scratch[BCM_AWDL_PEER_IE_MAX];
    copy(scratch, ies, length);
    ((void (*)(void *, void *, void *, unsigned, void *, unsigned, unsigned))native_0x127e1d)(
        PTR(wlc, 0x678), s->bss, s->scb, 1, scratch, length, 0);
    /* This port belongs to AWDL's unencrypted MAC transport. NAN peer
     * keys and its station authorization path are not modified. */
    ((void (*)(void *, void *, unsigned))native_0x123a7d)(wlc, s->scb, 0x13);
    return awdl_schedule_window(s);
}

int awdl_peer_remove(struct awdl_session *s)
{
    if (!s || !s->scb)
        return s ? 0 : -2;
    if (!present(s))
        return -30;
    s->peer_closing = 1;
    awdl_group_window(s);
    void *wlc = PTR(s->nan, 8);
    ((void (*)(void *, void *, unsigned))native_0x123821)(wlc, s->scb, 0x10);
    /* Unlike a temporary window close, peer teardown must discard buffered
     * data. With peer_closing set, native OPEN takes its unavailable-peer
     * flush path; retain the SCB until real packet references are released. */
    int error = nan_data_owned_channel(s->nan, s->local, s->timing.active, 1);
    if (error)
        return error;
    if (s->queued || s->submitted || U8(s->scb, 0x1d) != 1)
        return -16;
    error = nan_group_idle(s->nan, s->bss, s->scb);
    if (error)
        return error;
    unsigned freed = ((unsigned (*)(void *, void *))native_0x127c5d)(PTR(wlc, 0x678), s->scb);
    void *remaining = lookup(s, s->peer);
    if (remaining == s->scb) {
        if (!U8(s->scb, 0x1d))
            U8(s->scb, 0x1d) = 1;
        return -16;
    }
    if (!freed || remaining)
        return -30;
    s->scb = 0;
    s->peer_closing = 0;
    s->peer_ie_length = 0;
    for (unsigned i = 0; i < 6; ++i)
        s->peer[i] = 0;
    return 0;
}

void *awdl_peer_tx_owner(struct awdl_session *s, void *bss, const uint8_t *ethernet)
{
    if (!s || bss != s->bss || s->stopping || s->peer_closing ||
        !present(s) || !equal(ethernet + 6, s->local) ||
        (U8(s->scb, 0x1c) & 0x13u) != 0x13u)
        return 0;
    if (!(ethernet[0] & 1u))
        return equal(ethernet, s->peer) ? s->scb : 0;
    void *group = PTR(bss, 0x38);
    return group && PTR(group, 0x0c) == bss && (U8(group, 0x10) & 1u) ? group : 0;
}

void *awdl_peer_rx_owner(struct awdl_session *s, const uint8_t *header)
{
    static const uint8_t bssid[6] = BCM_AWDL_BSSID_BYTES;
    if (!s || s->stopping || s->peer_closing || !equal(header + 16, bssid) ||
        (!equal(header + 4, s->local) && !(header[4] & 1u)))
        return 0;
    unsigned fc = header[0] | (unsigned)header[1] << 8;
    unsigned type = fc & 12u;
    if ((fc & 0x4303u) || (type != 0 && type != 8))
        return 0;
    if (!equal(header + 10, s->peer)) {
        /* Same ownership rule as BE200 UNEXPECTED_FRAME: notify only for
         * data addressed to our NDI, not multicast discovery, null data,
         * management or another receiver. Never manufacture an SCB here. */
        static const uint8_t zero[6];
        if (type == 8 && !(fc & 0x40u) && !(header[10] & 1u) &&
            !equal(header + 10, zero) && !equal(header + 10, s->local) &&
            equal(header + 4, s->local)) {
            uint64_t now;
            int error = awdl_native_clock(s->nan, &now);
            if (!error)
                error = awdl_event_emit(s, BCM_AWDL_SENDER, now, 0, 0, 0, 0,
                                        header + 10, 0, 0);
            if (error)
                s->error = error;
        }
        return 0;
    }
    return present(s) ? s->scb : 0;
}

/* NAN's 0x2259c opens/closes every NDI multicast flow ring from the NMI
 * discovery schedule. Our NDI has its own AWDL windows, so that global
 * notification must not admit its group traffic on the discovery channel.
 * Keep native PCIe ring ownership and its actual flow-control mechanism. */
static unsigned group_ring(struct awdl_session *s)
{
    unsigned index;
    return nan_data_owned_bss(s->nan, s->local, &index) == s->bss
        ? U16(PTR(PTR(s->nan, 0x30), 0x20), index * 2) : 0xffff;
}

NATIVE_ENTRY static void original_group_state(void *, unsigned, unsigned)
{
    __asm__ volatile("push {r0,r1,r4,lr}\nldr r4,=0x1285cc\nldr r4,[r4]\n"
                     "ldr pc,=0x128595");
}

void awdl_group_ring_state(void *manager, unsigned ring, unsigned state)
{
    void *wrapper = PTR(PTR(manager, 0), 0x520);
    struct awdl_session *s = awdl_session_get(wrapper ? PTR(wrapper, 4) : 0);
    if (!s || ring != group_ring(s) || (state != 0x1a && state != 0x1b))
        original_group_state(manager, ring, state);
}

void awdl_group_window(struct awdl_session *s)
{
    unsigned ring = group_ring(s);
    if (ring != 0xffff)
        original_group_state(PTR(PTR(s->nan, 8), 0x678), ring,
            awdl_peer_available(s->nan, PTR(s->bss, 0x38), 0, 0) ? 0x1a : 0x1b);
}

int awdl_group_ring_owner(void *nan, unsigned slot, unsigned ring,
                          const uint8_t *address, unsigned created)
{
    int error = ((int (*)(void *, unsigned, unsigned, const void *, unsigned))
        native_0x229e1)(nan, slot, ring, address, created);
    struct awdl_session *s = awdl_session_get(nan);
    /* A new ring starts before the next MSCH callback. Apply its current
     * window immediately; on deletion the native owner already cleared it. */
    if (!error && s && created && (address[0] & 1u) && ring == group_ring(s))
        awdl_group_window(s);
    return error;
}

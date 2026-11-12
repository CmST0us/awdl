#include "native_calls.h"
#include "awdl_schedule.h"
#include "awdl_native.h"
#include "awdl_event.h"
#include "awdl_tx.h"
#include "awdl_peer.h"
#include "nan_scheduler.h"
#include "nan_data.h"

#define PTR(b, o) (*(void **)((uint8_t *)(b) + (o)))
#define U32(b, o) (*(uint32_t *)((uint8_t *)(b) + (o)))
#define U16(b, o) (*(uint16_t *)((uint8_t *)(b) + (o)))
#define U64(b, o) (*(uint64_t *)((uint8_t *)(b) + (o)))

/* Only the two NAN data-slot broadcast call sites enter here. AWDL has
 * its own MSCH callback and must not be opened/closed by the ordinary NAN
 * availability map. Leave other NDIs and the NMI notification unchanged. */
int awdl_nan_channel(void *manager, unsigned index, unsigned channel, unsigned event)
{
    void *wlc = manager ? PTR(manager, 0) : 0;
    void *wrapper = wlc ? PTR(wlc, 0x520) : 0;
    struct awdl_session *s = awdl_session_get(wrapper ? PTR(wrapper, 4) : 0);
    if (s && ((void *(*)(void *, unsigned))native_0x1279d1)(manager, index) == s->bss)
        return 0;
    return ((int (*)(void *, unsigned, unsigned, unsigned))native_0x1278b5)(
        manager, index, channel, event);
}

NATIVE_ENTRY void awdl_nan_channel_close(void)
{
    /* The original BL at 0x22726 spans two TCAM blocks. Replace the
     * preceding four-byte manager load, then resume after that BL. */
    __asm__ volatile(
        "ldr.w r0,[r7,#0x678]\n"
        "push {r4,lr}\nbl awdl_nan_channel\npop {r4,lr}\n"
        "ldr pc,=0x2272b\n");
}

/* A2 HRT removes the timeout and clears its callback before invoking it
 * (0xda8c8). Cancellation unlinks it synchronously (0xda9ec). All paths
 * execute on the serialized firmware context; no detached callback owner. */
static void window_event(void *context)
{
    /* The common path records both admission and cleanup failures. */
    awdl_schedule_window(context);
}

uint64_t awdl_schedule_deadline(struct awdl_session *s, uint64_t now)
{
    void *manager = PTR(PTR(s->nan, 8), 0x574);
    void *current = PTR(manager, 0x60);
    if (!current ||
        U16(PTR(current, 0x28), 8) != s->timing.active)
        return 0;
    /* A2 0xf6130 constructs physical slots; 0xf2728 publishes current
     * before ON_CHAN. Callback timing instead describes request entities.
     * Inspect next afresh at the drain timer: it may be allocated only
     * after ON_CHAN. Adjacent slots in the identical native channel context
     * need no physical drain between them. This does not synthesize OPEN
     * or suppress SLOT_END/OFF_CHAN, which still control ownership. */
    uint64_t end = U64(current, 0x10);
    void *next = PTR(manager, 0x64);
    if (next && PTR(next, 0x28) == PTR(current, 0x28) &&
        U64(next, 8) == end)
        end = U64(next, 0x10);
    /* Also arm a timer for the first different public slot. Merely testing
     * that boundary on packet admission leaves already-open native queues
     * running until another peer update or MSCH callback arrives. */
    return awdl_timing_deadline(&s->timing, now, end);
}

static int window_update(struct awdl_session *s)
{
    if (s->window_timer)
        ((void (*)(void *))native_0xda9ed)(s->window_timer);
    if (s->stopping || !s->timing.on_channel)
        return 0;
    uint64_t now;
    int error = awdl_native_clock(s->nan, &now);
    if (error)
        return error;
    s->timing.channel_close = awdl_schedule_deadline(s, now);
    if (s->timing.close < s->timing.channel_close)
        s->timing.channel_close = s->timing.close;
    /* Native OPEN with an unavailable SCB is destructive: 0x128690 filters
     * and frees its software queues (0x133a64 -> 0x135170 -> 0x12894), then
     * flushes aggregation. A scheduled absence must use native CLOSE,
     * which stops admission while preserving queued packets for reopening.
     * Before a peer exists, OPEN initializes the BSS channel for discovery.
     */
    s->bound_channel = s->timing.active;
    int available = awdl_timing_can_send(&s->timing, now);
    error = nan_data_owned_channel(s->nan, s->local, s->bound_channel,
        !s->scb || available);
    if (error)
        return error;
    if (available)
        awdl_peer_recover(s);
    uint64_t deadline = s->timing.open;
    if (now >= deadline) {
        deadline = s->timing.channel_close;
        if (deadline <= s->timing.drain_us)
            return 0;
        deadline -= s->timing.drain_us;
    }
    if (deadline <= now)
        return 0;
    if (!s->window_timer)
        s->window_timer = ((void *(*)(void *))native_0x1f41d)(s->nan);
    if (!s->window_timer)
        return -27;
    uint64_t delay = deadline - now;
    if (delay > UINT32_MAX)
        delay = UINT32_MAX;
    return ((int (*)(void *, unsigned, uintptr_t, void *))native_0xda949)(
        s->window_timer, (unsigned)delay, (uintptr_t)window_event, s) ? 0 : -16;
}

int awdl_schedule_window(struct awdl_session *s)
{
    int error = window_update(s);
    if (!error)
        awdl_group_window(s);
    if (error) {
        s->error = error;
        /* A failed timer arm must not leave an admitted queue without a
         * closing event. Remove owns any partially changed native bind. */
        awdl_schedule_remove(s);
    }
    return error;
}

/* A2 MSCH callback layout is the native 16-byte record consumed at 0x37ce0:
 * u32 event bits, u16 chanspec, u16 padding, request pointer, timing pointer.
 * Event bits are also declared in the pinned driver's event_log_payload.h.
 */
static int channel_event(struct awdl_session *s, const void *event)
{
    if (!s || !event || awdl_session_get(s->nan) != s)
        return -2;
    unsigned bits = U32(event, 0);
    uint16_t channel = U16(event, 4);
    uint64_t now;
    int error = awdl_native_clock(s->nan, &now);
    if (error) {
        s->error = error;
        s->timing.on_channel = 0;
        return error;
    }
    /* SLOT_END/OFF_CHAN/SKIP close admission before any subsequent work.
     * REQ_END is a callback, not proof that unregister freed the handle. */
    if (bits & (0x08u | 0x10u | 0x20u | 0x40u | 0x80u)) {
        awdl_timing_channel(&s->timing, channel, 0);
        if (bits & 0x80u)
            s->timing.on_channel = 0;
        awdl_group_window(s);
        if (!s->timing.on_channel && s->window_timer)
            ((void (*)(void *))native_0xda9ed)(s->window_timer);
        if (s->bound_channel && (s->bound_channel == channel || (bits & 0x80u))) {
            error = nan_data_owned_channel(s->nan, s->local, s->bound_channel, 0);
            if (!error)
                s->bound_channel = 0;
        }
        int emitted = awdl_event_emit(s, error ? BCM_AWDL_ERROR : BCM_AWDL_CHANNEL_CLOSED, now, 0, error,
                                      channel, 0, 0, 0, 0);
        if (!error)
            error = emitted;
    }
    if (!error && !s->stopping && (bits & (0x02u | 0x04u))) {
        if (!channel || (channel != s->timing.primary && channel != s->timing.secondary)) {
            s->error = -2;
            s->timing.on_channel = 0;
            awdl_event_emit(s, BCM_AWDL_ERROR, now, 0, -2, channel, 0, 0, 0, 0);
            return -2;
        }
        /* Native NAN slot-start updates its BCMC SCB band before opening
         * queues (0x301a4 -> 0x1284bc). Our independently scheduled NDI
         * needs the same update: it was allocated on the discovery band.
         * Otherwise 0x1f9a92 drops every action at its band check and the
         * packet-free PCB reports zero despite no DMA submission. */
        ((void (*)(void *, unsigned))native_0x301a5)(s->bss, channel);
        awdl_timing_channel(&s->timing, channel, 1);
        error = awdl_schedule_window(s);
        if (error) {
            s->timing.on_channel = 0;
            awdl_event_emit(s, BCM_AWDL_ERROR, now, 0, error, channel, 0, 0, 0, 0);
            return error;
        }
        error = awdl_event_emit(s, BCM_AWDL_CHANNEL_OPEN, now, 0, 0,
                                channel, 0, 0, 0, 0);
        awdl_tx_kick(s);
    }
    if (error)
        s->error = error;
    return error;
}

int awdl_schedule_remove(struct awdl_session *s)
{
    if (!s || awdl_session_get(s->nan) != s)
        return -30;
    s->timing.on_channel = 0;
    awdl_group_window(s);
    if (s->window_timer)
        ((void (*)(void *))native_0xda9ed)(s->window_timer);
    if (s->bound_channel) {
        int error = nan_data_owned_channel(s->nan, s->local, s->bound_channel, 0);
        if (error) {
            s->error = error;
            return error;
        }
        s->bound_channel = 0;
    }
    if (!s->msch)
        return 0;
    void *wlc = PTR(s->nan, 8);
    int error = nan_msch_unregister_owned(PTR(wlc, 0x574), &s->msch);
    if (error)
        s->error = error;
    return error;
}

/* Keep the native channel-array frame out of the command dispatcher. */
__attribute__((noinline)) int awdl_schedule_install(struct awdl_session *s, const uint8_t *payload, unsigned length)
{
    if (!s || s->stopping || awdl_session_get(s->nan) != s)
        return -16;
    struct awdl_timing next = s->timing;
    int error = awdl_timing_plan(&next, payload, length);
    if (error)
        return error;
    uint64_t now;
    error = awdl_native_clock(s->nan, &now);
    if (error)
        return error;
    const uint64_t period = (uint64_t)BCM_AWDL_PLAN_ENTRIES * BCM_AWDL_SLOT_TU * 1024;
    const uint64_t slot = BCM_AWDL_SLOT_TU * 1024u;
    /* The public controller refreshes its rolling view every eight slots,
     * with up to one TU of clock correction. Compare against the installed
     * epoch, not the last command: small corrections must not repeatedly
     * cancel a live request, and accumulated drift must still replace it.
     * The peer window and three-TU transition guards remain authoritative. */
    if (s->msch && s->timing.planned) {
        uint64_t difference = next.epoch >= s->timing.epoch ?
            next.epoch - s->timing.epoch : s->timing.epoch - next.epoch;
        /* Validated epochs leave a full period below UINT64_MAX. Bias
         * by the tolerance so either side of a boundary shares one test. */
        uint64_t biased = difference + 1024;
        if (biased % slot <= 2048) {
            unsigned shift = (unsigned)((biased / slot) % BCM_AWDL_PLAN_ENTRIES);
            if (next.epoch < s->timing.epoch)
                shift = (BCM_AWDL_PLAN_ENTRIES - shift) % BCM_AWDL_PLAN_ENTRIES;
            unsigned same = 1;
            for (unsigned i = 0; i < BCM_AWDL_PLAN_ENTRIES; ++i)
                if (next.channels[i] != s->timing.channels[(i + shift) % BCM_AWDL_PLAN_ENTRIES])
                    same = 0;
            if (same)
                return 0;
        }
    }
    /* Keep the protocol's 64-TU slots, but submit native 16-TU scheduling
     * quanta, as the NAN data scheduler does. BOTH_FIXED admission needs
     * the entire quantum plus retunes to fit around a higher-priority DW;
     * a 16-TU DW crossing a 64-TU boundary must not discard two full AWs.
     * Each protocol slot is repeated four times without adding airtime. */
    const unsigned subdivisions = 4;
    const unsigned count = BCM_AWDL_PLAN_ENTRIES * subdivisions;
    const uint64_t quantum = slot / subdivisions;
    uint64_t start = next.epoch;
    unsigned rotation = 0;
    if (start < now) {
        /* Both quantum and map period divide 2^32. Low words suffice
         * for phase; retain full-width ordering and overflow detection. */
        uint32_t wait = ((uint32_t)start - (uint32_t)now) & (quantum - 1);
        start = now + wait;
        if (start < now)
            return -2;
        rotation = (((uint32_t)start - (uint32_t)next.epoch) / quantum) % count;
    }
    /* The native API copies both parameter and channel arrays into its
     * request/entities before returning (0xf1448 and 0xf19ac). */
    uint16_t channels[count];
    unsigned available = 0;
    unsigned resident = next.channels[0];
    for (unsigned i = 0; i < count; ++i) {
        uint16_t channel = next.channels[((i + rotation) % count) / subdivisions];
        channels[i] = channel ? channel : 0xffffu;
        available |= channel;
        if (channel != resident)
            resident = 0;
    }
    error = awdl_schedule_remove(s);
    if (error)
        return error;
    next.on_channel = 0;
    s->timing = next;
    if (!available)
        return 0;
    uint32_t params[12] = {0};
    /* A constant receive plan needs residency, not periodic ownership gaps.
     * BOTH_FLEX is the native continuous-channel request. A2 0xf20d4
     * forbids CHAN_CONTIGUOUS for that type; do not merely add MERGE to a
     * periodic map (0xf227c requires duration == interval for that flag).
     * Multi-channel plans retain BOTH_FIXED and their exact slot geometry.
     * Native OFF_CHAN and the physical-slot deadline still gate all TX. */
    params[0] = resident ? 4 : 5; /* Preemptable; fixed maps also contiguous. */
    params[1] = resident ? 3 : 0; /* BOTH_FLEX / BOTH_FIXED. */
    params[2] = 3; /* Above NAN data (2), below native discovery DW (4). */
    params[3] = (uint32_t)start;
    params[4] = (uint32_t)(start >> 32);
    params[5] = (uint32_t)quantum;
    params[6] = resident ? (uint32_t)quantum : (uint32_t)period;
    /* BOTH_FLEX grants physical spans independently of these recurring
     * request windows. Keep a nonzero interval: 0xf50b8 otherwise emits
     * REQ_END and 0xf345c removes the entity without rescheduling it.
     * Minimum dwell is 16 TU; 32 TU away budget covers a 16-TU NAN DW
     * and both retunes. Higher-priority native discovery retains control. */
    params[7] = (uint32_t)quantum;
    params[8] = 32 * 1024u; /* The flexible union is unused for BOTH_FIXED. */
    error = ((int (*)(void *, const uint16_t *, unsigned, uintptr_t, void *,
                       const uint32_t *, void **))native_0xf6fe5)(
        PTR(PTR(s->nan, 8), 0x574), channels, resident ? 1 : count,
        (uintptr_t)channel_event, s, params, &s->msch);
    /* Even an error may retain a handle: STOP owns its actual unregister. */
    if (error)
        s->error = error;
    return error;
}

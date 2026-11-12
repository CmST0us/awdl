#include "native_calls.h"
/* BCM43752 A2 host-scheduled NAN. Native 0x37f94 translates the four-byte
 * availability entries and registers MSCH; 0x37ce0 / 0x37b84 deliver its real
 * events. The generic module enable is intentionally empty in A2 and B0.
 * Restore the missing slot-start and unregister callbacks, and replace the
 * registration wrapper which halts on recoverable allocation/MSCH errors.
 */
#include <stddef.h>
#include <stdint.h>
#include "nan_scheduler.h"
#include "nan_mac.h"
#include "nan_host_abi.h"

#define FIELD(type, base, offset) (*(type *)((uint8_t *)(base) + (offset)))
#define PTR(base, offset) FIELD(void *, base, offset)
#define U32(base, offset) FIELD(uint32_t, base, offset)
#define U16(base, offset) FIELD(uint16_t, base, offset)
#define U8(base, offset) FIELD(uint8_t, base, offset)
#define INITIALIZED 0x58
#define LAST_ERROR 0x5c

static void *allocate(size_t bytes)
{
    return ((void *(*)(size_t))native_0x7146d)(bytes);
}

NATIVE_ENTRY void nan_msch_trace_allocate(void)
{
    /* A2 0xf8744 allocates a four-byte record header plus only 512 bytes.
     * Both native trace serializers copy the full channel list unchecked:
     * 512 chanspecs alone need 1024 bytes. Keep the original allocator and
     * ownership layout, with 2 KiB for the channels and record metadata. */
    /* Replace the size instruction; its following BL straddles two TCAM
     * slots. Return directly to that BL's original continuation. */
    __asm__ volatile("movw r0,#2052\nldr lr,=0xf87e3\nldr pc,=0x1526e1");
}

static int prepare_msch_trace(void *wlc)
{
    void *msch = PTR(wlc, 0x574);
    void *trace = msch ? PTR(msch, 0x98) : 0;
    void *state = trace ? PTR(trace, 0) : 0;
    if (!state || !PTR(state, 4))
        return 0; /* The allocation hook covers a later trace enable. */
    void *old = PTR(state, 0x18);
    if (!old || PTR(state, 4) != (uint8_t *)old + 4)
        return -30;
    /* Boot can already own the original 516-byte buffer before NAN init.
     * Drain its pending record through the native logger, then replace the
     * actual owner. No record is being built during this command callback. */
    void *replacement = ((void *(*)(size_t))native_0x1526e1)(2052);
    if (!replacement)
        return -27;
    ((void (*)(void *))native_0xf7ead)(trace);
    PTR(state, 0x18) = replacement;
    PTR(state, 4) = (uint8_t *)replacement + 4;
    ((void (*)(void *))native_0x71491)(old);
    return 0;
}

static void release_slot(void *owner, size_t offset)
{
    if (PTR(owner, offset)) {
        ((void (*)(void *))native_0x71491)(PTR(owner, offset));
        PTR(owner, offset) = 0;
    }
}

void nan_scheduler_mac_event(void *mac, unsigned event, unsigned channel)
{
    /* Keep the native cluster/clock work and all 20 MHz handling. A2 then
     * compares the raw chanspec low byte with the discovery primary. For
     * VHT80 that byte is the center channel (155 rather than primary 149),
     * so it leaves WLC's NAN-on-channel bit clear throughout the data slot. */
    ((void (*)(void *, unsigned, unsigned))native_0x2e4d9)(mac, event, channel);
    unsigned sideband = (channel >> 8) & 7u;
    if ((channel & 0xf800u) != 0xe000u || sideband >= 4 || !(event & 6u))
        return;
    unsigned primary = (channel & 255u) - 6 + 4 * sideband;
    unsigned discovery = U16(PTR(mac, 0x20), 0x54);
    if ((discovery & 0xf800u) != 0xd000u || primary != (discovery & 255u))
        return;
    void *wlc = PTR(PTR(mac, 4), 8);
    if (event & 2u)
        U32(wlc, 0x16c) |= 8u;
    else if (event & 4u)
        U32(wlc, 0x16c) &= ~8u;
}

static void *scheduler_wlc(void *nan)
{
    void *mac = PTR(nan, 0x14);
    if (!mac || !PTR(mac, 8) || !PTR(PTR(mac, 8), 0) ||
        !PTR(PTR(PTR(mac, 8), 0), 0))
        return 0;
    return ((void *(*)(void *, unsigned))native_0x2f37d)(mac, 0);
}

int nan_scheduler_context_attach(void *wlc, void *nan)
{
    if (!wlc || !nan || !PTR(wlc, 0) || !PTR(nan, 0x10) || !PTR(nan, 0x74))
        return -2;
    /* A2 has one physical WLC; +0x14 is schedule metadata, not a second
     * WLC pointer. Reject a different unit rather than overwrite it. */
    if (U32(PTR(wlc, 0), 0x10))
        return -23;
    if (PTR(nan, 0x24))
        return -16;
    void *scheduler = allocate(0x60); /* Native 0x58 + private ownership/error. */
    if (!scheduler)
        return -27;
    PTR(scheduler, 0) = nan;
    PTR(scheduler, 0x44) = allocate(0x38);
    if (!PTR(scheduler, 0x44))
        goto fail;
    PTR(scheduler, 0x10) = allocate(0x14);
    if (!PTR(scheduler, 0x10))
        goto fail;
    U16(scheduler, 0x32) = 0xffff; /* No last active slot. */
    U16(scheduler, 0x40) = 16; /* Native scheduler granularity, TU. */
    PTR(nan, 0x24) = scheduler;
    return 0;
fail:
    release_slot(scheduler, 0x44);
    ((void (*)(void *))native_0x71491)(scheduler);
    return -27;
}

int nan_scheduler_initialize(void *wlc, void *nan)
{
    if (!wlc || !nan || !PTR(nan, 0x24) || !PTR(nan, 0x10) || !PTR(nan, 0x74))
        return -2;
    void *scheduler = PTR(nan, 0x24);
    if (U32(scheduler, INITIALIZED))
        return -16;
    void *entry = (uint8_t *)PTR(nan, 0x74) + 3 * 12;
    if (U32(entry, 0) || U32(entry, 4) || U32(entry, 8))
        return -16;
    int error = prepare_msch_trace(wlc);
    if (error)
        return error;
    error = ((int (*)(void *, void *))native_0x38329)(wlc, nan);
    if (!error)
        U32(scheduler, INITIALIZED) = 1;
    return error;
}

int nan_msch_unregister_owned(void *msch, void **owner)
{
    if (!msch || !owner)
        return -2;
    void *handle = *owner;
    if (!handle)
        return 0;
    int (*contains)(void *, void *) = (int (*)(void *, void *))0xf3ecdu;
    if (!contains(msch, handle))
        return -30;
    /* Native unregister clears its argument before validating/freeing. */
    void *request = handle;
    int error = ((int (*)(void *, void **))native_0xf7115)(msch, &request);
    if (error)
        return error;
    if (request || contains(msch, handle))
        return -16;
    *owner = 0;
    return 0;
}

int nan_scheduler_unregister(void *nan)
{
    if (!nan || !PTR(nan, 0x24))
        return -2;
    void *scheduler = PTR(nan, 0x24);
    void *wlc = scheduler_wlc(nan);
    void *handle = PTR(scheduler, 8);
    if (wlc && PTR(wlc, 0x678) &&
        !((unsigned (*)(void *))native_0x127969)(PTR(wlc, 0x678)))
        return -16;
    if (handle) {
        if (!wlc || !PTR(wlc, 0x574) || !PTR(nan, 0x30))
            return -25;
        void *bss = ((void *(*)(void *, unsigned))native_0x2f045)(PTR(nan, 0x14), 0);
        if (!bss || !PTR(bss, 0x98))
            return -25;
        uint16_t channel = U16(PTR(bss, 0x98), 0x36);
        void *msch = PTR(wlc, 0x574);
        int error = nan_msch_unregister_owned(msch, &PTR(scheduler, 8));
        if (error)
            return error;
        /* The NAN data / MAC consumers close the actual active channel. */
        ((void (*)(void *, uint16_t, unsigned))native_0x37f45)(nan, channel, 4);
        ((void (*)(void *, unsigned, uint16_t, unsigned))native_0x2259d)(nan, 4, channel, 0x10);
        nan_scheduler_mac_event(PTR(nan, 0x14), 4, channel);
        ((void (*)(void *, unsigned, unsigned, unsigned))native_0x1f0e1)(nan, 0, 4, 0);
    }
    release_slot(scheduler, 0x1c);
    U8(scheduler, 0x0c) = 0;
    U8(scheduler, 0x38) = 0;
    U16(scheduler, 0x32) = 0xffff;
    U8(scheduler, 0x4c) = 0;
    U32(scheduler, LAST_ERROR) = 0;
    if (wlc)
        ((void (*)(void *, unsigned))native_0x22559)(nan, 0);
    return 0;
}

int nan_scheduler_deinitialize(void *wlc, void *nan)
{
    (void)wlc;
    if (!nan)
        return -2;
    void *scheduler = PTR(nan, 0x24);
    if (!scheduler)
        return 0;
    void *mac = PTR(nan, 0x14);
    if (PTR(scheduler, 8) || U8(scheduler, 0x0c) ||
        (mac && PTR(mac, 0x38) && U8(PTR(mac, 0x38), 0x4a)))
        return -16;
    void *entry = (uint8_t *)PTR(nan, 0x74) + 3 * 12;
    if (U32(entry, 0) || U32(entry, 4) || U32(entry, 8)) {
        if (U32(entry, 0) || PTR(entry, 4) != PTR(scheduler, 0x44) ||
            U32(entry, 8) != 0x08010038u)
            return -30;
        ((void (*)(void *, size_t))native_0x18660b)(entry, 12);
    }
    release_slot(scheduler, 0x1c);
    if (U32(scheduler, INITIALIZED))
        U32(PTR(nan, 0x10), 0x44) &= ~8u;
    U32(scheduler, INITIALIZED) = 0;
    return 0;
}

int nan_scheduler_context_detach(void *nan)
{
    if (!nan)
        return -2;
    int error = nan_scheduler_deinitialize(PTR(nan, 8), nan);
    if (error || !PTR(nan, 0x24))
        return error;
    void *scheduler = PTR(nan, 0x24);
    release_slot(scheduler, 0x10);
    release_slot(scheduler, 0x44);
    release_slot(nan, 0x24);
    return 0;
}

static int valid_geometry(unsigned slots, unsigned unit, unsigned period)
{
    return unit == BCM_NAN_HOST_SCHEDULE_SLOT_TU &&
           period >= 512 && period <= BCM_NAN_HOST_SCHEDULE_PERIOD_TU &&
           !(period & (period - 1)) && slots == period / unit;
}

static uint64_t period_start(void *nan, unsigned period)
{
    void *mac = PTR(nan, 0x14);
    uint32_t high, low;
    ((void (*)(void *, uint32_t *, uint32_t *))native_0x30245)(mac, &high, &low);
    unsigned dw = ((unsigned (*)(void *))native_0x2f08d)(mac);
    /* Same CDW origin as native 0x381c4. Autonomous map expansion can use
     * shorter periods; host-installed maps retain their 8192 TU period. */
    uint64_t start = ((uint64_t)high << 32) | low;
    uint32_t phase = (dw % (period / 512)) << 19;
    /* Shortly after firmware boot the preceding cluster-aligned period
     * can predate MSCH's clock origin. Use its next equivalent boundary;
     * unsigned underflow would register an effectively infinite future. */
    if (start < phase)
        start += period * 1024;
    return start - phase;
}

int nan_scheduler_register(void *nan, void *const *schedules, unsigned count)
{
    if (!nan || !PTR(nan, 0x24) || !schedules || count != 1 || !schedules[0])
        return -2;
    void *scheduler = PTR(nan, 0x24);
    void *schedule = schedules[0];
    unsigned slots = U16(schedule, 0), period = U16(schedule, 2);
    if (!valid_geometry(slots, U8(schedule, 4), period))
        return -2;
    if (!U32(scheduler, INITIALIZED))
        return -25;
    void *wlc = scheduler_wlc(nan);
    if (!wlc || !PTR(wlc, 0x574))
        return -25;
    int error = nan_scheduler_unregister(nan);
    if (error)
        return error;
    uint32_t params[12];
    ((void (*)(void *, size_t))native_0x18660b)(params, sizeof(params));
    params[0] = 5;
    /* Keep discovery DW at its native priority 4. The independently owned
     * AWDL map uses 3, so an idle NAN data map cannot pin it to channel 149.
     * Normal NAN data retains its own MSCH handle and resumes in free slots. */
    params[2] = 2;
    params[5] = BCM_NAN_HOST_SCHEDULE_SLOT_TU * 1024; /* Microseconds. */
    params[6] = period * 1024;
    uint64_t start = period_start(nan, period);
    uint64_t now = ((uint64_t (*)(void *))native_0xf0ddd)(PTR(wlc, 0x574));
    if (now > start && now - start > params[6] - params[5])
        start += params[6];
    params[3] = (uint32_t)start;
    params[4] = (uint32_t)(start >> 32);
    U32(scheduler, 0x28) = params[3];
    U32(scheduler, 0x2c) = params[4];
    U8(scheduler, 0x3e) = 1;
    U16(scheduler, 0x18) = slots;
    U16(scheduler, 0x16) = BCM_NAN_HOST_SCHEDULE_SLOT_TU;
    U8(scheduler, 0x15) = U8(schedule, 6);
    U8(scheduler, 0x14) = U8(schedule, 5);
    error = ((int (*)(void *, void *, unsigned, void *const *, unsigned))native_0x37f95)(
        nan, params, 2, schedules, count);
    if (error && !PTR(scheduler, 8)) {
        release_slot(scheduler, 0x1c);
        U8(scheduler, 0x0c) = 0;
        if (error == -30)
            error = 0; /* A genuinely empty availability map has no MSCH handle. */
    }
    U32(scheduler, LAST_ERROR) = (uint32_t)error;
    if (!error)
        ((void (*)(void *, unsigned))native_0x22559)(nan, 1);
    return error;
}

void nan_scheduler_slot_start(void *nan)
{
    if (!nan || !PTR(nan, 0x24))
        return;
    void *scheduler = PTR(nan, 0x24);
    if (!PTR(scheduler, 8))
        return;
    unsigned slots = U16(scheduler, 0x18), unit = U16(scheduler, 0x16);
    unsigned period = slots * unit;
    if (!valid_geometry(slots, unit, period)) {
        U32(scheduler, LAST_ERROR) = (uint32_t)-2;
        return;
    }
    uint64_t start = period_start(nan, period);
    uint32_t high = start >> 32, low = start;
    /* Compare equal CDW phases. Keep the native B0 correction bound of
     * one discovery interval: an MSCH callback can carry an adjacent slot
     * or period reference, which is not evidence of clock drift. */
    int32_t delta = (int32_t)(low - U32(scheduler, 0x28));
    uint32_t magnitude = delta < 0 ? 0u - (uint32_t)delta : (uint32_t)delta;
    if (magnitude < 1024 || magnitude >= 512 * 1024 - 1024)
        return;
    void *wlc = scheduler_wlc(nan);
    if (!wlc || !PTR(wlc, 0x574)) {
        U32(scheduler, LAST_ERROR) = (uint32_t)-25;
        return;
    }
    uint32_t params[12];
    ((void (*)(void *, size_t))native_0x18660b)(params, sizeof(params));
    params[3] = low;
    params[4] = high;
    if (PTR(wlc, 0x678) &&
        !((unsigned (*)(void *))native_0x127969)(PTR(wlc, 0x678))) {
        U32(scheduler, LAST_ERROR) = (uint32_t)-16;
        return;
    }
    int error = ((int (*)(void *, void *, void *, unsigned))native_0xf72f5)(
        PTR(wlc, 0x574), PTR(scheduler, 8), params, 1);
    U32(scheduler, LAST_ERROR) = (uint32_t)error;
    if (error)
        return;
    ((void (*)(void *, unsigned))native_0x37f69)(nan, 0);
    U32(PTR(scheduler, 0x44), 0)++;
}

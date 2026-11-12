#include "native_calls.h"
#include <stddef.h>
#include "awdl_session.h"
#include "awdl_native.h"
#include "nan_data.h"
#include "nan_mac.h"
#include "nan_station.h"
#include "nan_group.h"
#include "awdl_schedule.h"
#include "awdl_tx.h"
#include "awdl_peer.h"
#include "awdl_event.h"

#define PTR(base, offset) (*(void **)((uint8_t *)(base) + (offset)))

static void copy(void *to, const void *from, unsigned bytes)
{
    ((void *(*)(void *, const void *, unsigned))native_0x151a21)(to, from, bytes);
}

struct awdl_session *awdl_session_get(void *nan)
{
    return nan && PTR(nan, 0x14) ? PTR(PTR(nan, 0x14), NAN_MAC_AWDL) : 0;
}

int awdl_session_holds_bss(void *nan, void *bss)
{
    struct awdl_session *s = awdl_session_get(nan);
    return s && s->bss == bss;
}

int awdl_session_bind(void *nan, uint64_t generation, const uint8_t local[6],
                      uint16_t primary, uint16_t secondary, uint32_t drain_us)
{
    if (!nan || !generation || !local || !PTR(nan, 0x14) ||
        drain_us >= BCM_AWDL_SLOT_TU * 1024u)
        return -2;
    if (awdl_session_get(nan))
        return -16;
    int error = awdl_native_channels(nan, primary, secondary);
    if (error)
        return error;
    unsigned index;
    void *bss = nan_data_owned_bss(nan, local, &index);
    if (!bss)
        return -30;
    if (nan_station_bss_busy(nan, bss) || nan_group_idle(nan, bss, 0))
        return -16;
    struct awdl_session *s = ((void *(*)(unsigned))native_0x7146d)(sizeof(*s));
    if (!s)
        return -27;
    s->nan = nan;
    s->bss = bss;
    s->generation = generation;
    s->timing.primary = primary;
    s->timing.secondary = secondary;
    s->timing.drain_us = drain_us;
    copy(s->local, local, 6);
    /* Publish before the mutating native prepare. Failed preparation retains
     * the binding so neither NDI delete nor NAN detach can hide its owner. */
    PTR(PTR(nan, 0x14), NAN_MAC_AWDL) = s;
    /* Reprepare the owned NDI so native hardware programming sees the
     * AWDL BSSID. Editing only bss->BSSID after enable leaves AMT stale. */
    error = nan_data_unprepare(nan, local);
    if (!error)
        error = nan_data_enable(nan, bss, index);
    if (error) {
        s->error = error;
        s->stopping = 1;
    }
    return error;
}

int awdl_session_release(struct awdl_session *s)
{
    if (!s || awdl_session_get(s->nan) != s)
        return -30;
    awdl_timing_close(&s->timing);
    s->stopping = 1;
    int error = awdl_tx_drain(s);
    if (error)
        return error;
    error = awdl_schedule_remove(s);
    if (error)
        return error;
    error = awdl_peer_remove(s);
    if (error)
        return error;
    error = nan_data_unprepare(s->nan, s->local);
    if (error)
        return error;
    if (s->scb || s->msch || s->tx || s->queued || s->submitted)
        return -16;
    /* NDI remains owned by its original interface lifecycle. Releasing this
     * binding permits that lifecycle to stop/free the BSS; it does not claim
     * that the host interface or native BSS has already been deleted. */
    uint64_t now;
    error = awdl_native_clock(s->nan, &now);
    if (!error)
        error = awdl_event_emit(s, BCM_AWDL_STOPPED, now, 0, 0, 0, 0, 0, 0, 0);
    if (error)
        return error;
    if (s->window_timer)
        ((void (*)(void *, void *))native_0x1f43d)(s->nan, s->window_timer);
    PTR(PTR(s->nan, 0x14), NAN_MAC_AWDL) = 0;
    ((void (*)(void *))native_0x71491)(s);
    return 0;
}


int awdl_session_status(void *nan, uint64_t generation, uint8_t *out, unsigned size)
{
    if (!out || size < BCM_AWDL_STATUS_SIZE)
        return -14;
    struct awdl_session *s = awdl_session_get(nan);
    if (generation && (!s || s->generation != generation))
        return -30;
    uint64_t now;
    int error = awdl_native_clock(nan, &now);
    if (error)
        return error;
    /* Encode the complete little-endian ABI before copying to a possibly
     * unaligned destination. Explicit reserved fields keep all bytes owned. */
    struct {
        uint16_t version, operation, bytes, reserved;
        uint64_t generation, timestamp;
        uint32_t flags;
        int32_t error;
        uint32_t queued, submitted;
        uint8_t local[6], peer[6];
        uint16_t channel, reserved2;
        uint32_t completed, received;
    } status = {.version = BCM_AWDL_ABI_VERSION, .operation = BCM_AWDL_QUERY,
                .bytes = BCM_AWDL_STATUS_SIZE, .timestamp = now};
    _Static_assert(sizeof(status) == BCM_AWDL_STATUS_SIZE);
    if (s) {
        status.generation = s->generation;
        status.flags = (s->bss ? BCM_AWDL_OWNS_BSS : 0) |
            (s->scb ? BCM_AWDL_OWNS_PEER : 0) | (s->msch ? BCM_AWDL_OWNS_MSCH : 0) |
            (s->stopping ? BCM_AWDL_STOPPING : 0) |
            (s->timing.on_channel ? BCM_AWDL_ON_CHANNEL : 0) |
            (awdl_timing_can_send(&s->timing, now) ? BCM_AWDL_WINDOW_OPEN : 0);
        status.error = s->error;
        status.queued = s->queued;
        status.submitted = s->submitted;
        _Static_assert(offsetof(struct awdl_session, peer) ==
                       offsetof(struct awdl_session, local) + 6);
        copy(status.local, s->local, 12);
        status.channel = s->timing.active;
        status.completed = s->completed;
        status.received = s->received;
    }
    copy(out, &status, sizeof(status));
    return 0;
}

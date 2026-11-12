#include "awdl_timing.h"

#define SLOT_US ((uint64_t)BCM_AWDL_SLOT_TU * 1024u)

static uint16_t read16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint64_t read64(const uint8_t *p)
{
    /* A2 executes little-endian; byte access keeps unaligned ABI input safe. */
    uint64_t value;
    for (unsigned i = 0; i < sizeof(value); ++i)
        ((uint8_t *)&value)[i] = p[i];
    return value;
}

static int channel_valid(const struct awdl_timing *t, uint16_t channel)
{
    return !channel || channel == t->primary ||
        (t->secondary && channel == t->secondary);
}

int awdl_timing_plan(struct awdl_timing *t, const uint8_t *p, unsigned length)
{
    if (!t || !p || !t->primary ||
        length != BCM_AWDL_PLAN_SIZE - BCM_AWDL_HEADER_SIZE)
        return -2;
    for (unsigned i = 0; i < 18; ++i)
        if (!channel_valid(t, read16(p + 8 + i * 2)))
            return -2;
    if (p[44] || p[45] || p[46] || p[47])
        return -2;
    uint64_t epoch = read64(p);
    if (epoch > UINT64_MAX - SLOT_US * BCM_AWDL_PLAN_ENTRIES)
        return -2;
    for (unsigned i = 0; i < BCM_AWDL_PLAN_ENTRIES; ++i)
        t->channels[i] = read16(p + 8 + i * 2);
    t->epoch = epoch;
    t->planned = 1;
    return 0;
}

int awdl_timing_window(struct awdl_timing *t, const uint8_t *p, unsigned length)
{
    if (!t || !p || length != BCM_AWDL_WINDOW_SIZE - BCM_AWDL_HEADER_SIZE)
        return -2;
    uint64_t open = read64(p), close = read64(p + 8);
    if ((open || close) && close <= open)
        return -2;
    t->open = open;
    t->close = close;
    return 0;
}

void awdl_timing_channel(struct awdl_timing *t, uint16_t channel, int opened)
{
    if (!opened) {
        /* A late close for the preceding channel cannot close its successor. */
        if (t->active == channel)
            t->on_channel = 0;
        return;
    }
    t->active = channel;
    t->on_channel = channel && channel_valid(t, channel);
}

void awdl_timing_close(struct awdl_timing *t)
{
    t->open = t->close = 0;
    t->on_channel = 0;
    t->planned = 0;
}

uint16_t awdl_timing_slot(const struct awdl_timing *t, uint64_t now,
                         uint64_t *next_us)
{
    *next_us = 0;
    if (!t->planned)
        return 0;
    if (now < t->epoch) {
        *next_us = t->epoch;
        return 0;
    }
    uint64_t elapsed = now - t->epoch;
    uint64_t remaining = SLOT_US - (elapsed & (SLOT_US - 1));
    if (now > UINT64_MAX - remaining)
        return 0;
    *next_us = now + remaining;
    return t->channels[(elapsed / SLOT_US) & (BCM_AWDL_PLAN_ENTRIES - 1)];
}

int awdl_timing_can_send(const struct awdl_timing *t, uint64_t now)
{
    if (!t->on_channel || now < t->open || now >= t->close ||
        t->close - now <= t->drain_us)
        return 0;
    uint64_t boundary;
    uint16_t channel = awdl_timing_slot(t, now, &boundary);
    if (!channel || channel != t->active)
        return 0;
    /* START requires drain_us < SLOT_US. Only the immediate next slot can
     * therefore enter this drain interval. An equal next slot extends the
     * opportunity by a full slot; the peer-window bound was checked above.
     * Keep awdl_timing_slot's overflow check for that next boundary. */
    if (boundary - now > t->drain_us)
        return 1;
    return awdl_timing_slot(t, boundary, &boundary) == channel;
}

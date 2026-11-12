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
        channel == t->secondary;
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

uint64_t awdl_timing_deadline(const struct awdl_timing *t, uint64_t now,
                              uint64_t end)
{
    if (!t->planned || now < t->epoch || now >= end)
        return 0;
    /* The power-of-two plan period divides 2^32; its phase needs only
     * low clock bits after the full-width pre-epoch check above. */
    uint32_t elapsed = (uint32_t)now - (uint32_t)t->epoch;
    unsigned index = (unsigned)(elapsed / SLOT_US);
    unsigned step = SLOT_US - (elapsed & (SLOT_US - 1));
    /* A full cycle proves all future slots equal. Check the distance to
     * end before advancing, so even UINT64_MAX cannot wrap the boundary. */
    for (unsigned i = 0; i < BCM_AWDL_PLAN_ENTRIES; ++i) {
        if (t->channels[index & (BCM_AWDL_PLAN_ENTRIES - 1)] != t->active)
            return now;
        if (end - now <= step)
            break;
        now += step;
        step = SLOT_US;
        ++index;
    }
    return end;
}

int awdl_timing_can_send(const struct awdl_timing *t, uint64_t now)
{
    return t->on_channel && now >= t->open && now < t->channel_close &&
        t->channel_close - now > t->drain_us;
}

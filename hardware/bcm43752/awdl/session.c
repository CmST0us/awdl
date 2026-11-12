/* SPDX-License-Identifier: BSD-3-Clause */
#define _POSIX_C_SOURCE 200809L
#include "session.h"
#include "../include/nan_vendor_abi.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint64_t read_le(const uint8_t *p, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i)
        value |= (uint64_t)p[i] << (i * 8);
    return value;
}
static void write_le(uint8_t *p, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = (uint8_t)(value >> (i * 8));
}
static void header(struct bcm_awdl_session *s, uint8_t *p, unsigned op, size_t size)
{
    memset(p, 0, size);
    write_le(p, BCM_AWDL_ABI_VERSION, 2);
    write_le(p + 2, op, 2);
    write_le(p + 4, size, 2);
    write_le(p + 8, s->generation, 8);
}
static int command(struct bcm_awdl_session *s, uint8_t *p, size_t length,
                    uint8_t *output, size_t capacity, int *rejected)
{
    if (rejected)
        *rejected = 0;
    int32_t native = 0;
    size_t used = 0;
    int error = bcm_link_command(s->link, BCM_AWDL_COMMAND,
        capacity ? BCM_NAN_VENDOR_GET : BCM_NAN_VENDOR_SET,
        p, length, output, capacity, &used, &native);
    if (error || native)
        fprintf(stderr, "BCM AWDL: op=%u transport=%d native=%d\n",
                (unsigned)read_le(p + 2, 2), error, native);
    if (error)
        return error;
    if (rejected)
        *rejected = native != 0;
    /* BCME is a different number space from Linux errno. */
    switch (native) {
    case 0: return used == capacity ? 0 : -EPROTO;
    case -2: return -EINVAL;
    case -14: case -24: return -EMSGSIZE;
    case -16: return -EBUSY;
    case -22: return -ENOSPC;
    case -23: return -EOPNOTSUPP;
    case -25: return -ENETDOWN;
    case -27: return -ENOMEM;
    case -30: return -ENOENT;
    default: return -EIO;
    }
}
uint64_t bcm_awdl_monotonic_us(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return 0;
    return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}
uint16_t bcm_awdl_chanspec(unsigned channel)
{
    /* 5 GHz / 80 MHz, center 155; primary sideband 0 or 3. Native CLM
     * remains authoritative for the configured country's allowed use. */
    return channel == 149 ? 0xe09b : channel == 161 ? 0xe39b : 0;
}
int bcm_awdl_query(struct bcm_awdl_session *s, struct bcm_awdl_status *status)
{
    uint8_t request[BCM_AWDL_HEADER_SIZE], reply[BCM_AWDL_STATUS_SIZE];
    header(s, request, BCM_AWDL_QUERY, sizeof(request));
    /* Snapshot survives an uncertain START/STOP reply, including idle. */
    memset(request + 8, 0, 8);
    int error = command(s, request, sizeof(request), reply, sizeof(reply), NULL);
    if (error)
        return error;
    if (read_le(reply, 2) != BCM_AWDL_ABI_VERSION ||
        read_le(reply + 2, 2) != BCM_AWDL_QUERY ||
        read_le(reply + 4, 2) != sizeof(reply) || read_le(reply + 6, 2) ||
        read_le(reply + 54, 2))
        return -EPROTO;
    uint32_t flags = (uint32_t)read_le(reply + 24, 4);
    uint64_t generation = read_le(reply + 8, 8);
    if ((flags & ~(BCM_AWDL_OWNS_BSS | BCM_AWDL_OWNS_PEER | BCM_AWDL_OWNS_MSCH |
                   BCM_AWDL_STOPPING | BCM_AWDL_ON_CHANNEL | BCM_AWDL_WINDOW_OPEN)) ||
        (!generation && flags))
        return -EPROTO;
    *status = (struct bcm_awdl_status) {
        .generation = generation, .firmware_us = read_le(reply + 16, 8), .flags = flags,
        .error = (int32_t)read_le(reply + 28, 4),
        .queued = (uint32_t)read_le(reply + 32, 4),
        .submitted = (uint32_t)read_le(reply + 36, 4),
        .channel = (uint16_t)read_le(reply + 52, 2),
        .completed = (uint32_t)read_le(reply + 56, 4),
        .received = (uint32_t)read_le(reply + 60, 4),
    };
    memcpy(status->local, reply + 40, 6);
    memcpy(status->peer, reply + 46, 6);
    return 0;
}
int bcm_awdl_clock_sample(struct bcm_awdl_session *s, struct bcm_awdl_status *status)
{
    uint64_t before = bcm_awdl_monotonic_us();
    int error = bcm_awdl_query(s, status);
    uint64_t after = bcm_awdl_monotonic_us();
    if (error)
        return error;
    if (s->generation && status->generation != s->generation)
        return -ESTALE;
    if (!before || after < before || !status->firmware_us ||
        (s->clock_ready && status->firmware_us < s->firmware_origin))
        return -ERANGE;
    /* Firmware timestamps the QUERY between these monotonic samples. The
     * half round-trip bounds mapping error; callers must include it in the
     * timing guard instead of treating the midpoint as an exact clock. */
    s->uncertainty_us = (after - before + 1) / 2;
    s->host_origin = before + (after - before) / 2;
    s->firmware_origin = status->firmware_us;
    s->clock_ready = 1;
    return 0;
}
static int convert(uint64_t value, uint64_t from, uint64_t to, uint64_t *out)
{
    if (value >= from) {
        uint64_t difference = value - from;
        if (difference > UINT64_MAX - to)
            return -ERANGE;
        *out = to + difference;
    } else {
        uint64_t difference = from - value;
        if (difference > to)
            return -ERANGE;
        *out = to - difference;
    }
    return 0;
}
int bcm_awdl_to_firmware(const struct bcm_awdl_session *s, uint64_t host, uint64_t *out)
{
    return s->clock_ready ? convert(host, s->host_origin, s->firmware_origin, out) : -ENODATA;
}
int bcm_awdl_to_host(const struct bcm_awdl_session *s, uint64_t firmware, uint64_t *out)
{
    return s->clock_ready ? convert(firmware, s->firmware_origin, s->host_origin, out) : -ENODATA;
}
int bcm_awdl_start(struct bcm_awdl_session *s, const uint8_t local[6],
                   unsigned primary, unsigned secondary, uint32_t drain_us)
{
    uint8_t p[BCM_AWDL_START_SIZE];
    if (!s->generation || !bcm_awdl_chanspec(primary) ||
        (secondary && (!bcm_awdl_chanspec(secondary) || secondary == primary)))
        return -EINVAL;
    header(s, p, BCM_AWDL_START, sizeof(p));
    memcpy(p + 16, local, 6);
    write_le(p + 22, bcm_awdl_chanspec(primary), 2);
    write_le(p + 24, bcm_awdl_chanspec(secondary), 2);
    write_le(p + 28, drain_us, 4);
    return command(s, p, sizeof(p), NULL, 0, NULL);
}
int bcm_awdl_stop(struct bcm_awdl_session *s)
{
    uint8_t p[BCM_AWDL_HEADER_SIZE];
    if (!s->generation)
        return -EINVAL;
    header(s, p, BCM_AWDL_STOP, sizeof(p));
    /* Never clear generation on command receipt. Caller must observe idle
     * and complete the separately owned DHD ring/netdev cleanup. */
    return command(s, p, sizeof(p), NULL, 0, NULL);
}
int bcm_awdl_peer(struct bcm_awdl_session *s, const uint8_t mac[6], const void *ies, size_t size)
{
    uint8_t p[BCM_AWDL_PEER_HEADER_SIZE + BCM_AWDL_PEER_IE_MAX];
    if (size > BCM_AWDL_PEER_IE_MAX || (size && !ies))
        return -EINVAL;
    size_t length = BCM_AWDL_PEER_HEADER_SIZE + size;
    header(s, p, BCM_AWDL_PEER, length);
    memcpy(p + 16, mac, 6);
    write_le(p + 22, size, 2);
    if (size)
        memcpy(p + BCM_AWDL_PEER_HEADER_SIZE, ies, size);
    return command(s, p, length, NULL, 0, NULL);
}
int bcm_awdl_plan(struct bcm_awdl_session *s, uint64_t epoch,
                  const uint8_t channels[16], unsigned preceding, unsigned following)
{
    uint8_t p[BCM_AWDL_PLAN_SIZE];
    header(s, p, BCM_AWDL_PLAN, sizeof(p));
    write_le(p + 16, epoch, 8);
    for (unsigned i = 0; i < BCM_AWDL_PLAN_ENTRIES + 2; ++i) {
        unsigned channel = i < BCM_AWDL_PLAN_ENTRIES ? channels[i] :
                           i == BCM_AWDL_PLAN_ENTRIES ? preceding : following;
        uint16_t chanspec = bcm_awdl_chanspec(channel);
        if (channel && !chanspec)
            return -EINVAL;
        write_le(p + 24 + 2 * i, chanspec, 2);
    }
    return command(s, p, sizeof(p), NULL, 0, NULL);
}
int bcm_awdl_window(struct bcm_awdl_session *s, uint64_t open, uint64_t close)
{
    uint8_t p[BCM_AWDL_WINDOW_SIZE];
    if ((open || close) && close <= open)
        return -EINVAL;
    /* Both endpoints are already in the firmware clock domain. */
    header(s, p, BCM_AWDL_WINDOW, sizeof(p));
    write_le(p + 16, open, 8);
    write_le(p + 24, close, 8);
    return command(s, p, sizeof(p), NULL, 0, NULL);
}
int bcm_awdl_tx(struct bcm_awdl_session *s, uint64_t cookie, unsigned channel,
                uint64_t origin, const void *frame, size_t length, int *rejected)
{
    *rejected = 0;
    uint8_t p[BCM_AWDL_TX_HEADER_SIZE + BCM_AWDL_TX_FRAME_MAX];
    /* Management actions use the 20 MHz primary inside the active VHT80
     * block. PLAN/START carry the full data chanspec, TX does not. */
    uint16_t chanspec = bcm_awdl_chanspec(channel) ? (uint16_t)(0xd000u | channel) : 0;
    if (!cookie || !origin || !chanspec || !frame || length < 36 || length > BCM_AWDL_TX_FRAME_MAX)
        return -EINVAL;
    size_t size = BCM_AWDL_TX_HEADER_SIZE + length;
    header(s, p, BCM_AWDL_TX, size);
    write_le(p + 16, cookie, 8);
    write_le(p + 24, chanspec, 2);
    write_le(p + 26, length, 2);
    write_le(p + 28, origin, 8);
    memcpy(p + BCM_AWDL_TX_HEADER_SIZE, frame, length);
    return command(s, p, size, NULL, 0, rejected);
}
int bcm_awdl_cancel(struct bcm_awdl_session *s, uint64_t cookie)
{
    uint8_t p[BCM_AWDL_CANCEL_TX_SIZE];
    if (!cookie)
        return -EINVAL;
    header(s, p, BCM_AWDL_CANCEL_TX, sizeof(p));
    write_le(p + 16, cookie, 8);
    return command(s, p, sizeof(p), NULL, 0, NULL);
}

int bcm_awdl_decode_event(const struct bcm_awdl_session *s, const struct bcm_event *event,
                           struct bcm_awdl_event *out)
{
    if (event->reason != BCM_AWDL_EVENT_REASON)
        return 0;
    if (event->type != 158 || event->status || !event->data ||
        event->length < BCM_AWDL_EVENT_HEADER_SIZE)
        return -EPROTO;
    const uint8_t *p = event->data;
    if (read_le(p, 2) != BCM_AWDL_ABI_VERSION || read_le(p + 4, 2) != event->length ||
        read_le(p + 38, 2) != event->length - BCM_AWDL_EVENT_HEADER_SIZE)
        return -EPROTO;
    uint64_t generation = read_le(p + 8, 8);
    if (!generation)
        return -EPROTO;
    if (generation != s->generation)
        return 0;
    unsigned kind = (unsigned)read_le(p + 2, 2);
    unsigned flags = (unsigned)read_le(p + 6, 2);
    uint64_t cookie = read_le(p + 24, 8);
    int32_t result = (int32_t)read_le(p + 32, 4);
    uint16_t channel = (uint16_t)read_le(p + 36, 2);
    size_t length = event->length - BCM_AWDL_EVENT_HEADER_SIZE;
    if (kind < BCM_AWDL_RX || kind > BCM_AWDL_SENDER ||
        (flags & ~BCM_AWDL_TX_ACK) || (flags && kind != BCM_AWDL_TX_DONE) ||
        (kind == BCM_AWDL_TX_DONE ? !cookie : !!cookie))
        return -EPROTO;
    if (kind == BCM_AWDL_RX) {
        if (length < 24 || length > BCM_AWDL_TX_FRAME_MAX || !channel || result)
            return -EPROTO;
    } else if (kind == BCM_AWDL_SENDER) {
        if (length != 6 || channel || result || (p[BCM_AWDL_EVENT_HEADER_SIZE] & 1u) ||
            !memcmp(p + BCM_AWDL_EVENT_HEADER_SIZE, "\0\0\0\0\0\0", 6))
            return -EPROTO;
    } else if (length) {
        return -EPROTO;
    }
    if (((kind == BCM_AWDL_CHANNEL_OPEN || kind == BCM_AWDL_CHANNEL_CLOSED || kind == BCM_AWDL_NAN_DW) &&
         (!channel || result)) || (kind == BCM_AWDL_STOPPED && result))
        return -EPROTO;
    uint64_t firmware_us = read_le(p + 16, 8);
    if (!firmware_us)
        return -ERANGE;
    *out = (struct bcm_awdl_event) {
        .kind = kind, .flags = flags, .channel = channel,
        .generation = generation, .firmware_us = firmware_us,
        .cookie = cookie, .result = result,
        .frame = length ? p + BCM_AWDL_EVENT_HEADER_SIZE : NULL,
        .length = length,
    };
    return 1;
}

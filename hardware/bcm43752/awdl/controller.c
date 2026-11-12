/* SPDX-License-Identifier: BSD-3-Clause */
#include "controller.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define SLOT_US UINT64_C(65536)
/* Recalibrate only the host wakeup estimate. Neither installed plans nor
 * peer RX timestamps are translated through this changing map. */
#define CLOCK_REFRESH_US UINT64_C(1000000)
static uint64_t firmware_now(struct bcm_awdl_controller *c)
{
    uint64_t host = bcm_awdl_monotonic_us(), now;
    if (!host) {
        c->error = -EIO;
        return 0;
    }
    if (host >= c->session->host_origin &&
        host - c->session->host_origin >= CLOCK_REFRESH_US) {
        struct bcm_awdl_status status;
        c->error = bcm_awdl_clock_sample(c->session, &status);
        if (c->error)
            return 0;
        if (status.error || (status.flags & BCM_AWDL_STOPPING)) {
            fprintf(stderr, "BCM AWDL: firmware-status native=%d flags=0x%x "
                    "channel=0x%x queued=%u submitted=%u completed=%u received=%u\n",
                    status.error, status.flags, status.channel, status.queued,
                    status.submitted, status.completed, status.received);
            c->error = status.error ? -EIO : -ESHUTDOWN;
            return 0;
        }
        host = bcm_awdl_monotonic_us();
    }
    c->error = bcm_awdl_to_firmware(c->session, host, &now);
    if (c->error)
        return 0;
    if (now < c->last_now)
        now = c->last_now;
    c->last_now = now;
    return now;
}
static uint64_t read_le64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static int absolute(struct bcm_awdl_controller *c, int64_t relative, uint64_t *out)
{
    if (relative >= 1) {
        uint64_t offset = (uint64_t)relative - 1;
        if (offset > UINT64_MAX - c->origin)
            return -ERANGE;
        *out = c->origin + offset;
    } else {
        uint64_t offset = (uint64_t)(-(relative + 1)) + 2;
        if (offset > c->origin)
            return -ERANGE;
        *out = c->origin - offset;
    }
    return 0;
}
static int capabilities(struct bcm_awdl_controller *c, const uint8_t caps[28])
{
    /* Same peer capability interpretation as BE200. The AWDL HT TLV has
     * two prefix bytes, then the six transmitted IEEE HT bytes. */
    uint8_t ies[66] = {45, 26};
    memcpy(ies + 2, caps + 14, 6);
    ies[28] = 191;
    ies[29] = 12;
    memcpy(ies + 30, caps, 12);
    size_t size = 42;
    if (caps[20]) {
        ies[size++] = 255;
        ies[size++] = 22;
        ies[size++] = 35; /* HE Capabilities extension element. */
        uint8_t *he = ies + size;
        he[6] = 4;
        he[7] = (caps[23] ? 32 : 0) | ((caps[24] & 1) ? 64 : 0);
        he[13] = (caps[24] & 2) ? 4 : 0;
        uint8_t padding = caps[25];
        for (unsigned i = 26; i < 28; ++i)
            if (caps[i] > padding)
                padding = caps[i];
        he[15] = padding << 6;
        memcpy(he + 17, caps + 21, 2);
        memcpy(he + 19, caps + 21, 2);
        size += 21;
    }
    return bcm_awdl_peer(c->session, c->peer, ies, size);
}
static int window(struct bcm_awdl_controller *c, uint64_t open, uint64_t close)
{
    if (open == c->window_open && close == c->window_close)
        return 0;
    int error = bcm_awdl_window(c->session, open, close);
    if (!error) {
        c->window_open = open;
        c->window_close = close;
    }
    return error;
}
static int emit(void *context, uint32_t kind, const uint8_t *data, size_t length)
{
    struct bcm_awdl_controller *c = context;
    int error = 0;
    if (kind == 0) {
        if (c->pending_cookie)
            return -EBUSY;
        if (length < 36 || length > sizeof(c->action))
            error = -EMSGSIZE;
        else {
            memcpy(c->action, data, length);
            c->action_length = length;
        }
    } else if (kind == 1) {
        if (length != 34)
            error = -EINVAL;
        else {
            memcpy(c->peer, data, 6);
            error = capabilities(c, data + 6);
            if (!error)
                error = c->netdev_ready(c->context, c->peer);
        }
    } else if (kind == 4) {
        error = -ENOLINK; /* Owning process drains before selecting another peer. */
    } else if (kind == 5) {
        uint64_t open = 0, close = 0;
        if (length != 16)
            error = -EINVAL;
        else if (read_le64(data) || read_le64(data + 8)) {
            if (read_le64(data) > INT64_MAX || read_le64(data + 8) > INT64_MAX)
                error = -ERANGE;
            else {
                error = absolute(c, (int64_t)read_le64(data), &open);
                if (!error)
                    error = absolute(c, (int64_t)read_le64(data + 8), &close);
            }
        }
        if (!error)
            error = window(c, open, close);
    } else if (kind == 6) {
        uint64_t epoch;
        if (length != 26)
            error = -EINVAL;
        else {
            error = absolute(c, (int64_t)read_le64(data), &epoch);
            if (!error) {
                /* Keep native RX resident while the peer sleeps. Preserve
                 * the original peer plan below: follow_window must never
                 * admit TX merely because our own receiver is available. */
                uint8_t receive[18];
                for (unsigned i = 0; i < sizeof(receive); ++i)
                    receive[i] = data[8 + i] ? data[8 + i] : c->primary;
                error = bcm_awdl_plan(c->session, epoch, receive, receive[16], receive[17]);
            }
            if (!error) {
                c->epoch = epoch;
                memcpy(c->channels, data + 8, 16);
                c->preceding = data[24];
                c->following = data[25];
                c->planned = 1;
            }
        }
    } else if (kind == 7) {
        error = length == 28 ? capabilities(c, data) : -EINVAL;
    } else {
        error = -EOPNOTSUPP;
    }
    if (error)
        c->error = error;
    return error;
}
int bcm_awdl_controller_open(struct bcm_awdl_controller *c, struct bcm_awdl_session *session,
    const uint8_t local[6], unsigned primary, unsigned secondary,
    const char *target, const char *name, const uint8_t *he, uint32_t vht,
    unsigned drain_us, int (*ready)(void *, const uint8_t *), void *context)
{
    if (!session->generation || !session->clock_ready || !ready || !target || !name)
        return -EINVAL;
    *c = (struct bcm_awdl_controller) {
        .session = session,
        .primary = primary, .secondary = secondary, .drain_us = drain_us,
        .netdev_ready = ready, .context = context,
    };
    memcpy(c->local, local, 6);
    uint64_t now = firmware_now(c);
    c->origin = session->firmware_origin;
    if (!now || !c->origin)
        return -EIO;
    c->control = control_new(local, primary, secondary, target, name, he, drain_us, emit, c);
    if (!c->control)
        return -ENOMEM;
    control_rx_aggregation(c->control, vht);
    /* Discovery needs a real schedule before any peer is selected. The
     * public controller replaces it with the negotiated rolling plan. */
    uint8_t channels[16];
    memset(channels, primary, sizeof(channels));
    int error = bcm_awdl_plan(session, c->origin, channels, primary, primary);
    if (!error)
        control_idle_receive_channel(c->control, primary);
    if (error)
        c->error = error;
    return error;
}
void bcm_awdl_controller_close(struct bcm_awdl_controller *c)
{
    if (c->control)
        control_free(c->control);
    c->control = NULL;
}
static int follow_window(struct bcm_awdl_controller *c, uint64_t now)
{
    if (!c->secondary || !c->planned)
        return 0;
    if (now < c->epoch)
        return window(c, 0, 0);
    uint64_t elapsed = now - c->epoch;
    unsigned index = (elapsed / SLOT_US) % 16;
    unsigned channel = c->channels[index];
    if (!channel || channel != c->active)
        return window(c, 0, 0);
    uint64_t begin = now - elapsed % SLOT_US;
    uint64_t end = begin + SLOT_US;
    unsigned before = index ? c->channels[index - 1] : c->preceding;
    if (before != channel)
        begin += 3072; /* Same post-retune guard as the public controller. */
    for (unsigned i = 1; i < 16; ++i) {
        unsigned next = index + i;
        unsigned next_channel = next < 16 ? c->channels[next] : c->following;
        if (next_channel != channel)
            break;
        end += SLOT_US;
        if (next >= 16)
            break;
    }
    /* Firmware enforces drain_us before the first actual plan change;
     * the window itself uses the same firmware clock as the plan. */
    return begin < end ? window(c, begin, end) : window(c, 0, 0);
}
uint64_t bcm_awdl_controller_tick(struct bcm_awdl_controller *c)
{
    if (c->error || !c->control)
        return 0;
    uint64_t now = firmware_now(c);
    if (c->error || now < c->origin)
        return 0;
    uint64_t deadline = control_tick(c->control, now - c->origin + 1);
    if (c->error)
        return 0;
    c->error = follow_window(c, now);
    if (c->error)
        return 0;
    if (c->action_length && !c->pending_cookie && c->active) {
        if (++c->next_cookie == 0) {
            c->error = -EOVERFLOW;
            return 0;
        }
        /* Retain cookie even on uncertain transport failure; STOP owns it. */
        c->pending_cookie = c->next_cookie;
        int rejected;
        c->error = bcm_awdl_tx(c->session, c->pending_cookie, c->active,
                               c->origin, c->action, c->action_length, &rejected);
        if (rejected)
            c->pending_cookie = 0;
        /* A confirmed allocation/queue rejection acquired no TX owner.
         * Keep the latest action for the next protocol tick/channel event.
         * Never wait for a TX_DONE that firmware cannot emit. Transport
         * uncertainty instead keeps its cookie and enters normal STOP. */
        if (rejected && (c->error == -ENOMEM || c->error == -EBUSY))
            c->error = 0;
        else {
            if (c->error)
                return 0; /* Do not overwrite it with clock conversion. */
            c->action_length = 0;
        }
    }
    uint64_t absolute_deadline;
    if (deadline > INT64_MAX || absolute(c, (int64_t)deadline, &absolute_deadline)) {
        c->error = -ERANGE;
        return 0;
    }
    /* Followed channel windows need refresh at every slot boundary even
     * when the public controller's next discovery timer is farther away. */
    if (c->secondary && c->planned && now >= c->epoch) {
        uint64_t boundary = now + SLOT_US - (now - c->epoch) % SLOT_US;
        if (boundary < absolute_deadline)
            absolute_deadline = boundary;
    }
    uint64_t host_deadline;
    c->error = bcm_awdl_to_host(c->session, absolute_deadline, &host_deadline);
    if (c->error)
        return 0;
    uint64_t refresh = c->session->host_origin + CLOCK_REFRESH_US;
    return host_deadline < refresh ? host_deadline : refresh;
}
int bcm_awdl_controller_event(struct bcm_awdl_controller *c, const struct bcm_event *event)
{
    struct bcm_awdl_event decoded;
    int result = bcm_awdl_decode_event(c->session, event, &decoded);
    if (result <= 0)
        return result;
    unsigned channel = decoded.channel & 255u;
    if ((decoded.channel & 0xf800u) == 0xe000u)
        channel = channel - 6 + 4 * ((decoded.channel >> 8) & 7u);
    switch (decoded.kind) {
    case BCM_AWDL_SENDER:
        printf("{\"event\":\"sender-requested\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}\n",
               decoded.frame[0], decoded.frame[1], decoded.frame[2],
               decoded.frame[3], decoded.frame[4], decoded.frame[5]);
        break;
    case BCM_AWDL_RX: {
        uint64_t now = firmware_now(c);
        if (c->error)
            return c->error;
        if (decoded.firmware_us < c->origin)
            break;
        /* Receipt proves firmware has reached this time. Do not clamp a
         * real RX timestamp to the midpoint estimate of a host clock. */
        if (now < decoded.firmware_us)
            now = decoded.firmware_us;
        c->last_now = now;
        uint64_t received = decoded.firmware_us;
        control_rx(c->control, decoded.frame, decoded.length,
                   received - c->origin + 1, now - c->origin + 1);
        break;
    }
    case BCM_AWDL_TX_DONE:
        if (decoded.cookie == c->pending_cookie)
            c->pending_cookie = 0;
        break;
    case BCM_AWDL_CHANNEL_OPEN:
        c->active = channel;
        break;
    case BCM_AWDL_CHANNEL_CLOSED:
        if (c->active == channel)
            c->active = 0;
        break;
    case BCM_AWDL_ERROR:
        fprintf(stderr, "BCM AWDL: firmware-event native=%d channel=0x%x flags=0x%x\n",
                decoded.result, decoded.channel, decoded.flags);
        c->error = -EIO;
        break;
    case BCM_AWDL_STOPPED:
        c->active = 0;
        break;
    case BCM_AWDL_NAN_DW: {
        if (decoded.firmware_us > INT64_MAX || c->origin > INT64_MAX)
            return -ERANGE;
        int64_t began = (int64_t)decoded.firmware_us - (int64_t)c->origin + 1;
        /* Exclude discovery and both retunes from advertised receive slots,
         * using the firmware timestamp rather than Netlink receipt time. */
        control_absence(c->control, channel > 14, began - 4096,
                          began + 16 * 1024 + 4096, 512 * 1024);
        break;
    }
    }
    return c->error;
}
int bcm_awdl_controller_select(struct bcm_awdl_controller *c, const char *target)
{
    return c->control ? control_select(c->control, target) : -ENODEV;
}

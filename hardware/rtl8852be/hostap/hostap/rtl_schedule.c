#include "rtl_schedule.h"

static int Frequency(u8 channel)
{
    return channel <= 14 ? 2407 + 5 * channel : 5000 + 5 * channel;
}

static int Channel(const struct wa_caps *caps, const struct nan_schedule_channel *input,
                   struct wa_nan_channel *output)
{
    if (input->freq == 2437 && input->bandwidth == 20 &&
        (!input->center_freq1 || input->center_freq1 == 2437) && !input->center_freq2 && caps->ht_supported) {
        *output = (struct wa_nan_channel){6,6,0}; return 0;
    }
    if (input->center_freq2) return -EOPNOTSUPP;
    for (size_t i = 0; i < caps->n_channels; i++) {
        const struct wa_nan_channel *c = &caps->channels[i];
        if (input->freq == Frequency(c->primary) &&
            (input->center_freq1 ? input->center_freq1 : input->freq) == Frequency(c->center) &&
            input->bandwidth == (20 << c->width)) {
            *output = *c;
            return 0;
        }
    }
    return -EOPNOTSUPP;
}

static int Bitmap(const struct nan_schedule_channel *c, u32 *slots)
{
    if (!c->time_bitmap || wpabuf_len(c->time_bitmap) != 4) return -EINVAL;
    *slots = WPA_GET_LE32(wpabuf_head(c->time_bitmap));
    return 0;
}

int wa_hostap_schedule_local(const struct wa_caps *caps, const struct nan_schedule_config *input,
                            struct wa_hostap_schedule *output)
{
    struct wa_hostap_schedule next = {0};
    u32 occupied = 0, discovery = 0;
    if (!input->num_channels || input->num_channels > 2 || input->deferred) return -EOPNOTSUPP;
    for (unsigned i = 0; i < input->num_channels; i++) {
        struct wa_nan_channel channel;
        u32 slots;
        int result = Channel(caps, &input->channels[i], &channel);
        if (!result) result = Bitmap(&input->channels[i], &slots);
        if (result) return result;
        if (occupied & slots) return -EINVAL;
        occupied |= slots;
        if (channel.primary == 6 && !channel.width) discovery = slots;
        else {
            if (next.slots || (slots & 3)) return -EOPNOTSUPP;
            next.channel = channel; next.slots = slots;
        }
    }
    /* Native DW is slot 0; authentication remains on channel 6 in slot 1. */
    if ((discovery & 3) != 3) return -EOPNOTSUPP;
    next.auth_slots = discovery & ~1U;
    if (!next.slots) {
        if (discovery != 0xffffffffU) return -EOPNOTSUPP;
        next.channel = (struct wa_nan_channel){6, 6, 0}; next.slots = discovery & ~3U;
    }
    *output = next;
    return 0;
}

int wa_hostap_schedule_peer(const struct wa_caps *caps, const struct wa_hostap_schedule *local,
                           const struct nan_peer_schedule_config *input, struct wa_hostap_schedule *output)
{
    if (!input || input->n_maps > MAX_NUM_NAN_MAPS) return -EINVAL;
    struct wa_hostap_schedule next = { .channel = local->channel };
    u32 auth = 0;
    for (unsigned m = 0; m < input->n_maps; m++) {
        const struct nan_schedule_config *map = &input->maps[m].sched;
        if (map->num_channels > MAX_NUM_NAN_SCHEDULE_CHANNELS) return -EINVAL;
        for (unsigned i = 0; i < map->num_channels; i++) {
            const struct nan_schedule_channel *c = &map->channels[i];
            struct wa_nan_channel channel;
            u32 slots;
            int result = Bitmap(c, &slots);
            if (result) return result;
            if (c->freq == 2437) auth |= slots & 2;
            if (Channel(caps, c, &channel)) continue;
            if (channel.primary == local->channel.primary) {
                /* Wider peer availability includes our primary-channel
                 * transmission width. Retain the narrower supported channel. */
                if (channel.width > local->channel.width) channel = local->channel;
                u32 common = slots & local->slots;
                if (!common) continue;
                if (next.slots && memcmp(&next.channel, &channel, sizeof(channel))) return -EOPNOTSUPP;
                next.channel = channel;
                next.slots |= common;
            }
        }
    }
    if (!auth) return -ENODATA;
    next.slots |= auth;
    *output = next;
    return 0;
}

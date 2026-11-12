#include "native_calls.h"
/* A2 proposal/response selection. The caller supplies a separate proposal
 * grid; selecting a channel does not install a timeline or acquire a peer.
 * One physical NAN radio may consume both advertised peer availability maps.
 */
#include <stdint.h>
#include "nan_dam.h"

#define PTR(base, offset) (*(void **)((uint8_t *)(base) + (offset)))
#define U8(base, offset) (*(uint8_t *)((uint8_t *)(base) + (offset)))
#define U16(base, offset) (*(uint16_t *)((uint8_t *)(base) + (offset)))

static int physical(unsigned channel)
{
    return channel && channel != 0xff && (channel & 0xff00u) != 0xff00u &&
           (!(channel & 0xc000u) || (channel & 0xc000u) == 0xc000u);
}

static int shape(void *grid, unsigned slots)
{
    return grid && U16(grid, 0) == slots && U16(grid, 2) == slots * 16 &&
           U8(grid, 4) == 16;
}

static int validate_proposal(void *dam, void **out, void **maps, unsigned slots)
{
    if (!dam || !out || !maps || !slots || slots > 512 ||
        !PTR(dam, 4) || U8(dam, 0x10) != 1 ||
        U16(dam, 0x12) != slots * 16 || U16(dam, 0x14) != 16 ||
        !PTR(dam, 0x18) || !shape(*out, slots))
        return -2;
    void *container = PTR(dam, 0x18);
    if (U8(container, 0x0e) != 0xff)
        return -23;
    if (U16(container, 0) != slots || U16(container, 2) != slots * 16 ||
        U8(container, 4) != 16 || !shape(PTR(container, 8), slots))
        return -2;
    if (*out == PTR(container, 8))
        return -16;
    for (unsigned i = 0; i < 2; i++) {
        if (maps[i] == *out)
            return -16;
        if (maps[i] && !shape(maps[i], slots))
            return -2;
    }
    return 0;
}

static int prepare(void *dam, void *peer, void **out, void **maps,
                   unsigned slots, void **prohibited)
{
    if (!peer)
        return -2;
    int error = validate_proposal(dam, out, maps, slots);
    if (error)
        return error;
    void *nan = PTR(dam, 4), *availability = PTR(nan, 0x4c);
    if (!availability || !PTR(availability, 0x2c) || !PTR(peer, 0x0c))
        return -25;
    if (PTR(nan, 0x5c) != dam)
        return -30;
    void *wlc = PTR(nan, 8), *social = PTR(dam, 0x34);
    if (!wlc || !PTR(wlc, 0x18) || !PTR(wlc, 0xac) || !social)
        return -25;
    if (U8(social, 1) > U8(social, 0))
        return -30;
    /* 0x20da8 hides a parser failure behind NULL. Preserve the actual
     * 0x30e04 result while using its same native scratch-list owner. */
    *prohibited = ((void *(*)(void *))native_0x316d9)(nan);
    return ((int (*)(void *, void *, void *))native_0x30e05)(
        nan, PTR(peer, 0x0c), *prohibited);
}

static int get(void *grid, unsigned index, unsigned type, uint16_t *channel)
{
    return ((int (*)(void *, unsigned, unsigned, uint16_t *))native_0x20f2d)(
        grid, index, type, channel);
}

static int free_slot(void *grid, unsigned index)
{
    return ((int (*)(void *, unsigned))native_0x20715)(grid, index);
}

static void done(void *grid, unsigned index, unsigned committed)
{
    ((void (*)(void *, unsigned, unsigned))native_0x2111d)(grid, index, committed);
}

static int skip_for_5g_potential(void *dam, void *peer, void **maps, unsigned index)
{
    uint16_t channel;
    for (unsigned i = 0; i < 2; i++) {
        if (!get(maps[i], index, 1, &channel) &&
            ((channel & 0xc000u) == 0xc000u || !get(maps[i], index, 4, &channel)))
            return 0;
    }
    void *nan = PTR(dam, 4), *availability = PTR(nan, 0x4c), *list = 0;
    if (!PTR(availability, 0x30))
        return -25;
    int error = ((int (*)(void *, void *, unsigned, void **))native_0x20d79)(
        dam, peer, index, &list);
    if (error == -30)
        return 0; /* No potential entry for this slot. */
    if (error)
        return error;
    if (!list || U8(list, 1) > U8(list, 0))
        return -30;
    unsigned reference = ((unsigned (*)(void *, unsigned))native_0x20ee9)(dam, 0);
    channel = ((unsigned (*)(void *, void *, unsigned, unsigned))native_0x20de9)(
        nan, list, 1, reference);
    if (channel == 0xff01 || channel == 0xff03)
        return 1;
    return physical(channel) && (channel & 0xc000u) == 0xc000u &&
           nan_dam_valid_chanspec(dam, 0, channel, 1);
}

int nan_dam_proposal_from_committed(void *dam, void *peer, void **maps,
                                    void **out, unsigned slots, unsigned type)
{
    void *prohibited;
    int error = prepare(dam, peer, out, maps, slots, &prohibited);
    if (error)
        return error;
    void *nan = PTR(dam, 4), *grid = *out;
    int prefer_potential = maps[0] && maps[1] && type != 5 &&
        ((int (*)(void *))native_0x21a81)(dam) &&
        ((unsigned (*)(void *, void *, unsigned))native_0x20f49)(nan, 0, 1) != 0xff;
    for (unsigned index = 0; index < slots; index++) {
        if (prefer_potential) {
            int skip = skip_for_5g_potential(dam, peer, maps, index);
            if (skip < 0)
                return skip;
            if (skip)
                continue;
        }
        unsigned selected = 0xff;
        for (unsigned i = 0; i < 2; i++) {
            uint16_t channel;
            if (get(maps[i], index, 1, &channel) || !physical(channel) ||
                !nan_dam_valid_chanspec(dam, prohibited, channel, 3) ||
                !free_slot(grid, index))
                continue;
            unsigned reference = ((unsigned (*)(void *, unsigned))native_0x20ee9)(dam, 0);
            if ((reference & 0xff00u) == 0xff00u ||
                !((int (*)(void *, unsigned, unsigned, unsigned))native_0x206e9)(
                    dam, 0, index, 3)) {
                selected = channel;
                if ((channel & 0xc000u) == 0xc000u)
                    break;
            } else {
                if (((int (*)(void *, unsigned, unsigned, unsigned))native_0x206e9)(
                        dam, 0, index, 2))
                    reference = U16(grid, 8 + index * 4 + 2);
                if (physical(reference) &&
                    nan_dam_valid_chanspec(dam, prohibited, reference, 2))
                    ((void (*)(void *, unsigned, unsigned, unsigned))native_0x21415)(
                        grid, index, 2, reference);
                done(grid, index, 0);
            }
        }
        if (selected != 0xff) {
            ((void (*)(void *, unsigned, unsigned, unsigned))native_0x21415)(
                grid, index, 2, selected);
            done(grid, index, 0);
        }
    }
    return 0;
}

static int peer_available(void *dam, void *peer, unsigned index, unsigned channel)
{
    void *nan = PTR(dam, 4), *list = 0;
    if (((int (*)(void *, void *, unsigned, unsigned, unsigned, void *))native_0x3189d)(
            nan, PTR(peer, 0x0c), index, 1, channel, 0) == 1)
        return 1;
    if (!PTR(PTR(nan, 0x4c), 0x30))
        return -25;
    int error = ((int (*)(void *, void *, unsigned, void **))native_0x20d79)(
        dam, peer, index, &list);
    if (error == -30)
        return 0;
    if (error)
        return error;
    if (!list || U8(list, 1) > U8(list, 0))
        return -30;
    unsigned offered = ((unsigned (*)(void *, void *, unsigned, unsigned))native_0x20de9)(
        nan, list, (channel & 0xc000u) != 0, channel);
    return offered != 0xff && ((offered & 0xff00u) == 0xff00u ||
        ((int (*)(unsigned, unsigned))native_0xbd71)(offered, channel));
}

static int select_ndc(void *dam, void *peer, void **out, unsigned slots,
                       unsigned target, unsigned first_phase, void *prohibited)
{
    uint8_t selected[64];
    ((void (*)(void *, unsigned))native_0x150891)(selected, sizeof(selected));
    unsigned count = 0;
    void *grid = *out;
    /* Prefer the usual discovery-interval positions, then remaining slots.
     * Count each slot once, even when the final sweep revisits a preference.
     * Publication follows successful selection; failure retains old NDC bits. */
    for (unsigned pass = 0; pass < 4 && count < target; pass++) {
        unsigned begin = pass == 0 ? 1 : pass == 1 ? 9 : pass == 2 ? 10 : 0;
        unsigned end = pass < 2 ? begin + 1 : pass == 2 ? 32 : slots;
        if (pass == 0 && !first_phase)
            continue;
        if (end > slots)
            end = slots;
        for (unsigned index = begin; index < end && count < target; index++) {
            unsigned mask = 1u << (index & 7u);
            if (selected[index / 8] & mask)
                continue;
            unsigned flags = U8(grid, 8 + index * 4);
            unsigned channel = U16(grid, 8 + index * 4 + 2);
            if (!(flags & (peer ? 3u : 0x40u)) || (!peer && !(flags & 1u)) ||
                !physical(channel) ||
                !nan_dam_valid_chanspec(dam, prohibited, channel, peer ? 3 : 1))
                continue;
            if (peer && (flags & 0x41u) == 1) {
                int available = peer_available(dam, peer, index, channel);
                if (available < 0)
                    return available;
                if (!available)
                    continue;
            }
            selected[index / 8] |= mask;
            count++;
        }
    }
    if (!count)
        return -1;
    for (unsigned index = 0; index < slots; index++) {
        U8(grid, 8 + index * 4) &= ~4u;
        if (selected[index / 8] & (1u << (index & 7u)))
            ((void (*)(void *, unsigned))native_0x21405)(grid, index);
    }
    return 0;
}

int nan_dam_proposal_ndc_slots(void *dam, void *peer, void **out, unsigned slots)
{
    if (!dam || !peer || !PTR(dam, 4) || !PTR(peer, 0x0c) || !PTR(peer, 0x10))
        return -2;
    void *nan = PTR(dam, 4), *maps[2] = {0, 0};
    unsigned count = U8(PTR(peer, 0x0c), 0);
    if (!count || count > 2)
        return -2;
    for (unsigned i = 0; i < count; i++)
        maps[i] = ((void *(*)(void *, void *, unsigned))native_0x30bd9)(
            nan, PTR(peer, 0x0c), i);
    void *prohibited;
    int error = prepare(dam, peer, out, maps, slots, &prohibited);
    if (error)
        return error;
    unsigned target = U8(PTR(peer, 0x10), 0);
    if (target < 22)
        target = 22;
    return select_ndc(dam, peer, out, slots, target, count == 1, prohibited);
}

int nan_dam_response_ndc_slots(void *dam, void **out, unsigned slots)
{
    /* A2 0x20c0c passes only DAM, output grids and slot count. There is no
     * peer argument here; never import the later firmware's four-arg ABI.
     * Only actually accepted (committed/applied) response slots are eligible. */
    void *maps[2] = {0, 0};
    int error = validate_proposal(dam, out, maps, slots);
    return error ? error : select_ndc(dam, 0, out, slots, 22, 1, 0);
}

int nan_dam_generate_proposal(void *dam, unsigned type, void *peer,
                              void **local, void **maps, void **out, unsigned slots)
{
    void *prohibited;
    int error = prepare(dam, peer, out, maps, slots, &prohibited);
    if (error)
        return error;
    if (!local || !shape(*local, slots) || *local == *out)
        return -2;
    error = ((int (*)(void *, unsigned, const void *, unsigned))native_0x151c81)(
        (uint8_t *)*out + 8, slots * 4, (uint8_t *)*local + 8, slots * 4);
    if (error)
        return error;
    for (unsigned i = 0; i < slots; i++)
        U8(*out, 9 + i * 4) &= ~0x80u;
    ((void (*)(void *, void **, void **, unsigned))native_0x2137d)(dam, local, out, slots);
    error = nan_dam_proposal_from_committed(dam, peer, maps, out, slots, type);
    if (!error)
        error = nan_dam_proposal_from_potential(dam, peer, maps, out, slots);
    if (!error)
        error = nan_dam_proposal_ndc_slots(dam, peer, out, slots);
    return error;
}

static unsigned potential_choice(void *dam, void *prohibited, void **out,
                                  unsigned index, unsigned band, unsigned channel,
                                  unsigned reference)
{
    if (!physical(channel) || ((channel & 0xc000u) != 0) != band ||
        !nan_dam_valid_chanspec(dam, prohibited, channel, 3))
        return 0xff;
    if (((int (*)(void *, unsigned, unsigned, unsigned))native_0x206e9)(dam, 0, index, 1) &&
        (!physical(reference) ||
         !((int (*)(unsigned, unsigned))native_0xbd71)(reference, channel)))
        return 0xff;
    return ((int (*)(void *, void **, unsigned, unsigned, unsigned))native_0x21145)(
        dam, out, 0, index, channel) ? channel : 0xff;
}

static unsigned expand_potential(void *dam, void *prohibited, void **out,
                                  unsigned index, unsigned band, unsigned offered,
                                  unsigned reference)
{
    if ((offered & 0xff00u) != 0xff00u)
        return potential_choice(dam, prohibited, out, index, band, offered, reference);
    if (offered != 0xff01 && offered != (band ? 0xff03u : 0xff02u))
        return 0xff;
    /* A peer band wildcard authorizes selection within that band, but never
     * bypasses local CLM, peer prohibitions or an existing reference owner.
     * Use native bandwidth selection only when the peer offered a wildcard;
     * an explicit peer chanspec retains its advertised width. */
    void *nan = PTR(dam, 4);
    unsigned choices[3] = {reference, U16(*out, 8 + index * 4 + 2),
        ((unsigned (*)(void *, void *, unsigned))native_0x20f49)(nan, prohibited, band)};
    for (unsigned i = 0; i < 3; i++) {
        struct { uint8_t flags, nss; uint16_t channel; } slot = {0, 0, choices[i]};
        if (!physical(slot.channel) || ((slot.channel & 0xc000u) != 0) != band)
            continue;
        ((void (*)(void *, void *))native_0x21421)(nan, &slot);
        unsigned result = potential_choice(dam, prohibited, out, index, band,
                                            slot.channel, reference);
        if (result != 0xff)
            return result;
    }
    return 0xff;
}

int nan_dam_proposal_from_potential(void *dam, void *peer, void **maps,
                                    void **out, unsigned slots)
{
    void *prohibited;
    int error = prepare(dam, peer, out, maps, slots, &prohibited);
    if (error)
        return error;
    void *nan = PTR(dam, 4), *availability = PTR(nan, 0x4c);
    if (!PTR(availability, 0x30))
        return -25;
    unsigned dual = !!((int (*)(void *))native_0x21a81)(dam);
    unsigned reference = ((unsigned (*)(void *, unsigned))native_0x20ee9)(dam, 0);
    for (unsigned index = 0; index < slots; index++) {
        if (!free_slot(*out, index) || !(U8(*out, 8 + index * 4) & 0x21u))
            continue;
        void *list = 0;
        error = ((int (*)(void *, void *, unsigned, void **))native_0x20d79)(
            dam, peer, index, &list);
        if (error == -30)
            continue;
        if (error)
            return error;
        if (!list || U8(list, 1) > U8(list, 0))
            return -30;
        unsigned selected = 0xff;
        for (int band = (int)dual; band >= 0 && selected == 0xff; band--) {
            unsigned preferred = ((unsigned (*)(void *, void *, unsigned, unsigned))native_0x20de9)(
                nan, list, (unsigned)band, reference);
            selected = expand_potential(dam, prohibited, out, index, (unsigned)band,
                                         preferred, reference);
            /* A valid peer preference may conflict with this local slot.
             * Consider the other decoded offers before abandoning the band. */
            for (unsigned i = 0; i < U8(list, 1) && selected == 0xff; i++) {
                unsigned offered = U16(list, 6 + i * 4);
                if (offered != preferred)
                    selected = expand_potential(dam, prohibited, out, index,
                                                 (unsigned)band, offered, reference);
            }
        }
        if (selected != 0xff) {
            ((void (*)(void *, unsigned, unsigned, unsigned))native_0x21415)(
                *out, index, 2, selected);
            done(*out, index, 0);
        }
    }
    return 0;
}

int nan_dam_response_from_type(void *dam, void *peer, void **out,
                               void **maps, unsigned slots, unsigned type)
{
    if (type != 1 && type != 0x20)
        return -2;
    void *prohibited;
    int error = prepare(dam, peer, out, maps, slots, &prohibited);
    if (error)
        return error;
    void *grid = *out;
    for (unsigned index = 0; index < slots; index++) {
        int selected = -1;
        for (unsigned i = 0; i < 2; i++) {
            uint16_t channel, local;
            if (get(maps[i], index, 3, &channel))
                continue;
            unsigned flags = U8(maps[i], 8 + index * 4);
            if (!physical(channel) || !nan_dam_valid_chanspec(dam, prohibited, channel, 3)) {
                if (flags & 8u)
                    return -1; /* A mandatory peer slot cannot be discarded. */
                continue;
            }
            if (!free_slot(grid, index)) {
                if ((flags & 8u) && !(U8(grid, 8 + index * 4) & 0x40u))
                    return -1;
                continue;
            }
            if (get(grid, index, type, &local))
                continue;
            int compatible = ((int (*)(void *, void **, unsigned, unsigned, unsigned))native_0x21145)(
                dam, out, 0, index, channel);
            if (!compatible) {
                if (flags & 8u)
                    return -1;
                continue;
            }
            if (!(flags & 8u) && (channel & 0xc000u) != 0xc000u &&
                (U8(grid, 8 + index * 4) & 1u)) {
                done(grid, index, 1);
                continue;
            }
            /* The first peer map wins when it is mandatory over an optional
             * second map, or when equal-priority maps offer 5 GHz first. */
            selected = (int)i;
            if (i || !maps[1])
                break;
            unsigned other = U8(maps[1], 8 + index * 4);
            if ((flags ^ other) & 8u) {
                if (flags & 8u)
                    break;
                selected = -1;
            } else if ((channel & 0xc000u) == 0xc000u) {
                break;
            }
        }
        if (selected >= 0) {
            ((void (*)(void *, void *, unsigned))native_0x20959)(maps[selected], grid, index);
            done(grid, index, 1);
        }
    }
    /* A later mandatory failure leaves a partially built proposal, not an
     * installed schedule. The caller must consume the returned error. */
    return 0;
}

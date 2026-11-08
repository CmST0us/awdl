/* A2 received availability and negotiated CRB validation. Keep parsed maps
 * and native allocation owners on errors; never turn failed validation into
 * a newly active peer. The host remains the sole protocol owner by default. */
#include <stdint.h>
#include "nan_dam.h"
#include "nan_schedule.h"

#define PTR(p, o) (*(void **)((uint8_t *)(p) + (o)))
#define U8(p, o) (*(uint8_t *)((uint8_t *)(p) + (o)))
#define U16(p, o) (*(uint16_t *)((uint8_t *)(p) + (o)))
#define U32(p, o) (*(uint32_t *)((uint8_t *)(p) + (o)))

static int grid_valid(void *grid)
{
    if (!grid)
        return 1;
    unsigned period = U16(grid, 2);
    return period >= 512 && period <= 8192 && !(period & (period - 1)) &&
           U8(grid, 4) == 16 && U16(grid, 0) == period / 16;
}

int nan_dam_check_peer_crb(void *dam, void *peer)
{
    if (!dam || !peer || !PTR(dam, 4) || !PTR(dam, 8))
        return -2;
    void *nan = PTR(dam, 4), *map = PTR(peer, 0x4c);
    if (!U8(PTR(dam, 8), 0) || !map)
        return 0;
    if (U8(PTR(dam, 8), 0) != 3 || U8(dam, 0x10) != 1 || U8(peer, 0x69))
        return -23;
    if (PTR(nan, 0x5c) != dam || PTR(dam, NAN_DAM_HOST_PEERS) ||
        nan_dam_transition_pending(dam))
        return -16;
    void *container = PTR(dam, 0x18), *source = PTR(peer, 0x0c);
    void *av = PTR(nan, 0x4c), *wlc = PTR(nan, 8);
    if (!container || !source || !av || !wlc || !PTR(wlc, 0) ||
        !PTR(av, 0x2c) || U8(av, 0x26) != 8 || U8(av, 0x27) != 4 ||
        U16(av, 0x10) != 20 || !U8(source, 0) || U8(source, 0) > 2 ||
        U8(map, 0) != 1 || U16(dam, 0x14) != 16 ||
        U8(container, 4) != 16 || !PTR(container, 8) ||
        !grid_valid(PTR(container, 8)) ||
        U16(container, 2) != U16(dam, 0x12) ||
        U16(container, 0) != U16(container, 2) / 16 ||
        U16(PTR(container, 8), 2) != U16(container, 2))
        return -2;
    if (U8(container, 0x0e) != 0xff)
        return -23;
    void *installed = PTR(map, 0x18);
    if (map == source || source == PTR(dam, 0x38) || map == PTR(dam, 0x38) ||
        installed == PTR(container, 8))
        return -16;
    /* Request preparation owns an empty negotiated map until activation.
     * Its missing grid is valid while no NDP owns an installed schedule. */
    if (!grid_valid(installed) || (!installed && U8(peer, 0x68)))
        return -2;
    for (unsigned i = 0; i < U8(source, 0); ++i) {
        void *grid = PTR(source, 0x18 + i * 20);
        if (!grid_valid(grid))
            return -2;
        if (grid && (grid == installed || grid == PTR(container, 8)))
            return -16;
    }
    uint16_t period = U16(container, 2), advertised = 0;
    int error = ((int (*)(void *, void *, uint16_t *))0x30cd5u)(
        nan, source, &advertised);
    if (error)
        return error;
    if (advertised > period || (installed && U16(installed, 2) != period))
        error = ((int (*)(void *, void *, uint16_t *))0x32155u)(nan, peer, &period);
    else
        error = nan_dam_resize_map(nan, source, 16, period);
    if (error)
        return error;
    void *maps[2], *out[2];
    ((void (*)(void *, void **, void **, void *, void *))0x20c5du)(
        dam, maps, out, source, map);
    if (out[1] || (out[0] && U16(out[0], 2) != period))
        return -30;
    for (unsigned i = 0; i < 2; ++i)
        if (maps[i] && U16(maps[i], 2) != period)
            return -30;
    void *prohibited = ((void *(*)(void *))0x316d9u)(nan);
    error = ((int (*)(void *, void *, void *))0x30e05u)(nan, source, prohibited);
    if (error)
        return error;
    unsigned band2 = 0, band5 = 0;
    for (unsigned m = 0; m < 2; ++m) {
        for (unsigned i = 0; maps[m] && i < period / 16; ++i) {
            uint16_t channel, committed;
            if (((int (*)(void *, unsigned, unsigned, uint16_t *))0x20f2du)(
                    maps[m], i, 1, &channel) || !channel || channel == 0xff ||
                (channel & 0xff00u) == 0xff00u ||
                ((channel & 0xc000u) && (channel & 0xc000u) != 0xc000u))
                continue;
            unsigned primary20 = ((unsigned (*)(unsigned))0xc009u)(channel);
            if (!nan_dam_valid_chanspec(dam, prohibited, primary20, 3) ||
                ((int (*)(void *, unsigned, unsigned, uint16_t *))0x20f2du)(
                    out[0], i, 1, &committed) ||
                !((int (*)(unsigned, unsigned))0xbd71u)(channel, committed))
                continue;
            if (committed & 0xc000u)
                ++band5;
            else
                ++band2;
        }
    }
    unsigned flags = U16(peer, 0) & ~0x52u;
    if (band2 || band5)
        flags |= 2u | (band5 ? 0x10u : 0) | (band2 > band5 ? 0x40u : 0);
    else
        U32(peer, 0x6c) = U32(PTR(wlc, 0), 0x28); /* A2 public clock, not B0 +0x24. */
    U16(peer, 0) = flags;
    return 0;
}

/* Original 0x21cdc discards the CRB callback result before configuring QoS.
 * 0x33a60 already consumes this function's error and rejects the exchange. */
int nan_dam_parse_peer_schedule(void *dam, unsigned type, void *peer,
                                void *attributes, void *frame, unsigned force)
{
    if (!dam || !peer || !PTR(dam, 4) || !PTR(dam, 8) || !attributes ||
        type > 8 || force > 1)
        return -2;
    if (nan_dam_transition_pending(dam))
        return -16;
    void *nan = PTR(dam, 4);
    unsigned cursor = 0;
    void *attribute = ((void *(*)(void *, void *, unsigned, unsigned *))0x36a39u)(
        nan, attributes, 0x12, &cursor);
    if (!attribute || (!force && U8(attribute, 3) ==
        ((unsigned (*)(void *, void *))0x373c9u)(nan, peer)))
        return 0;
    if (PTR(dam, 0x1c)) {
        if (!PTR(peer, 0x0c) || PTR(peer, 0x0c) == PTR(dam, 0x1c))
            return -16;
        int error = ((int (*)(void *, void *, void *))0x307d9u)(
            nan, PTR(peer, 0x0c), PTR(dam, 0x1c));
        if (!error)
            error = ((int (*)(void *, void *, void *))0x307fdu)(
                nan, PTR(peer, 0x0c), PTR(dam, 0x1c));
        if (error)
            return error;
    }
    int error = ((int (*)(void *, unsigned, void *, void **))0x328e5u)(
        nan, type, attributes, &PTR(dam, 0x1c));
    if (!error)
        error = ((int (*)(void *, void *, void *))0x31f65u)(nan, frame, PTR(dam, 0x1c));
    if (!error)
        error = ((int (*)(void *, unsigned, void *, void **))0x328e5u)(
            nan, type, attributes, &PTR(peer, 0x0c));
    if (!error)
        error = ((int (*)(void *, void *, void *))0x31f65u)(nan, frame, PTR(peer, 0x0c));
    if (!error && U8(PTR(dam, 8), 0) == 3)
        error = nan_dam_check_peer_crb(dam, peer);
    if (!error && frame)
        error = ((int (*)(void *, void *, void *, unsigned))0x34259u)(
            nan, peer, (uint8_t *)frame + 8, 0);
    return error;
}

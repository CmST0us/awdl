#include "native_calls.h"
/* A2 0x32154 expands local and peer timelines before schedule negotiation.
 * Its original map helper drops the old grids before allocating replacements.
 * Prepare every replacement first; publish only a complete set of owners.
 */
#include <stdint.h>
#include "nan_dam.h"

#define PTR(base, offset) (*(void **)((uint8_t *)(base) + (offset)))
#define U8(base, offset) (*(uint8_t *)((uint8_t *)(base) + (offset)))
#define U16(base, offset) (*(uint16_t *)((uint8_t *)(base) + (offset)))
#define U32(base, offset) (*(uint32_t *)((uint8_t *)(base) + (offset)))

static int valid_period(unsigned unit, unsigned period)
{
    return unit == 16 && period >= 512 && period <= 8192 &&
           !(period & (period - 1));
}

static int valid_grid(void *grid)
{
    return grid && valid_period(U8(grid, 4), U16(grid, 2)) &&
           U16(grid, 0) == U16(grid, 2) / 16;
}

static int valid_context(void *nan)
{
    void *availability = nan ? PTR(nan, 0x4c) : 0;
    return availability && U8(availability, 0x26) == 8 &&
           U8(availability, 0x27) == 4;
}

static void repeat_cells(void *out, void *source, unsigned old_slots,
                         unsigned new_slots)
{
    /* Both periods are powers of two with the same 16 TU origin. Copy all
     * four bytes, including native reference flags and per-slot NSS bits. */
    for (unsigned i = 0; i < new_slots; ++i)
        U32(out, 4 * i) = U32(source, 4 * (i % old_slots));
}

struct map_growth {
    void *map;
    void *old[2];
    void *next[2];
    unsigned count;
};

static void discard_growth(void *nan, struct map_growth *growth)
{
    for (unsigned i = 0; i < growth->count; ++i)
        if (growth->next[i])
            ((void (*)(void *, void **))native_0x30af9)(nan, &growth->next[i]);
}

static int prepare_growth(void *nan, void *map, unsigned unit, unsigned period,
                           struct map_growth *growth)
{
    growth->map = map;
    growth->count = 0;
    growth->next[0] = growth->next[1] = 0;
    if (!valid_context(nan) || !map || !valid_period(unit, period) ||
        !U8(map, 0) || U8(map, 0) > 2)
        return -2;
    unsigned count = U8(map, 0), old_period = 0;
    for (unsigned i = 0; i < count; ++i) {
        void *grid = PTR(map, 0x18 + i * 0x14);
        if (!valid_grid(grid) || (i && (U16(grid, 2) != old_period ||
                                      grid == growth->old[0])))
            return -2;
        old_period = U16(grid, 2);
        growth->old[i] = grid;
    }
    if (old_period >= period)
        return 0; /* Native contract only extends; it never shrinks. */
    growth->count = count;
    for (unsigned i = 0; i < count; ++i) {
        void *next = ((void *(*)(void *, unsigned, unsigned))native_0x305f5)(
            nan, unit, period);
        if (!next) {
            discard_growth(nan, growth);
            return -27;
        }
        growth->next[i] = next;
        void *old = growth->old[i];
        U8(next, 5) = U8(old, 5);
        U16(next, 6) = U16(old, 6);
        repeat_cells((uint8_t *)next + 8, (uint8_t *)old + 8,
                     U16(old, 0), U16(next, 0));
    }
    return 0;
}

static void publish_growth(void *nan, struct map_growth *growth)
{
    /* No callback runs between installing these pointers and freeing the old
     * grids. Attribute buffers, map IDs and list references stay with map. */
    for (unsigned i = 0; i < growth->count; ++i)
        PTR(growth->map, 0x18 + i * 0x14) = growth->next[i];
    for (unsigned i = 0; i < growth->count; ++i) {
        growth->next[i] = 0;
        ((void (*)(void *, void **))native_0x30af9)(nan, &growth->old[i]);
    }
}

int nan_dam_resize_map(void *nan, void *map, unsigned unit, unsigned period)
{
    struct map_growth growth;
    int error = prepare_growth(nan, map, unit, period, &growth);
    if (!error)
        publish_growth(nan, &growth);
    return error;
}

int nan_dam_resize_schedule(void *dam, unsigned unit, unsigned period)
{
    if (!dam || !PTR(dam, 4) || !PTR(dam, 8))
        return -2;
    unsigned mode = U8(PTR(dam, 8), 0);
    if (mode == 0 || mode == 2)
        return 0; /* Host-owned availability has its own install transaction. */
    if (mode != 3)
        return -23;
    void *nan = PTR(dam, 4), *old = PTR(dam, 0x18);
    if (!valid_context(nan) || PTR(nan, 0x5c) != dam ||
        !valid_period(unit, period) || U8(dam, 0x10) != 1 || !old ||
        !valid_grid(PTR(old, 8)))
        return -2;
    void *grid = PTR(old, 8);
    unsigned old_period = U16(grid, 2);
    if (U16(old, 0) != U16(grid, 0) || U16(old, 2) != old_period ||
        U8(old, 4) != 16 || U16(dam, 0x12) != old_period ||
        U16(dam, 0x14) != 16)
        return -2;
    if (U8(old, 0x0e) != 0xff)
        return -23; /* Infrastructure ownership is outside single NAN. */
    if (old_period >= period) {
        /* A prior expansion can have failed at attributes/MSCH after the
         * owner swap. Retry that real commit instead of claiming success. */
        return ((int (*)(void *, unsigned))native_0x21771)(dam, 1);
    }
    struct map_growth proposal;
    proposal.count = 0;
    void *map = PTR(dam, 0x38);
    if (map && U8(map, 0) > 2)
        return -2;
    if (map && ((int (*)(void *))native_0x20845)(dam)) {
        for (unsigned i = 0; i < U8(map, 0); ++i)
            if (PTR(map, 0x18 + i * 0x14) == grid)
                return -16;
        int error = prepare_growth(nan, map, unit, period, &proposal);
        if (error)
            return error;
    }
    void *next = ((void *(*)(void *, unsigned, unsigned))native_0x201bd)(dam, unit, period);
    if (!next) {
        discard_growth(nan, &proposal);
        return -27;
    }
    void *next_grid = PTR(next, 8);
    U8(next, 5) = U8(old, 5);
    U16(next, 6) = U16(old, 6);
    U32(next, 0x0c) = U32(old, 0x0c);
    U8(next_grid, 5) = U8(grid, 5);
    U16(next_grid, 6) = U16(grid, 6);
    repeat_cells((uint8_t *)next + 16, (uint8_t *)old + 16,
                 U16(old, 0), U16(next, 0));
    repeat_cells((uint8_t *)next_grid + 8, (uint8_t *)grid + 8,
                 U16(grid, 0), U16(next_grid, 0));
    /* Native MSCH owns a converted chanspec array, not either grid. All
     * allocation has succeeded before changing any live timeline owner. */
    PTR(dam, 0x18) = next;
    U16(dam, 0x12) = period;
    publish_growth(nan, &proposal);
    ((void (*)(void *, void *))native_0x2020d)(dam, old);
    /* Attribute or scheduler failure retains these actual replacement
     * owners. It must not restore freed storage or hide the real error. */
    return ((int (*)(void *, unsigned))native_0x21771)(dam, 1);
}

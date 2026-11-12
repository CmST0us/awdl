#include "native_calls.h"
/* A2 schedule response ownership. Keep native peer-NDC validation and NA
 * encoding; do not turn their allocation/parse failures into generic success.
 * These callbacks do not enable a second protocol owner alongside hostap.
 */
#include <stdint.h>
#include "nan_dam.h"

#define PTR(base, offset) (*(void **)((uint8_t *)(base) + (offset)))
#define U8(base, offset) (*(uint8_t *)((uint8_t *)(base) + (offset)))
#define U16(base, offset) (*(uint16_t *)((uint8_t *)(base) + (offset)))
#define U32(base, offset) (*(uint32_t *)((uint8_t *)(base) + (offset)))

static int reject(void *result, unsigned reason, int error)
{
    if (result) {
        U32(result, 0) = 2;
        U16(result, 4) = reason;
    }
    return error;
}

static int context(void *dam, void *peer, void *result, unsigned type)
{
    if (!dam || !peer || !result || !PTR(dam, 4) || !PTR(dam, 8))
        return -2;
    void *nan = PTR(dam, 4), *availability = PTR(nan, 0x4c);
    if (PTR(nan, 0x5c) != dam || !availability ||
        U8(availability, 0x26) != 8 || U8(availability, 0x27) != 4 ||
        U16(availability, 0x28) != 24)
        return -30;
    /* Host peer timelines and automatic negotiation cannot own this radio
     * together. +0x5c is this adapter's host-owner list, not a B0 field. */
    if (U8(PTR(dam, 8), 0) == 3 && PTR(dam, 0x5c))
        return -16;
    if (nan_dam_transition_pending(dam))
        return -16;
    return type <= 4 ? 0 : -23; /* Ranging is not advertised by this adapter. */
}

static int grid_matches(void *grid, unsigned slots)
{
    return grid && U16(grid, 0) == slots && U16(grid, 2) == slots * 16 &&
           U8(grid, 4) == 16;
}

static int response_arrays(void *dam, void *peer, void **maps, void **out,
                            uint16_t *slots)
{
    void *container = PTR(dam, 0x18);
    void *source = PTR(peer, 0x0c), *proposal = PTR(dam, 0x38);
    if (U8(dam, 0x10) != 1 || !container || !source || !proposal ||
        !U8(source, 0) || U8(source, 0) > 2 || U8(proposal, 0) != 1 ||
        !PTR(dam, 0x28) ||
        U16(dam, 0x14) != 16 || U8(container, 4) != 16)
        return -2;
    unsigned count = U16(container, 0), period = count * 16;
    if (count < 32 || count > 512 || (count & (count - 1)) ||
        U16(container, 2) != period || U16(dam, 0x12) != period ||
        !grid_matches(PTR(container, 8), count) ||
        !grid_matches(PTR(proposal, 0x18), count))
        return -2;
    if (U8(container, 0x0e) != 0xff)
        return -23;
    if (source == proposal || PTR(container, 8) == PTR(proposal, 0x18) ||
        PTR(proposal, 8) || PTR(proposal, 0x0c))
        return -16;
    for (unsigned i = 0; i < U8(source, 0); ++i) {
        void *grid = PTR(source, 0x18 + i * 20);
        if (grid && !grid_matches(grid, count))
            return -2;
        if (grid && (grid == PTR(proposal, 0x18) || grid == PTR(container, 8)))
            return -16;
    }
    /* Actual A2 helper obtains the current maximum period and two arrays.
     * Do not import B0 stack/layout assumptions or silently truncate a peer. */
    ((void (*)(void *, void **, void **, void *, uint16_t *))native_0x21081)(
        dam, maps, out, peer, slots);
    if (*slots != count || out[0] != PTR(proposal, 0x18) || out[1])
        return -2;
    return 0;
}

static void free_map(void *nan, void *map)
{
    if (map) {
        /* Native invalidation skips entries whose map ID is still 0xff.
         * A prepared, not-yet-encoded grid nevertheless has a real owner. */
        ((void (*)(void *, void *))native_0x30ab9)(nan, map);
        ((void (*)(void *, void *))native_0x30a55)(nan, map);
    }
}

/* Preparation owns new maps until all allocations succeed. 0x304f8 is safe
 * here only because its destructive failure cleanup sees a fresh map. */
static int prepare_request(void *dam, void *peer, void **maps, void **out,
                            uint16_t *slots)
{
    void *nan = PTR(dam, 4), *container = PTR(dam, 0x18);
    void *source = PTR(peer, 0x0c), *old = PTR(dam, 0x38);
    void *local = ((void *(*)(void *))native_0x3100d)(nan);
    if (!local || !source || !container || U8(dam, 0x10) != 1 ||
        !U8(source, 0) || U8(source, 0) > 2 || !PTR(dam, 0x28) ||
        U16(dam, 0x14) != 16 || U8(container, 4) != 16)
        return -2;
    /* A2 has no B0 FAW array at +0x30. Do not interpret its stats/owner
     * extension as that storage, or enable infrastructure concurrency. */
    if (U8(container, 0x0e) != 0xff ||
        (U32(PTR(dam, 8), 4) & 0x03000000u))
        return -23;
    if (U8(peer, 0x69))
        return -16;
    if (old && (U8(old, 0) != 1 || old == source || old == local ||
                old == PTR(peer, 0x4c) || PTR(old, 8) || PTR(old, 0x0c) ||
                PTR(old, 0x18) == PTR(container, 8) ||
                ((int (*)(void *))native_0x20845)(dam)))
        return -16;
    void *negotiated = PTR(peer, 0x4c);
    if (negotiated && (U8(negotiated, 0) != 1 || negotiated == source ||
                       negotiated == local || negotiated == old))
        return -16;
    void *negotiated_grid = negotiated ? PTR(negotiated, 0x18) : 0;
    if (negotiated_grid && (negotiated_grid == PTR(container, 8) ||
                            (old && negotiated_grid == PTR(old, 0x18))))
        return -16;
    for (unsigned i = 0; i < U8(source, 0); ++i) {
        void *grid = PTR(source, 0x18 + i * 20);
        if (grid && ((old && grid == PTR(old, 0x18)) || grid == negotiated_grid))
            return -16;
    }
    /* Receive parsing supplies actual peer grids. An OOB peer with no
     * advertised map must not be turned into an invented channel offer. */
    if (!((unsigned (*)(void *, void *))native_0x31339)(nan, source))
        return -25;
    uint16_t period = 0;
    int error = ((int (*)(void *, void *, uint16_t *))native_0x32155)(nan, peer, &period);
    if (error)
        return error; /* Earlier successful growth still owns its replacements. */
    if (period < 512 || period > 8192 || (period & (period - 1)))
        return -2;
    void *proposal = ((void *(*)(void *, unsigned))native_0x303ad)(nan, 1);
    if (!proposal)
        return -27;
    error = ((int (*)(void *, void *, unsigned, unsigned))native_0x304f9)(
        nan, proposal, 16, period);
    if (!error && !negotiated) {
        negotiated = ((void *(*)(void *, unsigned))native_0x303ad)(nan, 1);
        if (!negotiated)
            error = -27;
        else
            U8(negotiated, 0x14) = 0;
    }
    if (error) {
        free_map(nan, proposal);
        return error;
    }
    PTR(dam, 0x38) = proposal;
    PTR(peer, 0x4c) = negotiated;
    nan_dam_transition_reset(dam);
    free_map(nan, old);
    return response_arrays(dam, peer, maps, out, slots);
}

static int host_result(void *dam, void *result, unsigned request)
{
    void *nan = PTR(dam, 4);
    void *map = ((void *(*)(void *))native_0x3100d)(nan);
    void *table = ((void *(*)(void *))native_0x33a5d)(PTR(nan, 0x4c));
    unsigned bitmap = U16(dam, 0x22), index = U8(dam, 0x24);
    if (!map || !table || !bitmap ||
        (request && (index >= 16 || !(bitmap & (1u << index)))))
        return -1;
    PTR(result, 8) = map;
    PTR(result, 0x0c) = table;
    U16(result, 0x10) = bitmap;
    U8(result, 0x12) = request ? index : 0xff;
    U32(result, 0) = request;
    U16(result, 4) = 0;
    return 0;
}

static int negotiate(void *dam, unsigned type, void *peer, void *result,
                      unsigned request)
{
    int error = context(dam, peer, result, type);
    if (error || type != request * 2)
        return reject(result, 0x100, error ? error : -23);
    unsigned mode = U8(PTR(dam, 8), 0);
    if (!mode) {
        error = host_result(dam, result, request);
        return error ? reject(result, 0x100, error) : 0;
    }
    if (mode != 3)
        return reject(result, 0x100, -23);
    void *maps[2], *out[2];
    uint16_t slots;
    error = prepare_request(dam, peer, maps, out, &slots);
    if (error)
        return reject(result, 0x100, error);
    void *local = PTR(PTR(dam, 0x18), 8);
    unsigned committed = 0, counter = 0;
    void *attribute = PTR(PTR(peer, 0x0c), 8);
    if (request && attribute) {
        if ((U8(attribute, 1) | (U8(attribute, 2) << 8)) < 7)
            return reject(result, 1, -2);
        committed = U8(attribute, 9) & 1u;
    }
    if (request) {
        error = ((int (*)(void *, unsigned, void *, void **, void **, void **,
                          unsigned, unsigned))native_0x20b61)(
            dam, type, peer, &local, maps, out, committed, slots);
        if (error == -1) { /* Negotiation conflict; malformed state is fatal. */
            error = ((int (*)(void *, unsigned, void *, void **, void **,
                              void **, unsigned))native_0x209f5)(
                dam, type, peer, &local, maps, out, slots);
            counter = 1;
        }
    } else {
        error = nan_dam_generate_proposal(dam, type, peer, &local, maps, out, slots);
    }
    if (error)
        return reject(result, 1, error);
    void *nan = PTR(dam, 4), *proposal = PTR(dam, 0x38);
    ((void (*)(void *, void *))native_0x30871)(nan, PTR(dam, 0x28));
    error = ((int (*)(void *, void *, void **, unsigned, unsigned,
                      unsigned, unsigned))native_0x31ea5)(
        nan, proposal, out, 1, !request || counter ? 0x23 : 0x21, 0, 1);
    if (error)
        return reject(result, 1, error);
    if (!PTR(peer, 0x5c) || committed)
        error = nan_dam_update_ndc_entry(dam, peer, committed && !counter);
    if (error)
        return reject(result, 0x101, error);
    int index = ((int (*)(void *, void *))native_0x31a41)(nan, PTR(peer, 0x5c));
    if (index < 0 || index >= 16)
        return reject(result, 0x101, -30);
    PTR(result, 8) = proposal;
    PTR(result, 0x0c) = ((void *(*)(void *))native_0x33a5d)(PTR(nan, 0x4c));
    U16(result, 0x10) = 1u << index;
    U8(result, 0x12) = request ? (unsigned)index : 0xff;
    U32(result, 0) = request && !counter;
    U16(result, 4) = 0;
    ((void (*)(void *, unsigned))native_0x31d6d)(nan, 0x10);
    return 0;
}

int nan_dam_get_proposal(void *dam, unsigned type, void *peer, void *result)
{
    return negotiate(dam, type, peer, result, 0);
}

int nan_dam_process_request(void *dam, unsigned type, void *peer, void *result)
{
    return negotiate(dam, type, peer, result, 1);
}

int nan_dam_process_counter(void *dam, unsigned type, void *peer, void *result)
{
    int error = context(dam, peer, result, type);
    if (error)
        return reject(result, 0x100, error);
    if (U8(PTR(dam, 8), 0) != 3 || type != 3)
        return reject(result, 0x100, -23);
    void *maps[2], *out[2];
    uint16_t slots;
    error = response_arrays(dam, peer, maps, out, &slots);
    if (error)
        return reject(result, 1, error);
    void *local = PTR(PTR(dam, 0x18), 8);
    error = ((int (*)(void *, unsigned, void *, void **, void **, void **,
                      unsigned))native_0x209f5)(dam, type, peer, &local, maps, out, slots);
    if (error)
        return reject(result, 1, error);
    void *nan = PTR(dam, 4), *proposal = PTR(dam, 0x38);
    ((void (*)(void *, void *))native_0x30871)(nan, PTR(dam, 0x28));
    error = ((int (*)(void *, void *, void **, unsigned, unsigned,
                      unsigned, unsigned))native_0x31ea5)(nan, proposal, out, 1, 0x21, 0, 1);
    if (error)
        return reject(result, 1, error);
    error = nan_dam_update_ndc_entry(dam, peer, 1);
    if (error)
        return reject(result, 0x101, error);
    int index = ((int (*)(void *, void *))native_0x31a41)(nan, PTR(peer, 0x5c));
    if (index < 0 || index >= 16)
        return reject(result, 0x101, -30);
    PTR(result, 8) = proposal;
    PTR(result, 0x0c) = ((void *(*)(void *))native_0x33a5d)(PTR(nan, 0x4c));
    U16(result, 0x10) = 1u << index;
    U8(result, 0x12) = index;
    U32(result, 0) = 1;
    U16(result, 4) = 0;
    ((void (*)(void *, unsigned))native_0x31d6d)(nan, 0x10);
    return 0;
}

int nan_dam_process_response(void *dam, unsigned type, void *peer, void *result,
                              unsigned status)
{
    int error = context(dam, peer, result, type);
    if (error || status > 2)
        return reject(result, 0x100, error ? error : -2);
    unsigned mode = U8(PTR(dam, 8), 0);
    if (mode != 0 && mode != 3)
        return reject(result, 0x100, -23);
    if (status == 2)
        return reject(result, U16(result, 4) ? U16(result, 4) : 1, -1);
    void *nan = PTR(dam, 4);
    if (mode == 0) {
        if (status == 1) {
            U32(result, 0) = 1;
            U16(result, 4) = 0;
            return 0;
        }
        void *local = ((void *(*)(void *))native_0x3100d)(nan);
        if (!local)
            return reject(result, 0x100, -1);
        if (type == 3 && !U16(dam, 0x22))
            return reject(result, 0x101, -1);
        PTR(result, 8) = local;
        PTR(result, 0x0c) = ((void *(*)(void *))native_0x33a5d)(PTR(nan, 0x4c));
        U16(result, 0x10) = U16(dam, 0x22);
        U8(result, 0x12) = U8(dam, 0x24);
        U32(result, 0) = 1;
        U16(result, 4) = 0;
        return 0;
    }
    if (!status)
        return type == 3 ? nan_dam_process_counter(dam, type, peer, result) : 0;
    void *map = PTR(peer, 0x0c), *attribute = map ? PTR(map, 8) : 0;
    if ((type == 3 || type == 4) &&
        (!attribute || (U8(attribute, 1) | (U8(attribute, 2) << 8)) < 7 ||
         !(U8(attribute, 9) & 1u)))
        return reject(result, 3, -1);
    void *maps[2], *out[2];
    uint16_t slots;
    error = response_arrays(dam, peer, maps, out, &slots);
    if (!error)
        error = ((int (*)(void *, unsigned, void **, void **, unsigned))native_0x2187d)(
            dam, type, maps, out, slots);
    if (!error)
        error = nan_dam_update_ndc_entry(dam, peer, 1);
    if (error)
        return reject(result, 1, error);
    U32(result, 0) = 1;
    U16(result, 4) = 0;
    return 0;
}

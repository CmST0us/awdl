#include "native_calls.h"
/* A2 single-NAN schedule installation. Build reference changes on independent
 * grids, then publish them once. A failed native commit keeps the new owners
 * and a retry record; it must not apply another NDP reference on retry. */
#include <stdint.h>
#include "nan_dam.h"
#include "nan_schedule.h"

#define PTR(p, o) (*(void **)((uint8_t *)(p) + (o)))
#define U8(p, o) (*(uint8_t *)((uint8_t *)(p) + (o)))
#define U16(p, o) (*(uint16_t *)((uint8_t *)(p) + (o)))
#define U32(p, o) (*(uint32_t *)((uint8_t *)(p) + (o)))

enum { ACT_PENDING = 1, ACT_DONE, RELEASE_PENDING, RELEASE_DONE,
       STOP_PENDING, STOP_DONE };
struct transition {
    void *peer, *proposal, *session;
    uint32_t state, usage;
};
_Static_assert(sizeof(struct transition) == 20, "private A2 DAM tail");
_Static_assert(NAN_DAM_BYTES == 0x60 + sizeof(struct transition), "DAM size");

static struct transition *transition(void *dam)
{
    return (struct transition *)((uint8_t *)dam + 0x60);
}

int nan_dam_transition_pending(void *dam)
{
    unsigned state = transition(dam)->state;
    return (state & 1u) || state > STOP_DONE;
}

void nan_dam_transition_reset(void *dam)
{
    struct transition *t = transition(dam);
    t->peer = t->proposal = t->session = 0;
    t->state = t->usage = 0;
}

/* All callers copy complete, disjoint native shadow objects/maps; lengths
 * are multiples of four. Share the native copy routine with AWDL. */
static void copy_words(void *dest, const void *source, unsigned bytes)
{
    ((void *(*)(void *, const void *, unsigned))native_0x151a21)(dest, source, bytes);
}

static int grid_matches(void *grid, unsigned slots)
{
    return grid && U16(grid, 0) == slots && U16(grid, 2) == slots * 16 &&
           U8(grid, 4) == 16;
}

static int valid_context(void *dam, void *peer)
{
    if (!dam || !peer || !PTR(dam, 4) || !PTR(dam, 8))
        return -2;
    void *nan = PTR(dam, 4), *container = PTR(dam, 0x18);
    void *availability = PTR(nan, 0x4c);
    unsigned period = U16(dam, 0x12);
    if (PTR(nan, 0x5c) != dam || PTR(dam, NAN_DAM_HOST_PEERS))
        return -16;
    if (!availability || U8(availability, 0x26) != 8 || U8(availability, 0x27) != 4)
        return -30;
    if (U8(PTR(dam, 8), 0) != 3 || !container || U8(dam, 0x10) != 1 ||
        U8(container, 0x0e) != 0xff || U8(peer, 0x69))
        return -23; /* Single physical NAN; no infrastructure or ranging. */
    if (period < 512 || period > 8192 || (period & (period - 1)) ||
        U16(dam, 0x14) != 16 || U16(container, 2) != period ||
        U16(container, 0) != period / 16 || U8(container, 4) != 16 ||
        !grid_matches(PTR(container, 8), period / 16))
        return -2;
    return 0;
}

static void *clone_timeline(void *dam)
{
    unsigned slots = U16(dam, 0x12) / 16;
    void *old = PTR(dam, 0x18);
    void *next = ((void *(*)(void *, unsigned, unsigned))native_0x201bd)(
        dam, 16, slots * 16);
    if (next) {
        void *grid = PTR(next, 8);
        copy_words(next, old, 16 + 4 * slots);
        copy_words(grid, PTR(old, 8), 8 + 4 * slots);
        PTR(next, 8) = grid;
    }
    return next;
}

static void free_timeline(void *dam, void *container)
{
    if (container)
        ((void (*)(void *, void *))native_0x2020d)(dam, container);
}

/* Native 0x20114 groups by channel only, losing a differing conditional bit
 * in an adjacent entry. Check each reference first and group only identical
 * channels AND ownership flags; commit is deferred until all work succeeds. */
static int apply_refs(void *dam, void *grid, unsigned count, unsigned operation)
{
    if (!count)
        return 0;
    unsigned slots = U16(dam, 0x12) / 16;
    if (!grid_matches(grid, slots) || count > 255)
        return -30;
    void *container = PTR(dam, 0x18), *local = PTR(container, 8);
    for (unsigned i = 0; i < slots; ++i) {
        unsigned offset = 8 + 4 * i, flags = U8(grid, offset);
        if ((flags & 0x41u) != 0x41)
            continue;
        unsigned channel = U16(grid, offset + 2);
        unsigned committed = U8(container, 16 + 4 * i + 2);
        unsigned conditional = U8(container, 16 + 4 * i);
        if ((U8(local, offset) & 1u) &&
            !((int (*)(unsigned, unsigned))native_0xbd71)(U16(local, offset + 2), channel))
            return operation == 2 ? -16 : -30;
        if (operation == 2) {
            if (committed + count > 255 || ((flags & 0x10u) && conditional + count > 255))
                return -16;
        } else if (!(U8(local, offset) & 1u) || committed < count ||
                   ((flags & 0x10u) && conditional < count)) {
            return -30;
        }
    }
    for (unsigned i = 0; i < slots;) {
        unsigned flags = U8(grid, 8 + 4 * i);
        if ((flags & 0x41u) != 0x41) {
            ++i;
            continue;
        }
        unsigned channel = U16(grid, 10 + 4 * i), end = i;
        flags &= 0x11u;
        while (end + 1 < slots &&
               (U8(grid, 8 + 4 * (end + 1)) & 0x51u) == (flags | 0x40u) &&
               U16(grid, 10 + 4 * (end + 1)) == channel)
            ++end;
        for (unsigned n = 0; n < count; ++n) {
            int error = nan_dam_update_schedule(dam, 0, i, end, channel,
                operation == 2 ? flags : (flags & 0x10u) | 0x20u, operation, 0);
            if (error)
                return error;
        }
        i = end + 1;
    }
    return 0;
}

static int commit_transition(void *dam)
{
    struct transition *t = transition(dam);
    int error = ((int (*)(void *, unsigned))native_0x21771)(dam, 1);
    if (!error) {
        if (t->state == ACT_PENDING)
            U16(t->peer, 0) |= 2u;
        ++t->state;
    }
    return error;
}

static void begin_transition(void *dam, void *peer, unsigned state)
{
    struct transition *t = transition(dam);
    t->peer = peer;
    t->proposal = PTR(dam, 0x38);
    t->session = PTR(peer, 0x48);
    t->usage = U8(peer, 0x68);
    t->state = state;
}

int nan_dam_activate(void *dam, void *peer)
{
    if (!dam || !peer || !PTR(dam, 8))
        return -2;
    if (!U8(PTR(dam, 8), 0))
        return PTR(peer, 0x4c) ? -16 : 0;
    int error = valid_context(dam, peer);
    if (error)
        return error;
    struct transition *t = transition(dam);
    if (t->peer == peer && t->proposal == PTR(dam, 0x38) &&
        t->session == PTR(peer, 0x48)) {
        if (t->state == ACT_PENDING)
            return commit_transition(dam);
        if (t->state == ACT_DONE)
            return 0;
    }
    if (nan_dam_transition_pending(dam))
        return -16;
    unsigned slots = U16(dam, 0x12) / 16, usage = U8(peer, 0x68);
    void *nan = PTR(dam, 4), *map = PTR(peer, 0x4c), *proposal = PTR(dam, 0x38);
    void *source = proposal ? PTR(proposal, 0x18) : 0;
    void *old_grid = map ? PTR(map, 0x18) : 0;
    if (!map || !proposal || U8(map, 0) != 1 || U8(proposal, 0) != 1 ||
        !grid_matches(source, slots) || (old_grid && !grid_matches(old_grid, slots)))
        return -2;
    if (map == proposal || map == PTR(peer, 0x0c) ||
        source == old_grid || source == PTR(PTR(dam, 0x18), 8) ||
        old_grid == PTR(PTR(dam, 0x18), 8) || PTR(proposal, 8) || PTR(proposal, 0x0c))
        return -16;
    void *advertised = PTR(peer, 0x0c);
    if (advertised && U8(advertised, 0) > 2)
        return -2;
    for (unsigned i = 0; advertised && i < U8(advertised, 0); ++i) {
        void *grid = PTR(advertised, 0x18 + 20 * i);
        if (grid && (grid == source || grid == old_grid))
            return -16;
    }
    unsigned accepted = 0;
    for (unsigned i = 0; i < slots; ++i)
        accepted |= U8(source, 8 + 4 * i) & 0x40u;
    if (!accepted || (!old_grid && usage))
        return -30;
    void *session = PTR(peer, 0x48);
    unsigned ndl_only = session &&
        (((int (*)(void *, void *))native_0x28695)(nan, session) || U32(session, 0x28) == 5) &&
        !PTR(session, 0x14);
    unsigned next_usage = usage + !ndl_only;
    if (next_usage > 255)
        return -16;
    void *next = clone_timeline(dam);
    if (!next)
        return -27;
    void *grid = ((void *(*)(void *, unsigned, unsigned))native_0x305f5)(nan, 16, slots * 16);
    if (!grid) {
        free_timeline(dam, next);
        return -27;
    }
    uint32_t shadow_dam[NAN_DAM_BYTES / 4], shadow_peer[0x88 / 4], shadow_map[10];
    copy_words(shadow_dam, dam, NAN_DAM_BYTES);
    copy_words(shadow_peer, peer, 0x88);
    copy_words(shadow_map, map, 40);
    PTR(shadow_dam, 0x18) = next;
    U8(shadow_dam, 0x25) = 0;
    error = apply_refs(shadow_dam, old_grid, usage, 3);
    if (!error) {
        ((void (*)(void *, void **, void **, unsigned))native_0x20981)(
            shadow_dam, &grid, &source, slots);
        PTR(shadow_map, 0x18) = grid;
        PTR(shadow_peer, 0x4c) = shadow_map;
        if (U8(next, 0x0f)) {
            error = nan_dam_update_for_excursion(shadow_dam, shadow_peer, 0, 3, 0);
            if (error > 0)
                error = 0;
        }
        if (!error)
            error = apply_refs(shadow_dam, grid, next_usage, 2);
    }
    if (error) {
        ((void (*)(void *, void **))native_0x30af9)(nan, &grid);
        free_timeline(dam, next);
        return error;
    }
    void *old = PTR(dam, 0x18);
    PTR(dam, 0x18) = next;
    PTR(map, 0x18) = grid;
    begin_transition(dam, peer, ACT_PENDING);
    transition(dam)->usage = next_usage;
    ((void (*)(void *, void **))native_0x30af9)(nan, &old_grid);
    free_timeline(dam, old);
    return commit_transition(dam);
}

static int release(void *dam, void *peer, unsigned stop)
{
    int error = valid_context(dam, peer);
    if (error)
        return error;
    struct transition *t = transition(dam);
    unsigned pending = stop ? STOP_PENDING : RELEASE_PENDING;
    if (t->peer == peer && t->usage == U8(peer, 0x68)) {
        if (t->state == pending)
            return commit_transition(dam);
        if (t->state == pending + 1)
            return 0;
    }
    /* A failed activation may be cancelled before the native NDP count
     * advances. The installed reference is real and must be released. */
    unsigned count = 1;
    if (t->peer == peer && (t->state == ACT_PENDING || t->state == ACT_DONE)) {
        if (!t->usage && stop)
            count = 0; /* NDL-only update installed no NDP reference. */
        else if (t->state == ACT_PENDING && t->usage != U8(peer, 0x68) + 1u)
            return -30;
    } else if (nan_dam_transition_pending(dam)) {
        return -16;
    }
    void *map = PTR(peer, 0x4c), *grid = map ? PTR(map, 0x18) : 0;
    if (!map || U8(map, 0) != 1 || !grid_matches(grid, U16(dam, 0x12) / 16))
        return -30;
    if (grid == PTR(PTR(dam, 0x18), 8))
        return -16;
    void *next = clone_timeline(dam);
    if (!next)
        return -27;
    uint32_t shadow[NAN_DAM_BYTES / 4];
    copy_words(shadow, dam, NAN_DAM_BYTES);
    PTR(shadow, 0x18) = next;
    U8(shadow, 0x25) = 0;
    error = apply_refs(shadow, grid, count, 3);
    if (error) {
        free_timeline(dam, next);
        return error;
    }
    void *old = PTR(dam, 0x18);
    PTR(dam, 0x18) = next;
    begin_transition(dam, peer, pending);
    free_timeline(dam, old);
    return commit_transition(dam);
}

int nan_dam_release_session(void *dam, void *peer)
{
    return release(dam, peer, 0);
}

int nan_dam_deactivate_complete(void *nan, void *peer)
{
    if (!nan || !peer || !PTR(nan, 0x5c))
        return -2;
    void *dam = PTR(nan, 0x5c), *map = PTR(peer, 0x4c), *ndc = PTR(peer, 0x5c);
    if (map && (map == PTR(dam, 0x38) || map == PTR(peer, 0x0c) ||
                map == ((void *(*)(void *))native_0x3100d)(nan)))
        return -16;
    if (U8(peer, 0x68) || U8(peer, 0x69))
        return -16;
    struct transition *t = transition(dam);
    if (t->peer == peer && (t->state == ACT_PENDING || t->state == ACT_DONE)) {
        if (!map || t->usage > 1)
            return -30;
        /* Covers a native session abort after installation but before its
         * NDP activation count advances, including attribute/MSCH errors. */
        return nan_dam_deactivate(dam, peer);
    }
    if (nan_dam_transition_pending(dam))
        return -16;
    /* A2 0x32154 refreshes the existing NDC into map+c as a borrow. It
     * never acquires a second reference. Reject any unrelated owner. */
    if (map && PTR(map, 0x0c) && PTR(map, 0x0c) != ndc)
        return -30;
    if (ndc) {
        int error = ((int (*)(void *, void *))native_0x31cfd)(nan, ndc);
        if (error)
            return error;
        PTR(peer, 0x5c) = 0;
    }
    if (map) {
        PTR(map, 0x0c) = 0; /* End the known borrow after its owner releases. */
        ((void (*)(void *, void *))native_0x30ab9)(nan, map);
        ((void (*)(void *, void *))native_0x30a55)(nan, map);
        PTR(peer, 0x4c) = 0;
    }
    U16(peer, 0) &= ~2u;
    if (transition(dam)->peer == peer)
        nan_dam_transition_reset(dam);
    ((void (*)(void *, unsigned))native_0x31d6d)(nan, 1);
    return 0;
}

int nan_dam_deactivate(void *dam, void *peer)
{
    if (!dam || !peer || !PTR(dam, 4) || !PTR(dam, 8))
        return -2;
    if (!U8(PTR(dam, 8), 0))
        return PTR(peer, 0x4c) ? -16 : 0;
    if (U8(PTR(dam, 8), 0) != 3)
        return -23;
    if (U8(peer, 0x68) || U8(peer, 0x69))
        return -16;
    struct transition *t = transition(dam);
    if (t->peer == peer && !t->usage &&
        (t->state == RELEASE_PENDING || t->state == RELEASE_DONE)) {
        int error = t->state == RELEASE_PENDING ? commit_transition(dam) : 0;
        return error ? error : nan_dam_deactivate_complete(PTR(dam, 4), peer);
    }
    int error = PTR(peer, 0x4c) ? release(dam, peer, 1) : 0;
    return error ? error : nan_dam_deactivate_complete(PTR(dam, 4), peer);
}

/* Original 0x34278 ignores a direct completion error. Keep participation
 * and NDL ownership until the actual DAM transition has completed. */
int nan_dam_deactivate_ndl(void *ndl, void *peer, unsigned apply)
{
    if (!ndl || !peer || !PTR(ndl, 8) || apply > 1)
        return -2;
    void *nan = PTR(ndl, 8), *dam = PTR(nan, 0x5c);
    if (!dam)
        return -2;
    unsigned index = ((unsigned (*)(void *, void *))native_0x37481)(nan, peer);
    if (index != 0xff && index >= 32)
        return -30;
    int error = apply ? nan_dam_deactivate(dam, peer) :
                        nan_dam_deactivate_complete(nan, peer);
    if (error)
        return error;
    if (PTR(peer, 0x2c))
        U32(PTR(peer, 0x2c), 0) = 0;
    if (index != 0xff) {
        U8(nan, 0x88 + index / 8) &= ~(1u << (index % 8));
        U8(ndl, 0x14 + index / 8) &= ~(1u << (index % 8));
    }
    return 0;
}

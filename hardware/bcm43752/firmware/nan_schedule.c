#include "native_calls.h"
/* Install host-negotiated schedules into A2's native availability, DAM and
 * MSCH consumers. No autonomous NDL/NDP negotiation or alternate radio mode. */
#include "nan_schedule.h"
#include "nan_scheduler.h"
#include "nan_peer.h"
#include "nan_host.h"
#include "nan_host_abi.h"

#define FIELD(type, base, offset) (*(type *)((uint8_t *)(base) + (offset)))
#define PTR(base, offset) FIELD(void *, base, offset)
#define U32(base, offset) FIELD(uint32_t, base, offset)
#define U16(base, offset) FIELD(uint16_t, base, offset)
#define U8(base, offset) FIELD(uint8_t, base, offset)

struct schedule_peer {
    struct schedule_peer *next;
    void *peer;
    uint8_t address[6];
};

static int failure(const char *stage, int error)
{
    if (error)
        ((int (*)(const char *, ...))native_0xa815)("NAN schedule %s: %d\n", stage, error);
    return error;
}

static unsigned get16(const uint8_t *p)
{
    return p[0] | (unsigned)p[1] << 8;
}

static int zero(const uint8_t *p, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i)
        if (p[i])
            return 0;
    return 1;
}

static int equal(const void *a, const void *b, size_t bytes)
{
    return !((int (*)(const void *, const void *, size_t))native_0xa7a9)(a, b, bytes);
}

static void copy(void *to, const void *from, size_t bytes)
{
    ((void *(*)(void *, const void *, size_t))native_0x151a21)(to, from, bytes);
}

static void release_map(void *nan, void *map)
{
    if (map)
        ((void (*)(void *, void *))native_0x30a55)(nan, map);
}

static int syntax(const uint8_t *input, size_t length, unsigned peer)
{
    if (length != BCM_NAN_HOST_SCHEDULE_SIZE ||
        get16(input) != BCM_NAN_HOST_VERSION || get16(input + 4) != length ||
        get16(input + 6) || !zero(input + 14, 10))
        return 0;
    if (peer) {
        if ((input[8] & 1) || zero(input + 8, 6))
            return 0;
        if (get16(input + 2) == BCM_NAN_HOST_REMOVE)
            return zero(input + 24, BCM_NAN_HOST_SCHEDULE_WIRE_GRID_SIZE);
    } else if (!zero(input + 8, 6)) {
        return 0;
    }
    if (get16(input + 2) != BCM_NAN_HOST_ADD)
        return 0;
    const uint8_t *grid = input + 24;
    if (get16(grid) != BCM_NAN_HOST_SCHEDULE_SLOTS ||
        get16(grid + 2) != BCM_NAN_HOST_SCHEDULE_PERIOD_TU ||
        grid[4] != BCM_NAN_HOST_SCHEDULE_SLOT_TU ||
        grid[5] > 15 || grid[7])
        return 0;
    unsigned used = 0;
    for (unsigned i = 0; i < BCM_NAN_HOST_SCHEDULE_SLOTS; ++i) {
        unsigned channel = get16(grid + 8 + i * 2);
        unsigned two_streams = (grid[BCM_NAN_HOST_SCHEDULE_NSS_OFFSET + i / 8] >> (i % 8)) & 1u;
        if (!channel) {
            if (two_streams)
                return 0;
        } else {
            unsigned band = channel & 0xc000, width = channel & 0x3800;
            /* Only explicit committed 2.4/5 GHz channels and real 1/2 NSS.
             * Native CLM validation below checks the complete chanspec. */
            if ((band != 0 && band != 0xc000) ||
                (width != 0x1000 && width != 0x1800 && width != 0x2000)) {
                return 0;
            }
            ++used;
        }
    }
    return used != 0;
}

int nan_schedule_validate(const struct nan_command_digest *digest, uint32_t action,
                          const uint8_t *input, size_t length,
                          uint8_t *output, size_t *output_length)
{
    int error = nan_command_validate(digest, action, input, length, output, output_length);
    if (error)
        return failure("command", error);
    const struct nan_command *cmd = ((const struct nan_command *const *)digest)[3];
    if (syntax(input, length, cmd->id == BCM_NAN_HOST_PEER_SCHEDULE_COMMAND))
        return 0;
    return failure("wire", -2);
}

static int make_map(void *nan, const uint8_t *grid, void **result)
{
    /* Prepare the complete native map before replacing any live pointer.
     * The NA attribute and the channel schedule have one input source. */
    *result = 0;
    for (unsigned i = 0; i < BCM_NAN_HOST_SCHEDULE_SLOTS; ++i) {
        unsigned channel = get16(grid + 8 + i * 2);
        if (channel && !((int (*)(void *, unsigned))native_0xc815d)(PTR(nan, 0x2c), channel))
            return -20;
    }
    void *map = ((void *(*)(void *, unsigned))native_0x303ad)(nan, 1);
    if (!map)
        return -27;
    U8(map, 0x14) = grid[5]; /* Mark owned before nested allocation/free. */
    U8(map, 1) = grid[6];
    void *schedule = ((void *(*)(void *, unsigned, unsigned))native_0x305f5)(
        nan, BCM_NAN_HOST_SCHEDULE_SLOT_TU, BCM_NAN_HOST_SCHEDULE_PERIOD_TU);
    PTR(map, 0x18) = schedule;
    int error = -27;
    if (!schedule)
        goto fail;
    copy(schedule, grid, 8);
    for (unsigned i = 0; i < BCM_NAN_HOST_SCHEDULE_SLOTS; ++i) {
        unsigned channel = get16(grid + 8 + i * 2);
        uint8_t *slot = (uint8_t *)schedule + 8 + i * 4;
        slot[0] = channel != 0;
        slot[1] = channel ? 1 + ((grid[BCM_NAN_HOST_SCHEDULE_NSS_OFFSET + i / 8] >> (i % 8)) & 1u) : 0;
        U16(slot, 2) = channel;
    }
    /* A2 0x32238 derives the actual NAN Availability attribute, including
     * operating class, channel bitmap, bandwidth, NSS and time bitmap.
     * committed mask=1; no potential entries carried from an older map. */
    error = ((int (*)(void *, void *, void *, unsigned, unsigned, unsigned, unsigned))
             0x32239u)(nan, (uint8_t *)map + 0x14, schedule, 1, 1, 0, 0);
    if (error)
        goto fail;
    if (!PTR(map, 0x1c)) {
        error = -30;
        goto fail;
    }
    U8(PTR(map, 0x1c), 3) = grid[6];
    *result = map;
    return 0;
fail:
    release_map(nan, map);
    return error;
}

static struct schedule_peer **find(void *nan, const uint8_t *address)
{
    struct schedule_peer **slot = (struct schedule_peer **)(
        (uint8_t *)PTR(nan, 0x5c) + NAN_DAM_HOST_PEERS);
    while (*slot && !equal((*slot)->address, address, 6))
        slot = &(*slot)->next;
    return slot;
}

static int peer_present(void *nan, struct schedule_peer *owner)
{
    return ((void *(*)(void *, const void *, unsigned))native_0x37189)(
        nan, owner->address, 0) == owner->peer && U8(owner->peer, 9);
}

static int remove_peer(void *nan, struct schedule_peer **slot)
{
    struct schedule_peer *owner = *slot;
    if (!peer_present(nan, owner))
        return -30;
    /* NDP references and native data SCBs must finish before their schedule
     * disappears. The host's pin also prevents watchdog aging of this map. */
    if (U8(owner->peer, 9) != 1 || nan_host_peer_busy(nan, owner->address))
        return -16;
    --U8(owner->peer, 9);
    int error = nan_peer_release(nan, owner->peer);
    if (error) {
        ++U8(owner->peer, 9);
        return error;
    }
    *slot = owner->next;
    ((void (*)(void *))native_0x71491)(owner);
    return 0;
}

static int install_peer(void *nan, const uint8_t *address, void *map)
{
    struct schedule_peer **slot = find(nan, address);
    if (*slot && !peer_present(nan, *slot))
        return -30;
    if (!*slot) {
        struct schedule_peer *owner = ((void *(*)(size_t))native_0x7146d)(sizeof(*owner));
        if (!owner)
            return -27;
        void *peer = ((void *(*)(void *, const void *, unsigned))native_0x37189)(nan, address, 0);
        int error = 0;
        if (!peer)
            error = ((int (*)(void *, const void *, void **))native_0x36fb1)(nan, address, &peer);
        if (error || !peer || U8(peer, 9) == UINT8_MAX) {
            ((void (*)(void *))native_0x71491)(owner);
            return error ? error : -16;
        }
        owner->peer = peer;
        copy(owner->address, address, 6);
        ++U8(peer, 9);
        *slot = owner;
    }
    void *peer = (*slot)->peer;
    void *old = PTR(peer, 0x0c);
    PTR(peer, 0x0c) = map;
    release_map(nan, old);
    return 0;
}

static int refresh_beacons(void *nan)
{
    void *mac = PTR(nan, 0x14);
    void *old[2] = {PTR(mac, 0x3c), PTR(mac, 0x40)};
    void *next[2] = {0, 0};
    int error = 0;
    /* Native 0x2e57c returns void and frees the old template first. Retain
     * both old packets until both replacement builds really complete.
     * Its statistics counter advances only after successful body encoding. */
    for (unsigned i = 0; i < 2; ++i) {
        unsigned offset = 0x3c + i * 4;
        uint32_t count = U32(PTR(mac, 0x70), 0x0c);
        PTR(mac, offset) = 0;
        ((void (*)(void *, unsigned))native_0x2e57d)(mac, i + 1);
        next[i] = PTR(mac, offset);
        PTR(mac, offset) = old[i];
        if (!next[i] || U32(PTR(mac, 0x70), 0x0c) != count + 1) {
            error = next[i] ? -1 : -27;
            break;
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        void *discard = error ? next[i] : old[i];
        if (!error)
            PTR(mac, 0x3c + i * 4) = next[i];
        if (discard)
            ((void (*)(void *, void *, unsigned))native_0x151ae1)(
                PTR(PTR(nan, 8), 4), discard, 1);
    }
    return error;
}

int nan_schedule_command(const struct nan_command_digest *digest,
                         const uint8_t *input, size_t length,
                         uint8_t *output, size_t *output_length)
{
    (void)output;
    *output_length = 0;
    const struct nan_command *cmd = ((const struct nan_command *const *)digest)[3];
    unsigned peer = cmd->id == BCM_NAN_HOST_PEER_SCHEDULE_COMMAND;
    if (!syntax(input, length, peer))
        return -2;
    void *nan = ((void *const *)digest)[1];
    void *dam = PTR(nan, 0x5c), *av = PTR(nan, 0x4c);
    if (!dam || !av || !U32(dam, 0x58) || !U32(av, 0x38) ||
        !PTR(dam, 8) || U8(PTR(dam, 8), 0))
        return -25;
    if (peer && get16(input + 2) == BCM_NAN_HOST_REMOVE) {
        struct schedule_peer **slot = find(nan, input + 8);
        return *slot ? remove_peer(nan, slot) : -30;
    }
    if (!nan_host_active(nan) || !PTR(nan, 0x2c))
        return -25;
    if (peer && equal(input + 8, (uint8_t *)nan_host_nmi(nan) + 0x66, 6))
        return -2;
    void *map;
    int error = make_map(nan, input + 24, &map);
    if (error)
        return failure("map encoding", error);
    if (peer) {
        error = install_peer(nan, input + 8, map);
        if (error)
            release_map(nan, map);
        return error;
    }
    /* Preserve the old map if its actual MSCH request cannot be cancelled.
     * Once replaced, errors retain the new map, DAM storage and any native
     * partial MSCH owner. Retry/STOP works on that actual state. */
    error = nan_scheduler_unregister(nan);
    if (error) {
        release_map(nan, map);
        return failure("unregister", error);
    }
    void *old = PTR(av, 8);
    PTR(av, 8) = map;
    release_map(nan, old);
    void *dam_grid = PTR(dam, 0x18);
    if (!dam_grid || !PTR(dam_grid, 8))
        return -30;
    U8(PTR(dam_grid, 8), 5) = input[29];
    U8(PTR(dam_grid, 8), 6) = input[30];
    void *common = PTR(nan, 0x10);
    uint32_t beacon_flag = U32(common, 0x34) & 0x00200000u;
    /* 0x204dc sends a void early template notification before MSCH commits.
     * Defer that notification to the checked two-template rebuild below. */
    U32(common, 0x34) &= ~0x00200000u;
    error = ((int (*)(void *))native_0x22081)(nan);
    U32(common, 0x34) |= beacon_flag;
    if (error)
        return failure("DAM commit", error);
    /* A2 0x21c7c otherwise omits NA from beacons even with a valid map.
     * 0x2fc5c schedules scans; it is not a template refresh operation. */
    U32(common, 0x34) |= 0x00200000u; /* WL_NAN_CTRL_INCL_NA_IN_BCNS */
    return failure("beacon refresh", refresh_beacons(nan));
}

int nan_schedule_stop(void *nan)
{
    if (!nan || !PTR(nan, 0x5c))
        return 0;
    struct schedule_peer **slot = (struct schedule_peer **)(
        (uint8_t *)PTR(nan, 0x5c) + NAN_DAM_HOST_PEERS);
    while (*slot) {
        int error = remove_peer(nan, slot);
        if (error)
            return error;
    }
    return 0;
}

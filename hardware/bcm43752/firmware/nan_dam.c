/* A2 DAM state needed to execute host-negotiated availability. The native
 * initializer at 0x21b44 is empty in this image. A2 has one schedule pointer
 * at +0x18; B0 has two and shifts subsequent fields by four bytes. Never copy
 * the B0 layout or its 8-byte per-slot scheduling entries into A2 (4 bytes).
 */
#include <stddef.h>
#include <stdint.h>
#include "nan_dam.h"
#include "nan_mac.h"
#include "nan_scheduler.h"
#include "nan_schedule.h"
#include "nan_host_abi.h"

#define FIELD(type, base, offset) (*(type *)((uint8_t *)(base) + (offset)))
#define PTR(base, offset) FIELD(void *, base, offset)
#define U32(base, offset) FIELD(uint32_t, base, offset)
#define U16(base, offset) FIELD(uint16_t, base, offset)
#define U8(base, offset) FIELD(uint8_t, base, offset)
#define INITIALIZED 0x58

static void *allocate(size_t bytes)
{
    return ((void *(*)(size_t))0x7146du)(bytes);
}

static void release_slot(void *owner, size_t offset)
{
    if (PTR(owner, offset)) {
        ((void (*)(void *))0x71491u)(PTR(owner, offset));
        PTR(owner, offset) = 0;
    }
}

int nan_dam_context_attach(void *nan)
{
    if (!nan || !PTR(nan, 0x6c))
        return -2;
    if (PTR(nan, 0x5c))
        return -16;
    void *dam = allocate(NAN_DAM_BYTES); /* Native state + host/transition tail. */
    if (!dam)
        return -27;
    U32(dam, 0) = 0x5afec0de;
    PTR(dam, 4) = nan;
    U8(dam, 0x24) = 0xff; /* No selected NDC. Native 0x22020 uses 0xff. */
    PTR(nan, 0x5c) = dam;
    return 0;
}

int nan_dam_deinitialize(void *wlc, void *nan)
{
    (void)wlc;
    if (!nan)
        return -2;
    void *dam = PTR(nan, 0x5c);
    if (!dam)
        return 0;
    void *mac = PTR(nan, 0x14);
    void *availability = PTR(nan, 0x4c);
    if (U8(dam, 0x25) || PTR(dam, 0x1c) || PTR(dam, NAN_DAM_HOST_PEERS) ||
        PTR(dam, 0x60) ||
        (availability && PTR(availability, 0x0c)) ||
        (mac && (PTR(mac, NAN_MAC_BSS_MANAGER) ||
                 (PTR(mac, 0x38) && U8(PTR(mac, 0x38), 0x4a)))))
        return -16;
    void *stats = PTR(nan, 0x74);
    if (stats) {
        void *schedule_entry = (uint8_t *)stats + 5 * 12;
        void *dam_entry = (uint8_t *)stats + 6 * 12;
        int schedule_present = U32(schedule_entry, 0) || U32(schedule_entry, 4) ||
                               U32(schedule_entry, 8);
        int dam_present = U32(dam_entry, 0) || U32(dam_entry, 4) || U32(dam_entry, 8);
        if ((schedule_present && (U32(schedule_entry, 0) != 0x2022du ||
                                  U32(schedule_entry, 4) || U32(schedule_entry, 8))) ||
            (dam_present && (U32(dam_entry, 0) ||
                             PTR(dam_entry, 4) != PTR(dam, 0x48) ||
                             U32(dam_entry, 8) != 0x06010004u)))
            return -30;
        ((void (*)(void *, size_t))0x18660bu)(schedule_entry, 24);
    }
    if (PTR(dam, 0x18)) {
        ((void (*)(void *, void *))0x2020du)(dam, PTR(dam, 0x18));
        PTR(dam, 0x18) = 0;
    }
    for (size_t offset = 0x0c; offset <= 0x38; offset += 0x2c) {
        if (PTR(dam, offset)) {
            /* A failed proposal may own a grid before assigning its map ID;
             * native attribute invalidation alone skips that allocation. */
            ((void (*)(void *, void *))0x30ab9u)(nan, PTR(dam, offset));
            ((void (*)(void *, void *))0x30a55u)(nan, PTR(dam, offset));
            PTR(dam, offset) = 0;
        }
    }
    const uint8_t storage[] = {8, 0x28, 0x34, 0x44, 0x48};
    for (size_t i = 0; i < sizeof(storage); ++i)
        release_slot(dam, storage[i]);
    U8(dam, 0x10) = 0;
    U16(dam, 0x22) = 0;
    U8(dam, 0x24) = 0xff;
    U32(dam, INITIALIZED) = 0;
    return 0;
}

int nan_dam_initialize(void *wlc, void *nan)
{
    if (!wlc || !nan || !PTR(nan, 0x5c) || !PTR(nan, 0x4c) ||
        !PTR(nan, 0x74) || !PTR(nan, 0x6c))
        return -2;
    void *dam = PTR(nan, 0x5c);
    void *cfg = PTR(nan, 0x6c);
    if (!U8(cfg, 0x0d) || !U8(cfg, 0x0e))
        return -2;
    if (U32(dam, INITIALIZED) || PTR(dam, 8) || PTR(dam, 0x18) ||
        PTR(dam, 0x0c) || PTR(dam, 0x28) || PTR(dam, 0x34) ||
        PTR(dam, 0x38) || PTR(dam, 0x44) || PTR(dam, 0x48))
        return -16;
    /* Stats entries are borrowed storage, not space this module may overwrite. */
    void *stats = (uint8_t *)PTR(nan, 0x74) + 5 * 12;
    for (size_t i = 0; i < 24; i += 4)
        if (U32(stats, i))
            return -16;
    int error = -27;
    PTR(dam, 8) = allocate(8);
    if (!PTR(dam, 8))
        goto fail;
    /* Mode zero uses the host's native availability map (A2 getters and
     * enable use 0x3100c); 0x21dec treats mode three as automatic DAM.
     * One physical timeline retains all 512 native 16 TU slots. Peer maps
     * must not lose alternating DW windows to a 512 TU intersection. */
    U8(PTR(dam, 8), 0) = 0;
    U8(PTR(dam, 8), 1) = 1;
    U16(PTR(dam, 8), 2) = 0x800; /* Original internal config default. */
    U16(dam, 0x40) = 0x80; /* Original internal fast-discovery default. */
    error = ((int (*)(void *, unsigned, unsigned, unsigned))0x2128du)(
        dam, 1, BCM_NAN_HOST_SCHEDULE_SLOT_TU, BCM_NAN_HOST_SCHEDULE_PERIOD_TU);
    if (error)
        goto fail;
    error = -27;
    PTR(dam, 0x0c) = ((void *(*)(void *, unsigned))0x303adu)(nan, 1);
    if (!PTR(dam, 0x0c))
        goto fail;
    PTR(dam, 0x28) = ((void *(*)(void *, unsigned))0x30645u)(nan, U8(cfg, 0x0e));
    if (!PTR(dam, 0x28))
        goto fail;
    PTR(dam, 0x34) = ((void *(*)(void *, unsigned))0x30645u)(nan, U8(cfg, 0x0d));
    if (!PTR(dam, 0x34))
        goto fail;
    PTR(dam, 0x44) = allocate(3);
    if (!PTR(dam, 0x44))
        goto fail;
    PTR(dam, 0x48) = allocate(4);
    if (!PTR(dam, 0x48))
        goto fail;
    void (*register_stats)(void *, unsigned, void *, uint16_t, uint16_t, uintptr_t) =
        (void (*)(void *, unsigned, void *, uint16_t, uint16_t, uintptr_t))0x1f75du;
    register_stats(nan, 5, 0, 0, 0, 0x2022du);
    register_stats(nan, 6, PTR(dam, 0x48), 4, 0x601, 0);
    U32(dam, INITIALIZED) = 1;
    return 0;
fail:;
    int cleanup = nan_dam_deinitialize(wlc, nan);
    return cleanup ? cleanup : error;
}

int nan_dam_context_detach(void *nan)
{
    if (!nan)
        return -2;
    int error = nan_dam_deinitialize(PTR(nan, 8), nan);
    if (!error)
        release_slot(nan, 0x5c);
    return error;
}

int nan_dam_disable(void *wlc, void *nan)
{
    (void)wlc;
    if (!nan || !PTR(nan, 0x5c))
        return -2;
    void *dam = PTR(nan, 0x5c);
    /* ROM 0x21b74 clears enabled and frees the active map before asking
     * MSCH to stop. Preserve both until the scheduler confirms removal. */
    int error = nan_scheduler_unregister(nan);
    if (error)
        return error;
    if (PTR(dam, 0x1c)) {
        ((void (*)(void *, void *))0x30a55u)(nan, PTR(dam, 0x1c));
        PTR(dam, 0x1c) = 0;
    }
    /* The host-scheduled mode has no automatic proposal map. The original
     * disable dereferences this field unconditionally. */
    if (PTR(dam, 0x38))
        ((void (*)(void *, void *, int))0x317bdu)(nan, PTR(dam, 0x38), -1);
    U8(dam, 0x25) = 0;
    return 0;
}

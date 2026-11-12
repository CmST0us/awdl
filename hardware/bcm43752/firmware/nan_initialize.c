#include "native_calls.h"
/* A2 host-owned NAN runtime lifecycle. The ROM common deinitializer skips
 * partial initialization and discards module/unregister errors. Keep those
 * errors and their resources until a real retry can finish cleanup. */
#include <stddef.h>
#include <stdint.h>
#include "nan_initialize.h"
#include "nan_mac.h"
#include "nan_discovery.h"
#include "nan_availability.h"
#include "nan_dam.h"
#include "nan_scheduler.h"
#include "nan_data.h"
#include "nan_peer.h"
#include "nan_link.h"
#include "nan_fsm.h"

#define FIELD(type, base, offset) (*(type *)((uint8_t *)(base) + (offset)))
#define PTR(base, offset) FIELD(void *, base, offset)
#define U32(base, offset) FIELD(uint32_t, base, offset)
#define U16(base, offset) FIELD(uint16_t, base, offset)
#define U8(base, offset) FIELD(uint8_t, base, offset)

static int attached(void *wlc, void *nan)
{
    if (!wlc || !nan || PTR(nan, 8) != wlc || !PTR(wlc, 0) ||
        !PTR(PTR(wlc, 0), 4))
        return 0;
    const uint8_t slots[] = {0x10, 0x14, 0x18, 0x24, 0x28, 0x30, 0x34,
                              0x38, 0x48, 0x4c, 0x5c, 0x6c, 0x74, 0x94};
    for (size_t i = 0; i < sizeof(slots); ++i)
        if (!PTR(nan, slots[i]))
            return 0;
    void *common = PTR(nan, 0x10);
    return U16(PTR(nan, 0x6c), 0) && PTR(common, 0x38) &&
           PTR(PTR(common, 0x38), 4);
}

int nan_modules_initialize(void *wlc, void *nan)
{
    if (!attached(wlc, nan))
        return -25;
    /* Protocol/security negotiation belongs to hostap. The native NDPE
     * registry is initialized without enabling autonomous commands or RX. */
    int (*const initialize[])(void *, void *) = {
        nan_mac_initialize, nan_discovery_initialize, nan_dam_initialize,
        nan_availability_initialize, nan_peer_initialize, nan_ndl_initialize,
        nan_ndp_initialize, nan_fsm_initialize, nan_scheduler_initialize,
    };
    for (size_t i = 0; i < sizeof(initialize) / sizeof(initialize[0]); ++i) {
        int error = initialize[i](wlc, nan);
        if (error)
            return error;
    }
    return 0;
}

static int runtime_idle(void *wlc, void *nan)
{
    void *mac = PTR(nan, 0x14);
    void *scheduler = PTR(nan, 0x24);
    if (U8(PTR(PTR(wlc, 0), 4), 0x30) ||
        U8(PTR(mac, 0x38), 0x4a) || PTR(mac, 0x3c) || PTR(mac, 0x40) ||
        PTR(mac, 0xe4) || PTR(mac, 0x10c) || U32(mac, 0xb4) ||
        PTR(mac, NAN_MAC_HOST_TX) || U32(mac, NAN_MAC_HOST_TX_COUNT) || PTR(scheduler, 8) ||
        PTR(mac, NAN_MAC_HOST_STATIONS) ||
        U8(scheduler, 0x0c) || U8(PTR(nan, 0x5c), 0x25))
        return -16;
    int error = nan_data_check_idle(nan);
    if (!error)
        error = nan_peer_check_idle(nan);
    if (!error)
        error = nan_fsm_check_idle(nan);
    return error;
}

int nan_modules_deinitialize(void *wlc, void *nan)
{
    if (!attached(wlc, nan))
        return -25;
    int error = runtime_idle(wlc, nan);
    if (!error)
        error = nan_mac_remove_bsscfg(PTR(nan, 0x14));
    if (error)
        return error;
    /* Availability owns DAM references; all peer/NDI consumers must already
     * be gone. Stop on error so their dependencies remain allocated. */
    int (*const deinitialize[])(void *, void *) = {
        nan_fsm_deinitialize, nan_ndp_deinitialize, nan_ndl_deinitialize,
        nan_discovery_deinitialize, nan_peer_deinitialize,
        nan_availability_deinitialize, nan_dam_deinitialize,
        nan_scheduler_deinitialize, nan_mac_deinitialize,
    };
    for (size_t i = 0; i < sizeof(deinitialize) / sizeof(deinitialize[0]); ++i) {
        error = deinitialize[i](wlc, nan);
        if (error)
            return error;
    }
    return 0;
}

static void release_slot(void *owner, size_t offset)
{
    if (PTR(owner, offset)) {
        ((void (*)(void *))native_0x71491)(PTR(owner, offset));
        PTR(owner, offset) = 0;
    }
}

int nan_deinitialize(void *wlc, void *nan)
{
    if (!attached(wlc, nan))
        return -25;
    void *common = PTR(nan, 0x10);
    int error = runtime_idle(wlc, nan);
    if (error)
        return error; /* No cleanup started: keep STOP and owner commands usable. */
    /* Keep the native deinit-in-progress marker on failure. The shared
     * command validator permits cleanup retries but blocks other handlers. */
    U8(common, 0x30) = 1;
    error = nan_modules_deinitialize(wlc, nan);
    if (error)
        return error;
    if (PTR(common, 0x3c)) {
        error = ((int (*)(void *, void *))native_0x1ffc9)(nan, PTR(common, 0x3c));
        if (error)
            return error;
        PTR(common, 0x3c) = 0;
    }
    if (PTR(common, 0x24) || PTR(common, 0x28) || PTR(common, 0x2c))
        return -16;
    release_slot(common, 8);
    release_slot(common, 0x1c);
    ((void (*)(void *))native_0x1e3b5)(nan); /* Actual vendor-payload owner. */
    U32(common, 0x34) &= ~0x80000000u;
    U8(common, 0x20) = 0;
    U8(common, 0x30) = 0;
    return 0;
}

int nan_initialize(void *wlc, void *nan)
{
    if (!attached(wlc, nan))
        return -25;
    void *common = PTR(nan, 0x10);
    if (PTR(common, 8) || PTR(common, 0x1c) || PTR(common, 0x3c) ||
        U8(common, 0x20) || U8(common, 0x30) ||
        (U32(common, 0x34) & 0x80000000u))
        return -16;
    uint32_t flags = U32(common, 0x34), flags2 = U32(common, 0x44);
    int error = -27;
    PTR(common, 8) = ((void *(*)(size_t))native_0x7146d)(U16(PTR(nan, 0x6c), 0));
    if (!PTR(common, 8))
        goto failed;
    PTR(nan, 0x2c) = PTR(wlc, 0xac);
    U16(common, 0x14) = 1;
    U32(common, 0x0c) = 0xd2;
    U32(common, 0x10) = 0x50;
    PTR(common, 0x1c) = ((void *(*)(size_t))native_0x7146d)(16);
    if (!PTR(common, 0x1c))
        goto failed;
    /* The same six-byte native constant used by ROM 0x1e764. */
    for (size_t i = 0; i < 6; ++i)
        U8(PTR(common, 0x1c), i) = ((const uint8_t *)0x1530bau)[i];
    ((void (*)(void *))native_0x1e84d)(nan);
    error = nan_modules_initialize(wlc, nan);
    if (error)
        goto failed;
    PTR(common, 0x3c) = ((void *(*)(void *, unsigned, unsigned, unsigned,
                                   uintptr_t, uintptr_t, void *, unsigned))native_0x1ff41)(
        nan, 0xdd, 0x1e3f, 1, 0x1e649u, 0x1e5a5u, nan, 0);
    if (!PTR(common, 0x3c)) {
        error = -27;
        goto failed;
    }
    U32(common, 0x34) |= 0x80000000u;
    U8(common, 0x20) = 1;
    return 0;
failed: {
    int cleanup = nan_deinitialize(wlc, nan);
    if (cleanup)
        return cleanup;
    U32(common, 0x34) = flags;
    U32(common, 0x44) = flags2;
    return error;
}
}

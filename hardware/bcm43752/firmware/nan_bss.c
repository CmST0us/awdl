#include "native_calls.h"
/* A2 NAN BSSCFG creation and pre-enable unwind. The native 0x2e728 caller
 * dereferences NULL on create failure and ignores callback registration errors.
 * This preserves its 20/28-byte request ABI and its real slotted-BSS allocator.
 * No new wireless mode is created: both NAN band contexts borrow one BSSCFG.
 */
#include <stddef.h>
#include <stdint.h>
#include "nan_mac.h"
#include "nan_data.h"

#define FIELD(type, base, offset) (*(type *)((uint8_t *)(base) + (offset)))
#define PTR(base, offset) FIELD(void *, base, offset)
#define U32(base, offset) FIELD(uint32_t, base, offset)
#define U8(base, offset) FIELD(uint8_t, base, offset)

static void **entry(void *manager, uint32_t index)
{
    if (!manager || !PTR(manager, 4))
        return 0;
    void *table = PTR(PTR(manager, 4), 8);
    if (!table || !PTR(table, 0) || index >= U32(table, 8))
        return 0;
    return &((void **)PTR(table, 0))[index];
}

static int our_callbacks(void *node, void *mac)
{
    uint32_t *callbacks = PTR(node, 0x0c);
    return callbacks && callbacks[0] == (uintptr_t)PTR(mac, 4) &&
           callbacks[6] == 0x385a5u;
}

/* Native node allocation publishes the node before allocating its BSSCFG.
 * On BSSCFG failure, 0x127058 still calls 0x1263bc, which dereferences that
 * absent BSSCFG. Replace just that BL; native node/nested-storage free stays
 * responsible for its own partial allocation. */
int nan_slotted_release_bss(void *manager, uint32_t index)
{
    if (!((void *(*)(void *, uint32_t))native_0x1279d1)(manager, index))
        return 0;
    return ((int (*)(void *, uint32_t))native_0x1263bd)(manager, index);
}

int nan_mac_remove_bsscfg(void *mac)
{
    if (!mac || !PTR(mac, 8) || !PTR(PTR(mac, 8), 0) ||
        !PTR(PTR(mac, 8), 4))
        return -2;
    void *manager = PTR(mac, NAN_MAC_BSS_MANAGER);
    void *node = PTR(mac, NAN_MAC_BSS_NODE);
    void *bands = PTR(mac, 8);
    if (!manager)
        return node || PTR(PTR(bands, 0), 0) || PTR(PTR(bands, 4), 0) ? -30 : 0;
    if (manager != PTR(PTR(mac, 0x10), 0x678))
        return -30;
    int data_error = nan_data_check_idle(PTR(mac, 4));
    if (data_error)
        return data_error;
    uint32_t index = U32(mac, 0xd4);
    void **slot = entry(manager, index);
    if (!slot || !node || *slot != node)
        return -30;
    /* Native 0x127058 silently consumes one reference when refcount > 1.
     * A retry must never consume another owner's reference. Single NAN owns
     * one entry; retain it until external use/scheduling has actually ended. */
    if (PTR(mac, NAN_MAC_EVENT_BSS) || PTR(mac, NAN_MAC_EVENT_MANAGER) ||
        U8(node, 1) || U8(node, 2) > 1 || U8(PTR(mac, 0x38), 0x4a) ||
        PTR(mac, 0x3c) || PTR(mac, 0x40) || PTR(mac, 0xe4) ||
        PTR(mac, 0x10c) || U32(mac, 0xb4) ||
        PTR(mac, NAN_MAC_HOST_TX) || U32(mac, NAN_MAC_HOST_TX_COUNT) ||
        PTR(mac, NAN_MAC_HOST_STATIONS) || PTR(mac, NAN_MAC_HOST_GROUP_KEYS) ||
        PTR(PTR(bands, 0), 0x10) || PTR(PTR(bands, 0), 0x14) ||
        PTR(PTR(bands, 4), 0x10) || PTR(PTR(bands, 4), 0x14))
        return -16;
    if (PTR(node, 0x0c)) {
        if (!our_callbacks(node, mac))
            return -30;
        int error = ((int (*)(void *, uint32_t))native_0x128649)(manager, index);
        if (error || PTR(node, 0x0c))
            return error ? error : -16;
    }
    int error = ((int (*)(void *, uint32_t))native_0x1281e5)(manager, index);
    if (*slot)
        return error ? error : -16;
    /* Only the observed removal of the native table entry releases ownership.
     * Do not dereference node/BSSCFG after the native free. */
    PTR(PTR(bands, 0), 0) = 0;
    PTR(PTR(bands, 4), 0) = 0;
    PTR(mac, NAN_MAC_BSS_NODE) = 0;
    PTR(mac, NAN_MAC_BSS_MANAGER) = 0;
    U32(mac, 0xd4) = UINT32_MAX;
    U32(PTR(PTR(mac, 4), 0x10), 0x34) &= ~0x40000000u;
    return error;
}

int nan_mac_prepare_bsscfg(void *wlc, void *mac, const unsigned char *nmi,
                           unsigned char option)
{
    if (!wlc || !mac || !nmi || PTR(mac, 0x10) != wlc || !PTR(mac, 4) ||
        !PTR(PTR(mac, 4), 0x10) || !PTR(mac, 0x38) || !PTR(mac, 8) ||
        !PTR(PTR(mac, 8), 0) || !PTR(PTR(mac, 8), 4))
        return -2;
    uint8_t nonzero = 0;
    for (size_t i = 0; i < 6; ++i)
        nonzero |= nmi[i];
    if (!nonzero || (nmi[0] & 1u))
        return -2;
    void *manager = PTR(wlc, 0x678);
    void *bands = PTR(mac, 8);
    void *bss = PTR(PTR(bands, 0), 0);
    if (PTR(mac, NAN_MAC_BSS_MANAGER) || PTR(mac, NAN_MAC_BSS_NODE) ||
        bss || PTR(PTR(bands, 4), 0)) {
        void **slot = entry(manager, U32(mac, 0xd4));
        if (!slot || !*slot || *slot != PTR(mac, NAN_MAC_BSS_NODE) ||
            manager != PTR(mac, NAN_MAC_BSS_MANAGER) || !bss ||
            bss != PTR(PTR(bands, 4), 0) || !our_callbacks(*slot, mac) ||
            !(U32(PTR(PTR(mac, 4), 0x10), 0x34) & 0x40000000u))
            return -16;
        for (size_t i = 0; i < 6; ++i)
            if (nmi[i] != U8(bss, 0x66 + i))
                return -16;
        return 0;
    }
    if (!entry(manager, 0))
        return -2;
    if (((void *(*)(void *, const uint8_t *))native_0x1515c1)(wlc, nmi) ||
        ((int (*)(void *, const uint8_t *))native_0x126565)(manager, nmi) >= 0)
        return -21; /* Never acquire a shared entry for the same NMI. */

    uint32_t request[5];
    uint32_t settings[7];
    void (*zero)(void *, size_t) = (void (*)(void *, size_t))0x18660bu;
    zero(request, sizeof(request));
    zero(settings, sizeof(settings));
    const uint8_t type[4] = {5, 0x0a, 0, 0};
    request[0] = (uintptr_t)type;
    request[1] = (uintptr_t)settings;
    request[2] = 0x800;
    U8(settings, 6) = 3;
    U32(settings, 8) = 2;
    U8(settings, 0x18) = option;
    for (size_t i = 0; i < 6; ++i)
        U8(settings, 0x0c + i) = nmi[i];
    int status = 0;
    bss = ((void *(*)(void *, void *, uint32_t *, int *))native_0x127821)(
        manager, request, &U32(mac, 0xd4), &status);
    void **slot = entry(manager, U32(mac, 0xd4));
    if (bss || (slot && *slot)) {
        PTR(mac, NAN_MAC_BSS_MANAGER) = manager;
        PTR(mac, NAN_MAC_BSS_NODE) = slot ? *slot : 0;
        PTR(PTR(bands, 0), 0) = bss;
        PTR(PTR(bands, 4), 0) = bss;
    } else {
        U32(mac, 0xd4) = UINT32_MAX;
    }
    if (!bss || status) {
        int cleanup = nan_mac_remove_bsscfg(mac);
        return cleanup ? cleanup : status ? status : -1;
    }
    if (!slot || !*slot || PTR(bss, 0) != wlc)
        return -30;
    uint32_t callbacks[7];
    zero(callbacks, sizeof(callbacks));
    callbacks[0] = (uintptr_t)PTR(mac, 4);
    callbacks[6] = 0x385a5;
    status = ((int (*)(void *, const uint32_t *, uint32_t))native_0x152eb1)(
        manager, callbacks, U32(mac, 0xd4));
    if (status) {
        int cleanup = nan_mac_remove_bsscfg(mac);
        return cleanup ? cleanup : status;
    }
    U32(PTR(PTR(mac, 4), 0x10), 0x34) |= 0x40000000u;
    return 0;
}

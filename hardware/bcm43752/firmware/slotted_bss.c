#include "native_calls.h"
/* Restore the A2 callback registration used by NAN BSSCFG creation.
 * Caller 0x2e7de..0x2e7fe constructs exactly 28 bytes; B0 uses 32 bytes.
 * Native A2 unregister 0x128648 owns/frees node +0x0c with osl_mfree.
 */
#include <stddef.h>
#include <stdint.h>

#define PTR(base, offset) (*(void **)((uint8_t *)(base) + (offset)))

/* This A2 image's attach at 0x234334 allocates only 0x38 bytes for
 * manager->common. The NAN ROM start/stop routines still read common+0x38,
 * outside that allocation. Attach already creates the pre-close timer at
 * manager+0x58 with the native 0x1281c0 callback. Use that actual owner. */
unsigned slotted_bss_stop_pre_close_timer(void *manager)
{
    if (!manager || !PTR(manager, 0) || !PTR(manager, 0x58) ||
        !PTR(PTR(manager, 0x58), 0x44))
        return 0;
    return ((unsigned (*)(void *, void *))native_0x7fe95)(
        PTR(PTR(manager, 0), 8), PTR(manager, 0x58));
}

int slotted_bss_start_pre_close_timer(void *manager, unsigned milliseconds,
                                    unsigned periodic)
{
    if (!slotted_bss_stop_pre_close_timer(manager))
        return -16;
    return ((unsigned (*)(void *, void *, unsigned, unsigned))native_0x7fdc9)(
        PTR(PTR(manager, 0), 8), PTR(manager, 0x58), milliseconds, periodic)
        ? 0 : -1;
}

/* The missing wlc_cfg_get_now slot is called by native NAN peer allocation
 * and watchdog code. Retain the native shared slotted-BSS clock; never derive
 * timestamps from host wall time or fabricate a successful timeout. */
uint32_t bsscfg_now(void *bss)
{
    void *wlc = PTR(bss, 0);
    if (*(uint32_t *)((uint8_t *)bss + 0x74) == 5)
        return ((uint32_t (*)(void *, void *))native_0x127a79)(PTR(wlc, 0x678), bss);
    return *(uint32_t *)((uint8_t *)PTR(wlc, 0) + 0x28);
}

int slotted_bss_register_callbacks(void *manager, const uint32_t *callbacks,
                                   uint32_t index)
{
    if (!manager || !callbacks || !PTR(manager, 4))
        return -2;
    void *table = PTR(PTR(manager, 4), 8);
    if (!table || !PTR(table, 0) || index >= *(uint32_t *)((uint8_t *)table + 8))
        return -2;
    void *node = ((void **)PTR(table, 0))[index];
    if (!node)
        return -2;
    if (PTR(node, 0x0c))
        return -16;
    uint32_t *owned = ((void *(*)(size_t))native_0x7146d)(28);
    if (!owned)
        return -27;
    for (size_t i = 0; i < 7; ++i)
        owned[i] = callbacks[i];
    PTR(node, 0x0c) = owned;
    return 0;
}

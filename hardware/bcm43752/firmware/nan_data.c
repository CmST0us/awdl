#include "native_calls.h"
/* Native A2 NAN data interfaces. Hostap owns the NDP protocol; no autonomous
 * Broadcom NDP command group is registered here. Both NDIs have distinct MACs
 * and subtype 0x0b. Never borrow a STA/AP or the NMI BSSCFG as subtype 0x0c.
 */
#include <stddef.h>
#include <stdint.h>
#include "nan_data.h"
#include "nan_group.h"
#include "nan_mac.h"
#include "awdl_session.h"
#include "awdl_peer.h"

#define FIELD(type, base, offset) (*(type *)((uint8_t *)(base) + (offset)))
#define PTR(base, offset) FIELD(void *, base, offset)
#define U32(base, offset) FIELD(uint32_t, base, offset)
#define U16(base, offset) FIELD(uint16_t, base, offset)
#define U8(base, offset) FIELD(uint8_t, base, offset)
#define NDI_COUNT 2u /* A2 0x22298 checks exactly two native index entries. */
#define OWNER(dp, i) ((uint8_t *)(dp) + 0x28 + (i) * 12)

/* The A2 NAN interface-create callback uses the v2 information layout, but
 * its native encoder leaves the first four bytes untouched. DHD's in/out
 * buffer then returns the old "inte" prefix of interface_create. Keep the
 * native MAC/index/name encoding and complete its actual v2 header. */
int nan_data_interface_info(void *bss, void *info, const void *request)
{
    if (!bss || !info || !request)
        return -2;
    int error = ((int (*)(void *, void *, const void *))native_0x128461)(bss, info, request);
    if (!error) {
        U16(info, 0) = 2;
        U16(info, 2) = 28;
    }
    return error;
}

static void zero(void *p, size_t size)
{
    ((void (*)(void *, size_t))native_0x18660b)(p, size);
}

static void *allocate(size_t size)
{
    return ((void *(*)(size_t))native_0x7146d)(size);
}

static int valid_address(const uint8_t *address)
{
    uint8_t value = 0;
    for (unsigned i = 0; i < 6; ++i)
        value |= address[i];
    return value && !(address[0] & 1u);
}

static int same_address(const uint8_t *a, const uint8_t *b)
{
    for (unsigned i = 0; i < 6; ++i)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void **entry(void *manager, unsigned index)
{
    if (!manager || !PTR(manager, 4))
        return 0;
    void *table = PTR(PTR(manager, 4), 8);
    if (!table || !PTR(table, 0) || index >= U32(table, 8))
        return 0;
    return &((void **)PTR(table, 0))[index];
}

static int valid_context(void *dp)
{
    if (!dp || U32(dp, 4) != 0x004e4450 || !PTR(dp, 8))
        return 0;
    void *nan = PTR(dp, 8);
    return PTR(nan, 0x30) == dp && PTR(nan, 8) && PTR(nan, 0x6c) &&
           U8(PTR(nan, 0x6c), 5) == NDI_COUNT && PTR(dp, 0x14) &&
           PTR(dp, 0x1c) && PTR(dp, 0x20);
}

int nan_data_context_attach(void *wlc, void *nan)
{
    if (!wlc || !nan || PTR(nan, 8) != wlc || !PTR(nan, 0x6c) ||
        U8(PTR(nan, 0x6c), 5) != NDI_COUNT)
        return -2;
    if (PTR(nan, 0x30))
        return -16;
    /* The native reserve wrapper dereferences this complete chain. Cubby
     * handles are byte offsets with a mode nibble, not registration indices. */
    void *modules = PTR(wlc, 0x60);
    void *bss_module = modules ? PTR(modules, 0x10) : 0;
    void *cubbies = bss_module ? PTR(bss_module, 4) : 0;
    if (!cubbies || !PTR(cubbies, 0x18) || !PTR(cubbies, 0x1c))
        return -2;
    void *dp = allocate(0x40); /* Native 0x28 + two manager/node/count records. */
    if (!dp)
        return -27;
    U32(dp, 4) = 0x004e4450;
    PTR(dp, 8) = nan;
    PTR(nan, 0x30) = dp;
    int error = -27;
    const uint8_t offsets[] = {0x10, 0x14, 0x1c, 0x20};
    const uint8_t sizes[] = {4, 6 * NDI_COUNT, 4 * NDI_COUNT, 2 * NDI_COUNT};
    for (unsigned i = 0; i < sizeof(offsets); ++i) {
        PTR(dp, offsets[i]) = allocate(sizes[i]);
        if (!PTR(dp, offsets[i]))
            goto fail;
    }
    for (unsigned i = 0; i < NDI_COUNT; ++i) {
        U32(PTR(dp, 0x1c), i * 4) = UINT32_MAX;
        U16(PTR(dp, 0x20), i * 2) = UINT16_MAX;
    }
    /* Reserve last: once WLC owns this callback, the context stays alive for
     * WLC lifetime. Native 0x22440 initializes only NAN data BSS subtypes;
     * native 0x2243c is intentionally empty. There is no cubby unreserve API. */
    error = ((int (*)(void *, unsigned, uintptr_t, uintptr_t, uintptr_t, void *))
             0x22b251u)(wlc, 0, 0x22441u, 0x2243du, 0, dp);
    if (error < 0)
        goto fail;
    U32(dp, 0x0c) = (uint32_t)error;
    return 0;
fail:
    for (unsigned i = 0; i < sizeof(offsets); ++i)
        if (PTR(dp, offsets[i]))
            ((void (*)(void *))native_0x71491)(PTR(dp, offsets[i]));
    ((void (*)(void *))native_0x71491)(dp);
    PTR(nan, 0x30) = 0;
    return error;
}

static int our_callbacks(void *node, void *nan)
{
    uint32_t *cb = PTR(node, 0x0c);
    return cb && cb[0] == (uintptr_t)nan && cb[1] == (uintptr_t)awdl_peer_available &&
           cb[2] == (uintptr_t)awdl_peer_reinit && cb[3] == (uintptr_t)awdl_group_ring_owner &&
           !cb[4] && !cb[5] && !cb[6];
}

void *nan_data_owned_bss(void *nan, const unsigned char *address, unsigned *index)
{
    void *dp = nan ? PTR(nan, 0x30) : 0;
    if (!valid_context(dp) || !address || !valid_address(address))
        return 0;
    for (unsigned i = 0; i < NDI_COUNT; ++i) {
        if (!same_address(address, (uint8_t *)PTR(dp, 0x14) + i * 6))
            continue;
        void *owner = OWNER(dp, i), *manager = PTR(owner, 0);
        unsigned native_index = U32(PTR(dp, 0x1c), i * 4);
        void **slot = entry(manager, native_index);
        if (!slot || !*slot || *slot != PTR(owner, 4) ||
            manager != PTR(PTR(nan, 8), 0x678) || !(U32(owner, 8) & 1u) ||
            !our_callbacks(*slot, nan))
            return 0;
        void *bss = ((void *(*)(void *, unsigned))native_0x1279d1)(manager, native_index);
        if (!bss || PTR(bss, 0) != PTR(nan, 8) || U32(bss, 0x74) != 5 ||
            U32(bss, 0x78) != 11 || !same_address(address, (uint8_t *)bss + 0x66))
            return 0;
        if (index)
            *index = i;
        return bss;
    }
    return 0;
}

int nan_data_owned_slot(void *nan, const unsigned char *address)
{
    unsigned index;
    if (!nan_data_owned_bss(nan, address, &index))
        return -30;
    return (int)U32(PTR(PTR(nan, 0x30), 0x1c), index * 4);
}

int nan_data_unprepare(void *nan, const unsigned char *address)
{
    unsigned i;
    void *bss = nan_data_owned_bss(nan, address, &i);
    if (!bss)
        return -30;
    void *wlc = PTR(nan, 8), *dp = PTR(nan, 0x30);
    int error = nan_group_idle(nan, bss, 0);
    if (error)
        return error;
    if (!PTR(wlc, 0x5c) || !PTR(bss, 0x304))
        return -30;
    uint32_t iterator[3] = {0};
    ((void (*)(void *, void *, void *))native_0x1239a9)(PTR(wlc, 0x5c), iterator, bss);
    if (((void *(*)(void *, void *))native_0x1239e9)(PTR(wlc, 0x5c), iterator))
        return -16;
    error = nan_bss_stop(nan, bss, U32(PTR(dp, 0x1c), i * 4));
    if (!error)
        U32(OWNER(dp, i), 8) &= ~2u;
    return error;
}

int nan_data_owned_channel(void *nan, const unsigned char *address,
                           unsigned channel, unsigned opened)
{
    unsigned index;
    if (opened > 1 || !nan_data_owned_bss(nan, address, &index))
        return -30;
    void *dp = PTR(nan, 0x30);
    return ((int (*)(void *, unsigned, unsigned, unsigned))native_0x1278b5)(
        PTR(OWNER(dp, index), 0), U32(PTR(dp, 0x1c), index * 4),
        channel, opened ? 1 : 2);
}

int nan_data_register_callbacks(void *nan, unsigned index, unsigned subtype)
{
    if (!nan || index >= NDI_COUNT || subtype != 0x0b)
        return -2;
    void *dp = PTR(nan, 0x30);
    if (!valid_context(dp))
        return -2;
    void *owner = OWNER(dp, index);
    void *manager = PTR(owner, 0);
    void **slot = entry(manager, U32(PTR(dp, 0x1c), index * 4));
    if (!slot || !*slot || *slot != PTR(owner, 4) ||
        manager != PTR(PTR(nan, 8), 0x678))
        return -30;
    uint32_t callbacks[7];
    zero(callbacks, sizeof(callbacks));
    callbacks[0] = (uintptr_t)nan;
    callbacks[1] = (uintptr_t)awdl_peer_available;
    callbacks[2] = (uintptr_t)awdl_peer_reinit;
    callbacks[3] = (uintptr_t)awdl_group_ring_owner;
    return ((int (*)(void *, const uint32_t *, unsigned))native_0x152eb1)(
        manager, callbacks, U32(PTR(dp, 0x1c), index * 4));
}

static int remove_index(void *dp, unsigned i)
{
    void *nan = PTR(dp, 8);
    void *owner = OWNER(dp, i);
    unsigned index = U32(PTR(dp, 0x1c), i * 4);
    void *manager = PTR(owner, 0);
    void *node = PTR(owner, 4);
    if (index == UINT32_MAX)
        return manager || node || U32(owner, 8) ? -30 : 0;
    void *wlc = PTR(nan, 8);
    void **slot = entry(manager, index);
    if (!slot || !node || *slot != node || manager != PTR(wlc, 0x678))
        return -30;
    if (U8(node, 2) != 1 || U16(PTR(dp, 0x20), i * 2) != UINT16_MAX)
        return -16;
    if (U32(owner, 8) && !U8(dp, 0x18))
        return -30;
    void *bss = ((void *(*)(void *, unsigned))native_0x1279d1)(manager, index);
    if (bss) {
        if (awdl_session_holds_bss(nan, bss))
            return -16;
        if (PTR(bss, 0) != wlc || U32(bss, 0x74) != 5 || U32(bss, 0x78) != 0x0b)
            return -30;
        if (nan_group_idle(nan, bss, 0))
            return -16;
        /* The host must finish peer/key/TX teardown before deleting the NDI.
         * Never let native slotted-BSS stop silently destroy live stations. */
        if (!PTR(wlc, 0x5c) || !PTR(bss, 0x304))
            return -30;
        uint32_t iterator[3];
        zero(iterator, sizeof(iterator));
        ((void (*)(void *, void *, void *))native_0x1239a9)(PTR(wlc, 0x5c), iterator, bss);
        if (((void *(*)(void *, void *))native_0x1239e9)(PTR(wlc, 0x5c), iterator))
            return -16;
        int error = nan_bss_stop(nan, bss, index);
        if (error)
            return error;
    }
    if (PTR(node, 0x0c)) {
        if (!our_callbacks(node, nan))
            return -30;
        int error = ((int (*)(void *, unsigned))native_0x128649)(manager, index);
        if (error || PTR(node, 0x0c))
            return error ? error : -16;
    }
    /* This host-created NDI owns its +0x38 BCMC SCB. Native BSSCFG deinit
     * (0xbade4) must release it; clearing the pointer would leak that SCB.
     * The stock autonomous NDI borrows NMI's BCMC SCB and clears it here,
     * but that ownership model does not apply to our independent NDI. */
    if (bss && PTR(bss, 0x38) && PTR(PTR(bss, 0x38), 0x0c) != bss)
        return -30;
    int error = ((int (*)(void *, unsigned))native_0x1281e5)(manager, index);
    if (*slot)
        return error ? error : -16;
    if (U32(owner, 8))
        --U8(dp, 0x18);
    zero((uint8_t *)PTR(dp, 0x14) + i * 6, 6);
    U32(PTR(dp, 0x1c), i * 4) = UINT32_MAX;
    U16(PTR(dp, 0x20), i * 2) = UINT16_MAX;
    zero(owner, 12);
    return error;
}

int nan_data_enable(void *nan, void *bss, unsigned i)
{
    void *dp = nan ? PTR(nan, 0x30) : 0;
    if (!valid_context(dp) || i >= NDI_COUNT || !bss)
        return -2;
    void *owner = OWNER(dp, i), *manager = PTR(owner, 0), *node = PTR(owner, 4);
    unsigned index = U32(PTR(dp, 0x1c), i * 4);
    void **slot = entry(manager, index);
    if (!slot || !node || *slot != node || manager != PTR(PTR(nan, 8), 0x678) ||
        ((void *(*)(void *, unsigned))native_0x1279d1)(manager, index) != bss ||
        U32(bss, 0x74) != 5 || U32(bss, 0x78) != 11 || !our_callbacks(node, nan) ||
        !(U32(owner, 8) & 1u))
        return -30;
    if (U32(owner, 8) & 2u)
        return U8(bss, 5) && U8(node, 1) ? 0 : -30;
    if (U8(bss, 5) || U8(node, 1))
        return -16; /* Partial prepare remains owned until actual stop. */
    /* Native 0x224ac prepares the NDI before its first peer. Preserve this
     * host-created NDI's own BCMC SCB instead of borrowing NMI +0x38. */
    int error = nan_bss_prepare_enable(nan, bss, 0, index);
    if (error || !U8(bss, 5) || !U8(node, 1)) {
        ((int (*)(const char *, ...))native_0xa815)(
            "NAN NDI prepare: error=%d bss=%u node=%u\n",
            error, U8(bss, 5), U8(node, 1));
        return error ? error : -30;
    }
    U32(owner, 8) |= 2u; /* Full preparation, separate from counted ownership. */
    return 0;
}

int nan_data_remove(void *dp, const unsigned char *address)
{
    if (!valid_context(dp) || !address || !valid_address(address))
        return -2;
    for (unsigned i = 0; i < NDI_COUNT; ++i)
        if (same_address(address, (uint8_t *)PTR(dp, 0x14) + i * 6))
            return remove_index(dp, i);
    return -30; /* An unknown address is not evidence of successful removal. */
}

int nan_data_check_idle(void *nan)
{
    if (!nan)
        return -2;
    void *dp = PTR(nan, 0x30);
    if (!dp)
        return 0;
    if (!valid_context(dp))
        return -30;
    for (unsigned i = 0; i < NDI_COUNT; ++i)
        if (U32(PTR(dp, 0x1c), i * 4) != UINT32_MAX ||
            PTR(OWNER(dp, i), 0) || PTR(OWNER(dp, i), 4) ||
            U32(OWNER(dp, i), 8))
            return -16;
    return U8(dp, 0x18) ? -30 : 0;
}

int nan_data_deinitialize(void *wlc, void *nan)
{
    if (!wlc || !nan || PTR(nan, 8) != wlc)
        return -2;
    void *dp = PTR(nan, 0x30);
    if (!dp)
        return 0;
    if (!valid_context(dp))
        return -30;
    for (unsigned i = 0; i < NDI_COUNT; ++i) {
        int error = remove_index(dp, i);
        if (error)
            return error;
    }
    return nan_data_check_idle(nan);
}

void *nan_data_create(void *wlc, void *dp, const void *request, int *status)
{
    if (!status)
        return 0;
    *status = -2;
    if (!wlc || !valid_context(dp) || PTR(PTR(dp, 8), 8) != wlc ||
        !request || !PTR(request, 4))
        return 0;
    const uint8_t *address = (uint8_t *)PTR(request, 4) + 0x0c;
    if (!valid_address(address))
        return 0;
    void *manager = PTR(wlc, 0x678);
    if (!entry(manager, 0))
        return 0;
    if (((void *(*)(void *, const uint8_t *))native_0x1515c1)(wlc, address) ||
        ((int (*)(void *, const uint8_t *))native_0x126565)(manager, address) >= 0) {
        *status = -21;
        return 0;
    }
    unsigned i;
    for (i = 0; i < NDI_COUNT; ++i) {
        if (same_address(address, (uint8_t *)PTR(dp, 0x14) + i * 6)) {
            *status = -16;
            return 0;
        }
    }
    for (i = 0; i < NDI_COUNT; ++i)
        if (U32(PTR(dp, 0x1c), i * 4) == UINT32_MAX)
            break;
    *status = -29;
    if (i == NDI_COUNT)
        return 0;
    void *owner = OWNER(dp, i);
    if (PTR(owner, 0) || PTR(owner, 4) || U32(owner, 8)) {
        *status = -30;
        return 0;
    }
    uint32_t local_request[5];
    for (unsigned n = 0; n < 5; ++n)
        local_request[n] = U32(request, n * 4);
    const uint8_t type[4] = {5, 0x0b, 0, 0};
    local_request[0] = (uintptr_t)type;
    *status = 0;
    void *bss = ((void *(*)(void *, void *, uint32_t *, int *))native_0x127821)(
        manager, local_request, &U32(PTR(dp, 0x1c), i * 4), status);
    void **slot = entry(manager, U32(PTR(dp, 0x1c), i * 4));
    if (!bss && (!slot || !*slot)) {
        U32(PTR(dp, 0x1c), i * 4) = UINT32_MAX;
        if (!*status)
            *status = -1;
        return 0;
    }
    PTR(owner, 0) = manager;
    PTR(owner, 4) = slot ? *slot : 0;
    for (unsigned n = 0; n < 6; ++n)
        U8(PTR(dp, 0x14), i * 6 + n) = address[n];
    if (!*status && (!bss || !slot || !*slot || PTR(bss, 0) != wlc ||
                     U32(bss, 0x74) != 5 || U32(bss, 0x78) != 0x0b))
        *status = -30;
    if (!*status)
        *status = ((int (*)(void *, unsigned, unsigned))native_0x152061)(PTR(dp, 8), i, 0x0b);
    if (*status) {
        int cleanup = remove_index(dp, i);
        if (cleanup)
            *status = cleanup;
        return 0;
    }
    U32(owner, 8) = 1;
    ++U8(dp, 0x18);
    return bss;
}

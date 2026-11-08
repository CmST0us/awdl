/* A2 NDL/NDP context and native attribute lifecycle. Hostap remains the only
 * negotiation owner. Native data lookup and SCB callbacks consume these
 * contexts; the autonomous Broadcom NDP command group is not registered. */
#include <stddef.h>
#include <stdint.h>
#include "nan_link.h"
#include "nan_data.h"
#include "nan_peer.h"

#define FIELD(type, base, offset) (*(type *)((uint8_t *)(base) + (offset)))
#define PTR(base, offset) FIELD(void *, base, offset)
#define U32(base, offset) FIELD(uint32_t, base, offset)
#define U16(base, offset) FIELD(uint16_t, base, offset)
#define U8(base, offset) FIELD(uint8_t, base, offset)
#define NDL_INITIALIZED 0x18
#define NDP_INITIALIZED 0x2c
#define NDP_CAPACITY 0x30

static void *allocate(size_t size)
{
    return ((void *(*)(size_t))0x7146du)(size);
}

static void release(void *p)
{
    if (p)
        ((void (*)(void *))0x71491u)(p);
}

static int configured(void *nan)
{
    if (!nan || !PTR(nan, 8) || !PTR(nan, 0x10) || !PTR(nan, 0x6c))
        return 0;
    void *cfg = PTR(nan, 0x6c);
    /* NDL has a four-byte peer bitmap. Do not let signed peer iteration or
     * bitmap writes run beyond the native 32-peer extent. */
    return U8(cfg, 2) && U8(cfg, 2) <= 32 && U8(cfg, 9);
}

static int valid_ndp(void *ndp)
{
    if (!ndp || U32(ndp, 4) != 0x4e4e4450 || !configured(PTR(ndp, 8)))
        return 0;
    void *nan = PTR(ndp, 8);
    return PTR(nan, 0x34) == ndp && PTR(ndp, 0x10) &&
           U32(ndp, NDP_CAPACITY) == U8(PTR(nan, 0x6c), 9) &&
           (U32(PTR(nan, 0x10), 0x34) & NAN_CONTROL_HOST_NDP_ID) &&
           PTR(nan, 0x40) && U32(nan, 0x3c) == 0x88;
}

void *nan_ndp_entry_allocate(void *ndp, const unsigned char *nmi,
                             const unsigned char *id, unsigned host_id)
{
    /* A2 0x34c84 publishes a record even when peer lookup returns NULL, and
     * increments an unchecked byte reference counter. Validate first. */
    if (!valid_ndp(ndp) || !nmi || (nmi[0] & 1u) || !id || !*id || !host_id)
        return 0;
    unsigned nonzero = 0;
    for (unsigned i = 0; i < 6; ++i)
        nonzero |= nmi[i];
    if (!nonzero)
        return 0;
    void *nan = PTR(ndp, 8);
    void *peer = ((void *(*)(void *, const void *, unsigned))0x37189u)(nan, nmi, 0);
    if (!peer || U8(peer, 9) == UINT8_MAX)
        return 0;
    unsigned index;
    for (index = 0; index < U32(ndp, NDP_CAPACITY); ++index)
        if (!PTR(PTR(ndp, 0x10), index * 4))
            break;
    if (index == U32(ndp, NDP_CAPACITY))
        return 0;
    void *record = allocate(0x50);
    if (!record)
        return 0;
    PTR(record, 0x20) = allocate(8);
    if (!PTR(record, 0x20)) {
        release(record);
        return 0;
    }
    U8(record, 9) = *id;
    U8(record, 0x0b) = UINT8_MAX; /* NDI is bound after host negotiation. */
    for (unsigned i = 0; i < 6; ++i)
        U8(record, 0x0e + i) = nmi[i];
    U8(record, 0x30) = index;
    ((void *(*)(void *, const void *, unsigned))0x37189u)(nan, nmi, 1);
    PTR(PTR(ndp, 0x10), index * 4) = record;
    return record;
}

int nan_ndp_entry_release(void *ndp, void *record)
{
    if (!valid_ndp(ndp) || !record)
        return -2;
    /* Find ownership before dereferencing the caller's record pointer. */
    unsigned index;
    for (index = 0; index < U32(ndp, NDP_CAPACITY); ++index)
        if (PTR(PTR(ndp, 0x10), index * 4) == record)
            break;
    if (index == U32(ndp, NDP_CAPACITY) || U8(record, 0x30) != index)
        return -30;
    void *nan = PTR(ndp, 8);
    void *peer = ((void *(*)(void *, const void *, unsigned))0x37189u)(
        nan, (uint8_t *)record + 0x0e, 0);
    if (!peer || !U8(peer, 9))
        return -30;
    if (PTR(peer, 0x24) || PTR(peer, 0x30) || PTR(peer, 0x58))
        return -16;
    int error = nan_peer_check_stations(nan, peer, (uint8_t *)record + 0x14);
    if (error)
        return error;
    ((void *(*)(void *, const void *, unsigned))0x37189u)(
        nan, (uint8_t *)record + 0x0e, 2);
    PTR(PTR(ndp, 0x10), index * 4) = 0;
    release(PTR(record, 0x20));
    release(record);
    return 0;
}

int nan_link_context_attach(void *nan)
{
    if (!configured(nan))
        return -2;
    if (PTR(nan, 0x38) || PTR(nan, 0x34))
        return -16;
    void *ndl = allocate(0x1c); /* Native 0x18 + initialized word. */
    void *ndp = 0;
    if (!ndl)
        return -27;
    ndp = allocate(0x34); /* Native 0x2c + initialized/capacity words. */
    if (!ndp)
        goto fail;
    PTR(ndp, 0x10) = allocate(U8(PTR(nan, 0x6c), 9) * 4);
    if (!PTR(ndp, 0x10))
        goto fail;
    PTR(ndp, 0x24) = allocate(0x1c);
    if (!PTR(ndp, 0x24))
        goto fail;
    U32(ndl, 4) = 0x4e4e444c;
    PTR(ndl, 8) = nan;
    U32(ndp, 4) = 0x4e4e4450;
    PTR(ndp, 8) = nan;
    U16(ndp, 0x14) = 60; /* Native idle seconds; autonomous deletion stays off. */
    U16(ndp, 0x1c) = U8(PTR(nan, 0x6c), 0x22);
    U32(ndp, NDP_CAPACITY) = U8(PTR(nan, 0x6c), 9);
    PTR(nan, 0x38) = ndl;
    PTR(nan, 0x34) = ndp;
    /* Do not copy reference 0x20180: it enables automatic schedule replies
     * and peer deletion. The host owns both lifecycles in this project. */
    U32(PTR(nan, 0x10), 0x34) &=
        ~(NAN_CONTROL_AUTOMATIC | NAN_CONTROL_INFRA_COEX | NAN_CONTROL_DELETE_INACTIVE);
    U32(PTR(nan, 0x10), 0x34) |= NAN_CONTROL_HOST_NDP_ID;
    return 0;
fail:
    if (ndp) {
        release(PTR(ndp, 0x24));
        release(PTR(ndp, 0x10));
    }
    release(ndp);
    release(ndl);
    return -27;
}

int nan_link_check_idle(void *nan)
{
    if (!configured(nan))
        return -2;
    int error = nan_data_check_idle(nan);
    if (!error)
        error = nan_peer_check_idle(nan);
    if (error)
        return error;
    void *ndl = PTR(nan, 0x38);
    if (ndl && U32(ndl, 0x14))
        return -16;
    void *ndp = PTR(nan, 0x34);
    if (ndp) {
        if (!PTR(ndp, 0x10) ||
            U32(ndp, NDP_CAPACITY) != U8(PTR(nan, 0x6c), 9))
            return -30;
        for (unsigned i = 0; i < U32(ndp, NDP_CAPACITY); ++i)
            if (PTR(PTR(ndp, 0x10), i * 4))
                return -16;
    }
    return 0;
}

static int idle(void *nan)
{
    int error = nan_link_check_idle(nan);
    if (error)
        return error;
    void *mac = PTR(nan, 0x14);
    return mac && PTR(mac, 0x38) && U8(PTR(mac, 0x38), 0x4a) ? -16 : 0;
}

static int unregister_attribute(void *nan, void *owner, unsigned offset)
{
    if (!PTR(owner, offset))
        return 0;
    int error = ((int (*)(void *, void *))0x1ffc9u)(nan, PTR(owner, offset));
    if (!error)
        PTR(owner, offset) = 0;
    return error;
}

int nan_ndl_deinitialize(void *wlc, void *nan)
{
    if (!nan || PTR(nan, 8) != wlc)
        return -2;
    void *ndl = PTR(nan, 0x38);
    if (!ndl)
        return 0;
    int error = idle(nan);
    if (error)
        return error;
    error = unregister_attribute(nan, ndl, 0x0c);
    if (!error)
        error = unregister_attribute(nan, ndl, 0x10);
    if (!error)
        U32(ndl, NDL_INITIALIZED) = 0;
    return error;
}

int nan_ndp_deinitialize(void *wlc, void *nan)
{
    if (!nan || PTR(nan, 8) != wlc)
        return -2;
    void *ndp = PTR(nan, 0x34);
    if (!ndp)
        return 0;
    int error = idle(nan);
    if (error)
        return error;
    if (!PTR(nan, 0x74))
        return -30;
    void *stats = (uint8_t *)PTR(nan, 0x74) + 8 * 12;
    if ((U32(stats, 0) && U32(stats, 0) != 0x34e09u) ||
        U32(stats, 4) || U32(stats, 8))
        return -30;
    error = unregister_attribute(nan, ndp, 0x0c);
    if (!error) {
        ((void (*)(void *, size_t))0x18660bu)(stats, 12);
        U32(ndp, NDP_INITIALIZED) = 0;
    }
    return error;
}

int nan_ndl_initialize(void *wlc, void *nan)
{
    if (!configured(nan) || PTR(nan, 8) != wlc || !PTR(nan, 0x38))
        return -2;
    void *ndl = PTR(nan, 0x38);
    if (U32(ndl, NDL_INITIALIZED) || PTR(ndl, 0x0c) || PTR(ndl, 0x10))
        return -16;
    int error = ((int (*)(void *, void *))0x34451u)(wlc, nan);
    if (error) {
        int cleanup = nan_ndl_deinitialize(wlc, nan);
        return cleanup ? cleanup : error;
    }
    U32(ndl, NDL_INITIALIZED) = 1;
    return 0;
}

int nan_ndp_initialize(void *wlc, void *nan)
{
    if (!configured(nan) || PTR(nan, 8) != wlc || !PTR(nan, 0x34) ||
        !PTR(nan, 0x74))
        return -2;
    void *ndp = PTR(nan, 0x34);
    void *stats = (uint8_t *)PTR(nan, 0x74) + 8 * 12;
    if (U32(ndp, NDP_INITIALIZED) || PTR(ndp, 0x0c) ||
        U32(stats, 0) || U32(stats, 4) || U32(stats, 8))
        return -16;
    int error = ((int (*)(void *, void *))0x35d81u)(wlc, nan);
    if (error) {
        int cleanup = nan_ndp_deinitialize(wlc, nan);
        return cleanup ? cleanup : error;
    }
    /* Native init sets AUTO_DPCONF. Clear all automatic negotiation bits
     * before exposing initialized state to the rest of the firmware. */
    U32(PTR(nan, 0x10), 0x34) &= ~NAN_CONTROL_AUTOMATIC;
    U32(PTR(nan, 0x10), 0x34) |= NAN_CONTROL_HOST_NDP_ID;
    U32(ndp, NDP_INITIALIZED) = 1;
    return 0;
}

int nan_link_context_detach(void *nan)
{
    if (!nan)
        return -2;
    int error = nan_ndp_deinitialize(PTR(nan, 8), nan);
    if (!error)
        error = nan_ndl_deinitialize(PTR(nan, 8), nan);
    if (error)
        return error;
    void *ndp = PTR(nan, 0x34);
    if (ndp) {
        release(PTR(ndp, 0x24));
        release(PTR(ndp, 0x10));
        release(ndp);
        PTR(nan, 0x34) = 0;
    }
    release(PTR(nan, 0x38));
    PTR(nan, 0x38) = 0;
    return 0;
}

#include "native_calls.h"
/* A2 native peer/SCB attachment. A2 stores its SCB cubby getter at core+0x94
 * and its age limit at +0x98; B0's +0x88/+0x56 layout is incompatible. */
#include <stddef.h>
#include <stdint.h>
#include "nan_peer.h"
#include "nan_data.h"
#include "awdl_session.h"

#define FIELD(type, base, offset) (*(type *)((uint8_t *)(base) + (offset)))
#define PTR(base, offset) FIELD(void *, base, offset)
#define U32(base, offset) FIELD(uint32_t, base, offset)
#define U16(base, offset) FIELD(uint16_t, base, offset)
#define U8(base, offset) FIELD(uint8_t, base, offset)
#define PEER_BYTES 0x88u

static int peer_scb_initialize(void *nan, void *scb)
{
    /* Native 0x36e40 requires an NDP peer for every unicast NDI SCB.
     * AWDL owns its peer independently and must leave this NAN cubby empty.
     * Native 0x36df4 deinit explicitly accepts that empty cubby. */
    if (awdl_session_holds_bss(nan, PTR(scb, 0x0c)))
        return 0;
    return ((int (*)(void *, void *))0x36e41u)(nan, scb);
}

int nan_peer_context_attach(void *wlc, void *nan)
{
    if (!wlc || !nan || PTR(nan, 8) != wlc || !PTR(wlc, 0x5c))
        return -2;
    if (PTR(nan, 0x94) || PTR(nan, 0x40))
        return -16;
    void *cubbies = PTR(PTR(wlc, 0x5c), 0x0c);
    if (!cubbies || !PTR(cubbies, 0x18) || !PTR(cubbies, 0x1c))
        return -2;
    void *getter = ((void *(*)(size_t))native_0x7146d)(8);
    if (!getter)
        return -27;
    int error = ((int (*)(void *, void *, unsigned, uintptr_t, uintptr_t,
                         uintptr_t, void *))native_0x233ed9)(
        wlc, getter, 12, (uintptr_t)peer_scb_initialize, 0x36df5u, 0, nan);
    if (error) {
        ((void (*)(void *))native_0x71491)(getter);
        return error;
    }
    PTR(nan, 0x94) = getter;
    U32(nan, 0x3c) = PEER_BYTES;
    /* Same native age threshold as the reference: 20 public firmware clock
     * ticks. A2 watchdog 0x37a5c compares this against wlc_cfg_get_now. */
    U16(nan, 0x98) = 20;
    /* A2 0x36ed0 (the reference's SCB state-update callback) is bx lr;
     * registering that empty notification has no functional effect. The
     * actual SCB init/deinit callbacks above are owned by the native cubby. */
    return 0;
}

static int valid_context(void *wlc, void *nan)
{
    return wlc && nan && PTR(nan, 8) == wlc && PTR(nan, 0x94) &&
           U32(nan, 0x3c) == PEER_BYTES && PTR(nan, 0x6c) &&
           U8(PTR(nan, 0x6c), 2) && U8(PTR(nan, 0x6c), 2) <= 32 &&
           PTR(nan, 0x74);
}

static int peer_has_resources(void *peer)
{
    return (U16(peer, 0) & 1u) || U8(peer, 9) || PTR(peer, 0x0c) ||
           PTR(peer, 0x10) || PTR(peer, 0x24) || PTR(peer, 0x2c) ||
           PTR(peer, 0x30) || PTR(peer, 0x38) || PTR(peer, 0x4c) ||
           PTR(peer, 0x50) || PTR(peer, 0x58) || PTR(peer, 0x64);
}

int nan_peer_check_stations(void *nan, void *peer, const unsigned char *ndi)
{
    if (!nan || !valid_context(PTR(nan, 8), nan) || !peer)
        return -2;
    void *wlc = PTR(nan, 8);
    void *scbs = PTR(wlc, 0x5c);
    void *getter = PTR(nan, 0x94);
    if (!scbs || PTR(scbs, 0) != wlc || !PTR(wlc, 0) ||
        !PTR(PTR(wlc, 0), 4) || !PTR(wlc, 0x1bc) ||
        U32(getter, 0) != 0x1d33d9u)
        return -30;
    uint32_t iterator[3] = {0, 0, 0};
    ((void (*)(void *, void *, void *))native_0x1239a9)(scbs, iterator, 0);
    void *scb;
    while ((scb = ((void *(*)(void *, void *))native_0x1239e9)(scbs, iterator))) {
        void *bss = PTR(scb, 0x0c);
        if (bss && U32(bss, 0x74) == 5 &&
            (U32(bss, 0x78) == 0x0b || U32(bss, 0x78) == 0x0c) &&
            PTR(scb, U32(getter, 4)) == peer &&
            (!ndi || !((int (*)(const void *, const void *, size_t))native_0xa7a9)(
                (uint8_t *)scb + 0x10, ndi, 6)))
            return -16;
    }
    return 0;
}

int nan_peer_release(void *nan, void *peer)
{
    if (!nan || !valid_context(PTR(nan, 8), nan) || !peer || !PTR(nan, 0x40))
        return -2;
    uintptr_t start = (uintptr_t)PTR(nan, 0x40);
    uintptr_t offset = (uintptr_t)peer - start;
    if ((uintptr_t)peer < start || offset % PEER_BYTES ||
        offset / PEER_BYTES >= U8(PTR(nan, 0x6c), 2))
        return -2;
    /* ROM 0x37214 returns success while references remain, and ignores the
     * NDL deactivate error before freeing the peer's maps. Preserve both. */
    if (U8(peer, 9) || PTR(peer, 0x24) || PTR(peer, 0x30) || PTR(peer, 0x58))
        return -16;
    if (!PTR(nan, 0x38) || !PTR(nan, 0x18) || !PTR(nan, 0x14) ||
        !PTR(nan, 0x5c) || !PTR(nan, 0x4c))
        return -30;
    int error = nan_peer_check_stations(nan, peer, 0);
    if (error)
        return error;
    error = ((int (*)(void *, void *, unsigned))native_0x34279)(
        PTR(nan, 0x38), peer, 1);
    if (error)
        return error;
    if (PTR(peer, 0x0c)) {
        ((void (*)(void *, void *))native_0x30a55)(nan, PTR(peer, 0x0c));
        PTR(peer, 0x0c) = 0;
    }
    ((void (*)(void *, void *))native_0x34a59)(nan, peer);
    ((void (*)(void *, void *))native_0x34315)(PTR(nan, 0x38), peer);
    ((void (*)(void *, void *))native_0x36cfd)(nan, peer);
    /* Pending FSM and management frames were excluded above. Retain the
     * real native discovery notification; no fabricated TX completion. */
    ((void (*)(void *, void *, unsigned, unsigned, unsigned))native_0x261a1)(
        PTR(nan, 0x18), peer, 0x72000, 0, 1);
    ((void (*)(void *, void *))native_0x2ed61)(PTR(nan, 0x14), peer);
    ((void (*)(void *, void *))native_0x33a49)(nan, (uint8_t *)peer + 0x50);
    U16(peer, 0) = 0;
    return 0;
}

int nan_peer_disable(void *wlc, void *nan)
{
    if (!valid_context(wlc, nan))
        return -2;
    if (!PTR(nan, 0x40))
        return 0;
    for (unsigned i = 0; i < U8(PTR(nan, 0x6c), 2); ++i) {
        void *peer = (uint8_t *)PTR(nan, 0x40) + i * PEER_BYTES;
        if (!peer_has_resources(peer))
            continue;
        int error = nan_peer_release(nan, peer);
        if (error)
            return error;
    }
    return 0;
}

int nan_peer_check_idle(void *nan)
{
    if (!nan)
        return -2;
    void *peers = PTR(nan, 0x40);
    if (peers) {
        if (!valid_context(PTR(nan, 8), nan))
            return -30;
        for (unsigned i = 0; i < U8(PTR(nan, 0x6c), 2); ++i) {
            void *peer = (uint8_t *)peers + i * PEER_BYTES;
            if (peer_has_resources(peer))
                return -16;
        }
    }
    return 0;
}

int nan_peer_deinitialize(void *wlc, void *nan)
{
    if (!valid_context(wlc, nan))
        return -2;
    int error = nan_data_check_idle(nan);
    if (!error)
        error = nan_peer_check_idle(nan);
    if (error)
        return error;
    void *mac = PTR(nan, 0x14);
    if (mac && PTR(mac, 0x38) && U8(PTR(mac, 0x38), 0x4a))
        return -16;
    void *peers = PTR(nan, 0x40);
    void *stats = (uint8_t *)PTR(nan, 0x74) + 4 * 12;
    if ((U32(stats, 0) && U32(stats, 0) != 0x36d11u) ||
        U32(stats, 4) || U32(stats, 8))
        return -30;
    ((void (*)(void *, size_t))native_0x18660b)(stats, 12);
    if (peers) {
        ((void (*)(void *))native_0x71491)(peers);
        PTR(nan, 0x40) = 0;
    }
    return 0;
}

int nan_peer_initialize(void *wlc, void *nan)
{
    if (!valid_context(wlc, nan))
        return -2;
    void *stats = (uint8_t *)PTR(nan, 0x74) + 4 * 12;
    if (PTR(nan, 0x40) || U32(stats, 0) || U32(stats, 4) || U32(stats, 8))
        return -16;
    int error = ((int (*)(void *, void *))native_0x3711d)(wlc, nan);
    if (error) {
        /* Native init registers statistics even when allocation fails. Remove
         * that borrow on failure; preserve the native allocation error. */
        int cleanup = nan_peer_deinitialize(wlc, nan);
        return cleanup ? cleanup : error;
    }
    return 0;
}

/* A2 NAN BSS hardware preparation and NMI TBTT callback ownership. The original
 * preparation ignores slotted-BSS errors; the original event lifecycle cannot
 * distinguish a failed registration from an owned callback on STOP/retry. */
#include <stddef.h>
#include <stdint.h>
#include "nan_mac.h"

#define FIELD(type, base, offset) (*(type *)((uint8_t *)(base) + (offset)))
#define PTR(base, offset) FIELD(void *, base, offset)
#define U32(base, offset) FIELD(uint32_t, base, offset)
#define U16(base, offset) FIELD(uint16_t, base, offset)
#define U8(base, offset) FIELD(uint8_t, base, offset)

int nan_bss_prepare_enable(void *nan, void *bss, unsigned band, unsigned index)
{
    if (!nan || !bss || !PTR(nan, 8) || PTR(bss, 0) != PTR(nan, 8) ||
        !PTR(nan, 0x14) || !PTR(nan, 0x2c) || band)
        return -2;
    void *wlc = PTR(nan, 8);
    void *mac = PTR(nan, 0x14);
    if (!PTR(wlc, 0) || !PTR(wlc, 0x34) ||
        !PTR(wlc, 0x1f8) || !PTR(wlc, 0x678) || !PTR(bss, 0x94) ||
        !PTR(bss, 0x98) || !PTR(bss, 0xa0) || !PTR(bss, 0x2e0))
        return -25;
    if (U32(bss, 0x74) != 5 || (U32(bss, 0x78) != 10 && U32(bss, 0x78) != 11) ||
        ((void *(*)(void *, unsigned))0x1279d1u)(PTR(wlc, 0x678), index) != bss ||
        (U32(bss, 0x78) == 10 && index != U32(mac, 0xd4)))
        return -30;
    void *defaults = PTR(wlc, 0x1f8);
    uint16_t old_channel = U16(wlc, 0x28c);
    uint16_t old_interval = U16(defaults, 0x32);
    uint16_t old_default_channel = U16(defaults, 0x36);
    /* Native SSID setter takes (BSS, bytes, length). Even for an empty
     * NAN SSID, r2 must be zero; the ROM copies r2 bytes before clearing
     * the remaining 32-byte field. */
    ((void (*)(void *, const void *, unsigned))0xbb775u)(bss, 0, 0);
    U8(bss, 0x10) = 0;
    unsigned channel = ((unsigned (*)(void *, unsigned))0x2f33du)(nan, band);
    if (*(const uint8_t *)(uintptr_t)*(const uint32_t *)0x1fee0u)
        ((void (*)(void *))0x8eafdu)(wlc);
    if (channel == 0xff || ((int (*)(void *, unsigned))0xc7845u)(PTR(nan, 0x2c), channel)) {
        channel = old_channel;
        if (!((int (*)(void *, unsigned))0xc815du)(PTR(nan, 0x2c), channel) ||
            ((int (*)(void *, unsigned))0xc7845u)(PTR(nan, 0x2c), channel))
            channel = U16(wlc, 0x28e);
    }
    U16(PTR(bss, 0x94), 0x36) = channel;
    U16(defaults, 0x32) = ((unsigned (*)(void *))0x2f185u)(nan);
    U16(defaults, 0x36) = channel;
    uint8_t cluster[6];
    ((void (*)(void *, unsigned, void *))0x2efd5u)(nan, 1, cluster);
    ((int (*)(void *, size_t, const void *, size_t))0x151c81u)(
        (uint8_t *)bss + 0x60, 6, cluster, 6);
    int error = ((int (*)(void *, unsigned, const void *))0x128415u)(PTR(wlc, 0x678), index, cluster);
    if (error)
        ((int (*)(const char *, ...))0xa815u)("NAN BSS address: %d\n", error);
    if (!error)
        error = ((int (*)(void *, unsigned))0x127ae1u)(PTR(wlc, 0x678), index);
    if (error)
        ((int (*)(const char *, ...))0xa815u)("NAN BSS prepare: %d\n", error);
    /* The native operation temporarily borrows WLC's default BSS settings.
     * Restore them on failure too; any active BSS remains owned for STOP. */
    U16(defaults, 0x32) = old_interval;
    U16(defaults, 0x36) = old_default_channel;
    if (!error) {
        U8(PTR(bss, 0xa0), 0xc1) = 0;
        U32(PTR(bss, 0x2e0), 0) = 1;
        if (!((int (*)(void *))0x127acdu)(wlc) && !U8(PTR(wlc, 0x34), 0x10)) {
            error = -12;
        } else {
            void *queue = ((void *(*)(void *, void *))0x8aae5u)(wlc, bss);
            error = ((int (*)(void *, void *, unsigned, unsigned, void *))0xbc009u)(
                wlc, bss, 2, 0x14, queue);
            if (error)
                ((int (*)(const char *, ...))0xa815u)("NAN BSS rates: %d\n", error);
            if (!error) {
                void *rates = ((void *(*)(void *))0x1279cdu)(bss);
                if (rates) {
                    unsigned band_type = (U8(PTR(wlc, 0), 0x57) & 3u) ?
                        ((U32(bss, 0x70) ^ 0x1000u) >> 12) & 1u : 0;
                    ((void (*)(void *, void *, unsigned, unsigned, unsigned, unsigned))0x10e5c1u)(
                        (uint8_t *)PTR(bss, 0x98) + 0x3c, (uint8_t *)rates + 0x34,
                        0, 2, 0x7f, band_type);
                }
            }
        }
    }
    U16(wlc, 0x28c) = old_channel;
    return error;
}

int nan_mac_stop_bss(void *mac, void *bss)
{
    if (!mac || !bss || !PTR(bss, 0) || PTR(mac, 0x10) != PTR(bss, 0))
        return -2;
    void *wlc = PTR(bss, 0);
    void *manager = PTR(mac, NAN_MAC_BSS_MANAGER);
    void *node = PTR(mac, NAN_MAC_BSS_NODE);
    if (!manager || manager != PTR(wlc, 0x678) || !node ||
        ((void *(*)(void *, unsigned))0x126641u)(manager, U32(mac, 0xd4)) != node ||
        ((void *(*)(void *, unsigned))0x1279d1u)(manager, U32(mac, 0xd4)) != bss)
        return -30;
    return nan_bss_stop(PTR(mac, 4), bss, U32(mac, 0xd4));
}

int nan_bss_stop(void *nan, void *bss, unsigned index)
{
    if (!nan || !bss || !PTR(nan, 8) || PTR(bss, 0) != PTR(nan, 8) ||
        U32(bss, 0x74) != 5 || (U32(bss, 0x78) != 10 && U32(bss, 0x78) != 11))
        return -2;
    void *wlc = PTR(nan, 8), *manager = PTR(wlc, 0x678);
    if (!manager || ((void *(*)(void *, unsigned))0x1279d1u)(manager, index) != bss)
        return -30;
    void *node = ((void *(*)(void *, unsigned))0x126641u)(manager, index);
    if (!node)
        return -30;
    /* ade8c clears the BSS flag before the remaining stop operations can
     * fail. The native node's active flag must also be clear before STOP is
     * complete; otherwise a retry would incorrectly skip unfinished work. */
    if (!U8(bss, 5) && !U8(node, 1))
        return 0;
    if (!PTR(bss, 0x9c) || !PTR(manager, 4) || !PTR(PTR(manager, 4), 8))
        return -25;
    U8(node, 1) = 1;
    ((void (*)(void *, unsigned, unsigned))0xade8du)(bss, 0, 1);
    if (*(const uint8_t *)(uintptr_t)*(const uint32_t *)0x1288a8u)
        ((void (*)(void *))0x8eafdu)(wlc);
    int error = ((int (*)(void *, unsigned))0xaf015u)(bss, !U8(PTR(bss, 0x9c), 0x4a));
    if (!error)
        error = ((int (*)(void *, void *))0xbbad1u)(wlc, bss);
    if (error)
        return error;
    void *table = PTR(PTR(manager, 4), 8);
    if (!PTR(table, 0))
        return -30;
    unsigned active = 0;
    for (unsigned i = 0; i < U32(table, 8); ++i) {
        void *other = ((void **)PTR(table, 0))[i];
        if (other && U8(other, 1))
            active = 1;
    }
    if (!active)
        U32(manager, 0x44) = 0;
    error = ((int (*)(void *, unsigned))0x12651du)(manager, 0);
    if (error || U8(bss, 5))
        return error ? error : -16;
    U8(node, 1) = 0;
    return 0;
}

static int registered(void *manager, void *bss, void *mac)
{
    /* Native 0x1306b8/0x130968 cubby lookup, without allocating on lookup. */
    uint32_t token = U32(manager, 4);
    void *segments = PTR(bss, 0x304);
    if (!segments || !PTR(segments, (token >> 28) * 4))
        return 0;
    void *state = PTR(PTR(segments, (token >> 28) * 4), token & 0x0fffffffu);
    if (!state || !PTR(state, 0x1c))
        return 0;
    for (void *node = PTR(PTR(state, 0x1c), 0); node; node = PTR(node, 8))
        if (PTR(node, 0) == mac && U32(node, 4) == 0x2f759u)
            return 1;
    return 0;
}

int nan_bss_events(void *nan, unsigned enable)
{
    if (!nan || !PTR(nan, 8) || !PTR(nan, 0x14) || enable > 1)
        return -2;
    void *mac = PTR(nan, 0x14);
    void *wlc = PTR(nan, 8);
    void *owned = PTR(mac, NAN_MAC_EVENT_BSS);
    void *manager = PTR(wlc, 0x98);
    if (!enable && !owned)
        return PTR(mac, NAN_MAC_EVENT_MANAGER) ? -30 : 0;
    if (!manager || !PTR(mac, 8) || !PTR(PTR(mac, 8), 0))
        return -25;
    void *bss = PTR(PTR(PTR(mac, 8), 0), 0);
    if (!bss || PTR(bss, 0) != wlc || U32(bss, 0x74) != 5 || U32(bss, 0x78) != 10)
        return -30;
    if (!PTR(bss, 0x98))
        return -25;
    if (owned && (owned != bss || PTR(mac, NAN_MAC_EVENT_MANAGER) != manager))
        return -30;
    if (enable) {
        if (owned)
            return registered(manager, bss, mac) ? 0 : -30;
        if (PTR(mac, NAN_MAC_EVENT_MANAGER) || registered(manager, bss, mac))
            return -30;
        int error = ((int (*)(void *, void *, uintptr_t, uintptr_t, void *))0x1308e1u)(
            manager, bss, 0x2f759u, 0, mac);
        if (registered(manager, bss, mac)) {
            PTR(mac, NAN_MAC_EVENT_BSS) = bss;
            PTR(mac, NAN_MAC_EVENT_MANAGER) = manager;
        }
        if (error || !PTR(mac, NAN_MAC_EVENT_BSS))
            return error ? error : -30;
        U16(PTR(bss, 0x98), 0x32) = ((unsigned (*)(void *))0x2f185u)(nan);
        ((void (*)(void *, void *))0x130a11u)(manager, bss);
        ((void (*)(void *, void *, unsigned))0xef5fdu)(PTR(wlc, 0x94), bss, 1);
        return 0;
    }
    int error = ((int (*)(void *, void *, uintptr_t, uintptr_t, void *))0x130969u)(
        manager, bss, 0x2f759u, 0, mac);
    if (error || registered(manager, bss, mac))
        return error ? error : -16;
    PTR(mac, NAN_MAC_EVENT_BSS) = 0;
    PTR(mac, NAN_MAC_EVENT_MANAGER) = 0;
    return 0;
}

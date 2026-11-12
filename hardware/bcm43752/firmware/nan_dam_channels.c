#include "native_calls.h"
/* A2 DAM channel decisions. Keep the chip's CLM/channel-list consumers and
 * primary-channel decoder; B0's DAM fields after +0x18 are shifted by four
 * bytes and cannot be used as A2 offsets. These callbacks do not create an
 * infrastructure BSS, switch channels or enable another wireless mode.
 */
#include <stdint.h>
#include "nan_dam.h"

#define PTR(base, offset) (*(void **)((uint8_t *)(base) + (offset)))
#define U8(base, offset) (*(uint8_t *)((uint8_t *)(base) + (offset)))

int nan_dam_update_social_channels(void *dam, unsigned dual_band)
{
    if (!dam || !PTR(dam, 4))
        return -2;
    void *list = PTR(dam, 0x34);
    if (!list)
        return 0; /* Native optional list, not an allocated owner to replace. */
    void *nan = PTR(dam, 4);
    void *mac = PTR(nan, 0x14);
    if (!PTR(dam, 0x44) || (dual_band && (!mac || !PTR(mac, 0x20))))
        return -25;
    /* A2 0x302b4 appends distinct chanspecs to a four-byte header followed
     * by uint16_t entries. Stage at most six candidates; an undersized
     * destination must retain its previous list, including its count. */
    struct {
        uint8_t capacity, count;
        uint16_t reserved;
        uint16_t channels[6];
    } replacement;
    replacement.capacity = U8(list, 0) < 6 ? U8(list, 0) : 6;
    replacement.count = 0;
    replacement.reserved = 0;
    uint16_t channels[6];
    channels[0] = 0x1006;
    unsigned count = 1;
    if (dual_band) {
        unsigned social = ((unsigned (*)(void *, unsigned))native_0x2f33d)(nan, 1);
        if (social != 0xff)
            channels[count++] = social;
        channels[count++] = 0xd095; /* 149 / 20 MHz. */
        channels[count++] = 0xd02c; /* 44 / 20 MHz. */
    }
    unsigned mode = U8(PTR(dam, 0x44), 2);
    if (mode == 0 || mode == 2)
        channels[count++] = 0x1001;
    if (mode == 0 || mode == 1)
        channels[count++] = 0x100b;
    for (unsigned i = 0; i < count; i++) {
        int error = ((int (*)(void *, void *, unsigned))native_0x302b5)(
            nan, &replacement, channels[i]);
        if (error)
            return error;
    }
    ((void (*)(void *))native_0x30849)(list);
    for (unsigned i = 0; i < replacement.count; i++)
        ((uint16_t *)((uint8_t *)list + 4))[i] = replacement.channels[i];
    U8(list, 1) = replacement.count;
    return 0;
}

int nan_dam_compatible_with_bt(void *dam, unsigned chanspec)
{
    if (!dam || !PTR(dam, 0x44))
        return 0;
    /* Native +0x44 is the three-byte BT state, not a BSS owner. The
     * selected half-band is byte 2; zero leaves all primary channels. */
    unsigned mode = U8(PTR(dam, 0x44), 2);
    unsigned primary = ((unsigned (*)(unsigned))native_0x183601)(chanspec);
    return mode == 1 ? primary > 5 : mode == 2 ? primary < 7 : 1;
}

int nan_dam_valid_chanspec(void *dam, void *prohibited, unsigned chanspec,
                          unsigned checks)
{
    if (!dam || !PTR(dam, 4))
        return 0;
    void *nan = PTR(dam, 4);
    void *local_wlc = PTR(nan, 8);
    if (!local_wlc || !PTR(local_wlc, 0x18) || !PTR(local_wlc, 0xac))
        return 0;
    /* A2 callers use bit 0 for local CLM/BT/list checks, bit 1 for the
     * supplied prohibited list and bit 2 to omit the local list check.
     * Native radar/restricted-channel rejection still applies afterward. */
    if (checks & 1u) {
        unsigned band = chanspec & 0xc000u;
        if (band != 0 && band != 0xc000u)
            return 0; /* A2 NAN exposes only 2.4 and 5 GHz. */
        void *wlc = ((void *(*)(void *, unsigned))native_0x20fdd)(nan, band != 0);
        if (!wlc || !PTR(wlc, 0xac) ||
            !((int (*)(void *, unsigned))native_0xc815d)(PTR(wlc, 0xac), chanspec) ||
            (!band && !nan_dam_compatible_with_bt(dam, chanspec)))
            return 0;
        if (!(checks & 4u)) {
            if (!PTR(dam, 0x28) ||
                ((int (*)(void *, unsigned, void *))native_0x3180d)(
                    nan, chanspec, PTR(dam, 0x28)))
                return 0;
        }
    }
    if ((checks & 2u) && prohibited &&
        ((int (*)(void *, unsigned, void *))native_0x3180d)(nan, chanspec, prohibited))
        return 0;
    /* 0x21fc4 obtains the channel manager through the prepared NMI BSS.
     * A missing BSS is not evidence that a channel is permitted. */
    void *mac = PTR(nan, 0x14);
    if (!mac || !PTR(mac, 8) || !PTR(PTR(mac, 8), 0) ||
        !PTR(PTR(PTR(mac, 8), 0), 0))
        return 0;
    void *bss = PTR(PTR(PTR(mac, 8), 0), 0);
    if (PTR(bss, 0) != local_wlc)
        return 0;
    return !((int (*)(void *, unsigned))native_0x21fc5)(dam, chanspec);
}

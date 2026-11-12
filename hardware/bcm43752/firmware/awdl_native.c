#include "native_calls.h"
#include "awdl_native.h"
#include "nan_host.h"

#define PTR(base, offset) (*(void **)((uint8_t *)(base) + (offset)))

int awdl_native_clock(void *nan, uint64_t *microseconds)
{
    if (!nan || !microseconds || !PTR(nan, 8))
        return -2;
    void *msch = PTR(PTR(nan, 8), 0x574);
    if (!msch)
        return -25;
    /* Native nan_scheduler_register uses this same microsecond clock.
     * The slotted-BSS watchdog clock at 0x127a78 has a different unit. */
    *microseconds = ((uint64_t (*)(void *))native_0xf0ddd)(msch);
    return 0;
}

int awdl_native_rx_clock(void *nan, const void *rx, uint64_t *microseconds)
{
    uint64_t before, after;
    int error = awdl_native_clock(nan, &before);
    if (error)
        return error;
    void *wlc = PTR(nan, 8);
    uint32_t low, high;
    /* A2 0xef500 uses this TSF read with the same RX metadata and
     * 0x8c8f4/0x8c8e0 recovery. The RX low sixteen bits are at +0x2a;
     * native recovery anchors them to the captured TSF at RX metadata+0.
     * TSF and MSCH are different clocks: transfer only the elapsed age,
     * never install a raw MAC timestamp as an MSCH deadline. */
    ((void (*)(void *, uint32_t *, uint32_t *))native_0x8c8d9)(wlc, &low, &high);
    uint32_t received = ((uint32_t (*)(void *, const void *))native_0x8c8e1)(wlc, rx);
    error = awdl_native_clock(nan, &after);
    if (error)
        return error;
    uint32_t age = low - received;
    if (after < before || age > INT32_MAX)
        return -2;
    uint64_t sampled = before + (after - before) / 2;
    if (age > sampled)
        return -2;
    *microseconds = sampled - age;
    return 0;
}

int awdl_native_channels(void *nan, uint16_t primary, uint16_t secondary)
{
    if (!nan || !PTR(nan, 0x2c) || !nan_host_active(nan))
        return -25;
    /* A2 VHT80: band/width 0xe000, sideband 0..3, center in low byte.
     * Follow only within the existing 149/161 block (center 155), matching
     * the public AWDL controller's implemented channel-pair contract. */
    if ((primary & 0xf800u) != 0xe000u || (primary & 255u) != 155 ||
        (((primary >> 8) & 7u) != 0 && ((primary >> 8) & 7u) != 3))
        return -20;
    if (secondary && (secondary == primary ||
        (secondary & 0xf800u) != 0xe000u || (secondary & 255u) != 155 ||
        (((secondary >> 8) & 7u) != 0 && ((secondary >> 8) & 7u) != 3)))
        return -20;
    int (*legal)(void *, unsigned) = (int (*)(void *, unsigned))0xc815du;
    if (!legal(PTR(nan, 0x2c), primary) ||
        (secondary && !legal(PTR(nan, 0x2c), secondary)))
        return -20;
    return 0;
}

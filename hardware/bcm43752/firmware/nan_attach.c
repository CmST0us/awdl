#include "native_calls.h"
/* Boot-only A2 attachment, after the original WLC module dependencies.
 * A WLC attach failure terminates this firmware instance at ROM 0x71514.
 * Published contexts therefore remain owned by WLC until that termination;
 * never free a context still referenced by a cubby or notification callback.
 */
#include <stddef.h>
#include <stdint.h>
#include "nan_commands.h"
#include "nan_mac.h"
#include "nan_discovery.h"
#include "nan_availability.h"
#include "nan_dam.h"
#include "nan_scheduler.h"
#include "nan_data.h"
#include "nan_peer.h"
#include "nan_link.h"
#include "nan_initialize.h"
#include "nan_fsm.h"

#define FIELD(type, base, offset) (*(type *)((uint8_t *)(base) + (offset)))
#define PTR(base, offset) FIELD(void *, base, offset)
#define U32(base, offset) FIELD(uint32_t, base, offset)
#define U16(base, offset) FIELD(uint16_t, base, offset)
#define U8(base, offset) FIELD(uint8_t, base, offset)

struct nan_iovar {
    const char *name;
    uint16_t id, flags;
    uint8_t flags2, type;
    uint16_t minimum;
};
_Static_assert(sizeof(struct nan_iovar) == 12, "native iovar table stride");
/* Native versioned/batch NAN wire protocol, variable-length IOVT_BUFFER.
 * The parser and its registered handlers enforce their exact lengths. */
static const struct nan_iovar iovars[] = {{"nan", 0, 0, 0, 8, 0}, {0}};

static void *allocate(size_t bytes)
{
    return ((void *(*)(size_t))native_0x7146d)(bytes);
}

static int attach_core(void *wlc)
{
    if (!wlc || !PTR(wlc, 0) || !PTR(PTR(wlc, 0), 4) || U32(PTR(wlc, 0), 0x10))
        return -2;
    if (PTR(wlc, 0x520))
        return -16;
    void *wrapper = allocate(8);
    if (!wrapper)
        return -27;
    PTR(wrapper, 0) = wlc;
    PTR(wlc, 0x520) = wrapper;
    /* A2 SCB getter and peer age occupy +0x94/+0x98. Unlike B0, +0x78
     * and +0x7c are pause bitmaps, not allocated-pointer slots. */
    void *nan = allocate(0x9c);
    PTR(wrapper, 4) = nan;
    if (!nan)
        return -27;
    U32(nan, 0) = 0x5afec0de;
    PTR(nan, 4) = PTR(wlc, 4);
    PTR(nan, 8) = wlc;
    PTR(nan, 0x0c) = PTR(wlc, 0);
    U8(nan, 0x80) = 0x2a; /* A2 attribute-index bound, not a pointer. */

    PTR(nan, 0x6c) = allocate(0x40);
    if (!PTR(nan, 0x6c))
        return -27;
    void *config = PTR(nan, 0x6c);
    U16(config, 0) = 2048; /* Native common command/work buffer, bytes. */
    U8(config, 2) = 8; /* Peer rows, <=32 due to native NDL bitmap. */
    U8(config, 3) = 2; /* Local availability-map entries. */
    U8(config, 4) = 8; /* Peer availability-map entries. */
    U8(config, 5) = 2; /* Native NDI slots, required by nan_data.c. */
    U8(config, 9) = 8; /* NDP records, host-assigned IDs. */
    U8(config, 0x0d) = 20; /* Channel-list capacity. */
    U8(config, 0x0e) = 12; /* Operating-class-list capacity. */
    U16(config, 0x10) = 1500; /* NAN action body limit, bytes, including OUI header. */
    U8(config, 0x14) = 12; /* Management legacy rate, units of 500 kbit/s: 6 Mbit/s. */
    U8(config, 0x20) = 2; /* Native device-capability list entries. */
    /* 0x1dbc8/0x1dbdc expose this as MIN_TX_RATE, not a stream count.
     * Nonzero makes 0x2d6b4 discard the fixed management ratespec. Keep
     * native peer rate adaptation and the explicit 6-Mbit/s discovery rate. */
    U8(config, 0x21) = 0;
    U8(config, 0x22) = 4; /* Native NDP heartbeat-count default. */
    PTR(nan, 0x10) = allocate(0x4c);
    if (!PTR(nan, 0x10))
        return -27;
    void *common = PTR(nan, 0x10);
    U32(common, 0) = 0x804d414d; /* Native common-state tag. */
    U8(common, 0x16) = 0x50;
    U8(common, 0x17) = 0x6f;
    U8(common, 0x18) = 0x9a; /* WFA OUI for native NAN frames. */
    U8(common, 0x19) = 0x13; /* NAN OUI type. */
    /* 0x2c328 checks these four bits before discovery/sync beacon TX on
     * the two NAN bands. Missing defaults leave every beacon suppressed. */
    U32(common, 0x34) = 0x1e;
    PTR(common, 0x38) = allocate(8);
    if (!PTR(common, 0x38))
        return -27;
    PTR(PTR(common, 0x38), 4) = allocate(16); /* Native event mask, bytes. */
    if (!PTR(PTR(common, 0x38), 4))
        return -27;
    PTR(nan, 0x70) = allocate(3); /* Native optional-security flags, disabled. */
    PTR(nan, 0x74) = allocate(13 * 12); /* Native borrowed statistics entries. */
    PTR(nan, 0x84) = allocate(40); /* Native ID bitmap header + IDs 1..255. */
    PTR(nan, 0x90) = allocate(4); /* Native suppressed-event counter. */
    if (!PTR(nan, 0x70) || !PTR(nan, 0x74) || !PTR(nan, 0x84) || !PTR(nan, 0x90))
        return -27;
    void *ids = PTR(nan, 0x84);
    U8(ids, 0) = 40;
    U8(ids, 1) = 1;
    U8(ids, 2) = 255;
    U8(ids, 4) = 255;
    return 0;
}

int nan_boot_attach(void *wlc)
{
    int error = attach_core(wlc);
    if (error)
        return error;
    void *wrapper = PTR(wlc, 0x520);
    void *nan = PTR(wrapper, 4);
    error = nan_commands_attach(nan);
    if (!error)
        error = nan_mac_context_attach(wlc, nan);
    if (!error)
        error = nan_discovery_context_attach(nan);
    if (!error)
        error = nan_dam_context_attach(nan);
    if (!error)
        error = nan_availability_context_attach(nan);
    if (!error)
        error = nan_scheduler_context_attach(wlc, nan);
    if (!error)
        error = nan_link_context_attach(nan);
    if (!error)
        error = nan_data_context_attach(wlc, nan);
    if (!error)
        error = nan_peer_context_attach(wlc, nan);
    if (!error)
        error = nan_fsm_context_attach(nan);
    if (error)
        return error;
    /* Native up is deliberately empty; down invokes the ownership-preserving
     * NAN stop path. The host backend owns protocol negotiation while these
     * callbacks retain native MAC, packet and interface lifetimes. */
    error = ((int (*)(void *, const void *, const char *, void *, uintptr_t,
                      uintptr_t, uintptr_t, uintptr_t, unsigned, void *))native_0x225bdf)(
        PTR(wlc, 0), iovars, "nan", wrapper, 0x1f5d5u,
        (uintptr_t)nan_watchdog, 0x1eaccu | 1u, (uintptr_t)nan_wlc_down, 0, 0);
    if (error)
        return error;
    /* A2 WLC core attach publishes PCB at +0x2c4 (RAM 0x225038).
     * The packet tag's group 0 / index 12 dispatches native NAN TX status. */
    error = ((int (*)(void *, unsigned, unsigned, uintptr_t))native_0x231c41)(
        PTR(wlc, 0x2c4), 0, 12, 0x1e54du);
    if (error)
        return error;
    /* Original WLC interface type 3 maps only to NAN NMI/NDI subtypes.
     * Retain actual create/remove errors and their slotted-BSS ownership. */
    error = ((int (*)(void *, unsigned, uintptr_t, uintptr_t, unsigned, void *))native_0x22b2b9)(
        wlc, 3, 0x1ebb1u, 0x1ec75u, 0, wrapper);
    if (error)
        return error;
    return ((int (*)(void *, uintptr_t, void *))native_0x22b2a1)(
        wlc, 0x1ead1u, wrapper);
}

int nan_wlc_attach_modules(void *wlc)
{
    int error = ((int (*)(void *))native_0x2251b5)(wlc);
    if (error)
        return error;
    return nan_boot_attach(wlc);
}

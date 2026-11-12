#include "native_calls.h"
/* Native A2 data-path exchange dispatch. The ROM engine at 0x28e7c
 * matches byte state / byte event, then invokes the three-argument handler.
 * These are A2 ROM addresses, never pointers from the B0 reference image.
 * Hostap continues to own protocol negotiation; restoring the table does
 * not register autonomous commands or route management RX into this FSM.
 */
#include <stddef.h>
#include <stdint.h>
#include "nan_fsm.h"
#include "nan_peer.h"

#define FIELD(type, base, offset) (*(type *)((uint8_t *)(base) + (offset)))
#define PTR(base, offset) FIELD(void *, base, offset)
#define U32(base, offset) FIELD(uint32_t, base, offset)
#define U8(base, offset) FIELD(uint8_t, base, offset)

/* Preserve the native 0x14-byte header and its five 0x18-byte registration
 * slots. Implementation owners belong to this NAN instance, at +0x14;
 * unlike the reference, the immutable implementation table owns no heap.
 */
#define FSM_REGISTRATIONS 5u
#define FSM_IMPLEMENTATIONS 1u
#define FSM_OWNER_OFFSET 0x14u
#define FSM_INITIALIZED (FSM_OWNER_OFFSET + FSM_IMPLEMENTATIONS * 4u)
#define FSM_BYTES (FSM_INITIALIZED + 4u)

static void *registration(void *nan, void *owner)
{
    void *fsm = PTR(nan, 0x48);
    if (!owner || PTR(owner, 0) != nan || !fsm || !PTR(fsm, 4) ||
        U8(fsm, 8) != FSM_REGISTRATIONS)
        return 0;
    unsigned handle = U32(owner, 4), index = handle & ~0x8000u;
    if (!(handle & 0x8000u) || index >= FSM_REGISTRATIONS)
        return 0;
    void *entry = (uint8_t *)PTR(fsm, 4) + index * 0x18u;
    return U32(entry, 0) == handle && U32(entry, 4) == 0x3fe0u &&
        U32(entry, 8) == 0x27a89u && U32(entry, 12) == 0x27825u &&
        U8(entry, 16) == 1 && PTR(entry, 20) == owner ? entry : 0;
}

static int ndpe_deinitialize(void *nan, void *owner)
{
    void *entry = registration(nan, owner);
    if (!entry)
        return -30;
    int error = ((int (*)(void *, unsigned))native_0x28cf5)(nan, U32(owner, 4));
    if (error)
        return error;
    if (U32(entry, 0) || U32(entry, 4) || U32(entry, 8) ||
        U32(entry, 12) || PTR(entry, 20))
        return -16;
    /* The native deregister leaves only the unowned type/reserved bytes. */
    ((void (*)(void *, size_t))native_0x150891)(entry, 0x18);
    ((void (*)(void *))native_0x71491)(owner);
    return 0;
}

struct fsm_implementation {
    uint32_t type;
    void *(*initialize)(void *);
    int (*deinitialize)(void *, void *);
    uint32_t reserved;
};
_Static_assert(sizeof(struct fsm_implementation) == 16, "A2 implementation stride");

/* NRE ranging is not advertised by this backend. NDPE is the data-path
 * exchange implementation. Native lookup and idle-exit handlers are kept
 * in the actual registration table; commands and RX routing remain owned
 * by the host transport. No second protocol owner is enabled here.
 */
static const struct fsm_implementation implementations[FSM_IMPLEMENTATIONS] = {
    {1, (void *(*)(void *))0x28751u, ndpe_deinitialize, 0},
};

unsigned nan_fsm_implementation_count(void)
{
    return FSM_IMPLEMENTATIONS;
}

const struct fsm_implementation *nan_fsm_implementation(unsigned index)
{
    return index < FSM_IMPLEMENTATIONS ? &implementations[index] : 0;
}

int nan_fsm_context_attach(void *nan)
{
    if (!nan || !PTR(nan, 8))
        return -2;
    if (PTR(nan, 0x48))
        return -16;
    void *fsm = ((void *(*)(size_t))native_0x7146d)(FSM_BYTES);
    if (!fsm)
        return -27;
    PTR(fsm, 4) = ((void *(*)(size_t))native_0x7146d)(FSM_REGISTRATIONS * 0x18u);
    if (!PTR(fsm, 4)) {
        ((void (*)(void *))native_0x71491)(fsm);
        return -27;
    }
    U8(fsm, 8) = FSM_REGISTRATIONS;
    PTR(nan, 0x48) = fsm;
    return 0;
}

int nan_fsm_check_idle(void *nan)
{
    if (!nan || !PTR(nan, 0x48))
        return -25;
    void *fsm = PTR(nan, 0x48);
    if (!PTR(fsm, 4) || U8(fsm, 8) != FSM_REGISTRATIONS)
        return -30;
    if (U8(fsm, 9) || PTR(fsm, 0x0c) || PTR(fsm, 0x10))
        return -16; /* Deferred PCB and timer ownership must actually end. */
    return nan_peer_check_idle(nan);
}

static int inactive(void *wlc, void *nan)
{
    if (!wlc || !nan || PTR(nan, 8) != wlc || !PTR(nan, 0x14))
        return -2;
    void *mac = PTR(nan, 0x14);
    if (PTR(mac, 0x38) && U8(PTR(mac, 0x38), 0x4a))
        return -16;
    return nan_fsm_check_idle(nan);
}

int nan_fsm_deinitialize(void *wlc, void *nan)
{
    int error = inactive(wlc, nan);
    if (error)
        return error;
    void *fsm = PTR(nan, 0x48);
    for (unsigned i = 0; i < FSM_IMPLEMENTATIONS; ++i) {
        void *owner = PTR(fsm, FSM_OWNER_OFFSET + i * 4u);
        if (!owner)
            continue;
        error = implementations[i].deinitialize(nan, owner);
        if (error)
            return error;
        PTR(fsm, FSM_OWNER_OFFSET + i * 4u) = 0;
    }
    for (unsigned i = 0; i < FSM_REGISTRATIONS * 0x18u; i += 4)
        if (U32(PTR(fsm, 4), i))
            return -16;
    U32(fsm, FSM_INITIALIZED) = 0;
    return 0;
}

int nan_fsm_initialize(void *wlc, void *nan)
{
    int error = inactive(wlc, nan);
    if (error)
        return error;
    void *fsm = PTR(nan, 0x48);
    if (U32(fsm, FSM_INITIALIZED))
        return -16;
    for (unsigned i = 0; i < FSM_IMPLEMENTATIONS; ++i)
        if (PTR(fsm, FSM_OWNER_OFFSET + i * 4u))
            return -16;
    for (unsigned i = 0; i < FSM_REGISTRATIONS * 0x18u; i += 4)
        if (U32(PTR(fsm, 4), i))
            return -16;
    for (unsigned i = 0; i < FSM_IMPLEMENTATIONS; ++i) {
        void *owner = implementations[i].initialize(nan);
        PTR(fsm, FSM_OWNER_OFFSET + i * 4u) = owner;
        error = !owner ? -27 : !registration(nan, owner) ? -30 : 0;
        if (error) {
            int cleanup = nan_fsm_deinitialize(wlc, nan);
            return cleanup ? cleanup : error;
        }
    }
    U32(fsm, FSM_INITIALIZED) = 1;
    return 0;
}

struct ndpe_handler {
    uint8_t state, event;
    uint16_t reserved;
    uintptr_t function;
};
_Static_assert(sizeof(struct ndpe_handler) == 8, "A2 FSM dispatch stride");

/* The sentinel is tested before the function pointer by the native engine.
 * A2 handlers which reject out-of-sequence requests remain real ROM
 * handlers; do not replace those errors with successful transitions.
 */
#define HANDLE(state_, event_, address_) \
    {state_, event_, 0, (address_) | 1u}
#define END_HANDLERS {0x7f, 0xff, 0, 0}

static const struct ndpe_handler initiator[] = {
    HANDLE(0, 1, 0x27174), /* host NDP request */
    HANDLE(0, 3, 0x27744), /* host NDP termination */
    HANDLE(0, 8, 0x27848), /* peer NDP termination */
    HANDLE(0, 15, 0x27780), /* host schedule update */
    HANDLE(1, 5, 0x27360), /* peer NDP response */
    HANDLE(1, 8, 0x27848),
    HANDLE(1, 0x82, 0x279ac), /* real packet completion */
    HANDLE(1, 0x83, 0x2789c), /* timeout */
    HANDLE(1, 3, 0x27744),
    HANDLE(1, 10, 0x27518), /* peer schedule response */
    HANDLE(1, 9, 0x274d4), /* peer schedule request */
    HANDLE(2, 14, 0x2712c), /* host NDP confirmation */
    HANDLE(2, 0x82, 0x279ac),
    HANDLE(2, 8, 0x27848),
    HANDLE(2, 0x83, 0x2789c),
    HANDLE(2, 3, 0x27744),
    HANDLE(3, 7, 0x27604), /* peer security installation */
    HANDLE(3, 3, 0x27744),
    HANDLE(3, 8, 0x27848),
    HANDLE(3, 0x83, 0x2789c),
    HANDLE(5, 0x82, 0x27984), /* termination completion */
    HANDLE(6, 0x82, 0x27a58), /* schedule-update completion */
    HANDLE(5, 8, 0x27848),
    END_HANDLERS,
};

static const struct ndpe_handler responder[] = {
    HANDLE(0, 4, 0x2807c), /* peer NDP request */
    HANDLE(0, 3, 0x27744),
    HANDLE(0, 8, 0x27848),
    HANDLE(0, 15, 0x27780),
    HANDLE(0, 9, 0x282bc), /* peer schedule request */
    HANDLE(1, 2, 0x27f0c), /* host NDP response */
    HANDLE(1, 8, 0x27848),
    HANDLE(1, 0x82, 0x279ac),
    HANDLE(1, 0x83, 0x2789c),
    HANDLE(1, 3, 0x27744),
    HANDLE(1, 4, 0x28250), /* request while waiting for host response */
    HANDLE(2, 4, 0x28074), /* request while waiting for confirmation */
    HANDLE(2, 6, 0x27f54), /* peer NDP confirmation */
    HANDLE(2, 8, 0x27848),
    HANDLE(2, 11, 0x28260), /* peer schedule confirmation */
    HANDLE(2, 0x82, 0x279ac),
    HANDLE(2, 0x83, 0x2789c),
    HANDLE(2, 3, 0x27744),
    HANDLE(2, 14, 0x27ec4), /* host NDP confirmation */
    HANDLE(3, 4, 0x28258), /* request while waiting for security */
    HANDLE(3, 6, 0x2806c), /* confirmation while waiting for security */
    HANDLE(3, 8, 0x27848),
    HANDLE(3, 0x83, 0x2789c),
    HANDLE(3, 0x82, 0x279ac),
    HANDLE(3, 3, 0x27744),
    HANDLE(5, 8, 0x27848),
    HANDLE(5, 0x82, 0x27984),
    HANDLE(6, 0x82, 0x27a58),
    END_HANDLERS,
};

const struct ndpe_handler *nan_ndpe_fsm_handlers(unsigned role)
{
    /* Native 0x26b26 passes the already-selected role from its descriptor. */
    return role == 1 ? initiator : responder;
}

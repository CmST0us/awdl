/* A2 availability storage and initialization. ROM 0x316e0 owns the map,
 * channel/opclass lists and attribute builders. Keep those implementations;
 * repair unregister ownership before allowing their backing memory to go.
 */
#include <stddef.h>
#include <stdint.h>
#include "nan_mac.h"
#include "nan_availability.h"

#define FIELD(type, base, offset) (*(type *)((uint8_t *)(base) + (offset)))
#define PTR(base, offset) FIELD(void *, base, offset)
#define U32(base, offset) FIELD(uint32_t, base, offset)
#define U16(base, offset) FIELD(uint16_t, base, offset)
#define U8(base, offset) FIELD(uint8_t, base, offset)

/* A2 native fields extend through +0x34; +0x38 is private init ownership. */
#define INITIALIZED 0x38

static void release_slot(void *owner, size_t offset)
{
    if (PTR(owner, offset)) {
        ((void (*)(void *))0x71491u)(PTR(owner, offset));
        PTR(owner, offset) = 0;
    }
}

int nan_availability_context_attach(void *nan)
{
    if (!nan || !PTR(nan, 0x6c))
        return -2;
    if (PTR(nan, 0x4c))
        return -16;
    void *cfg = PTR(nan, 0x6c);
    if (!U8(cfg, 3) || !U8(cfg, 4) || !U8(cfg, 0x0d) || !U8(cfg, 0x0e))
        return -2;
    void *availability = ((void *(*)(size_t))0x7146du)(0x3c);
    if (!availability)
        return -27;
    U32(availability, 0) = 0x5afec0de;
    PTR(availability, 4) = nan;
    /* 0x303ac uses the native fixed 20-byte map-entry stride; 0x308a0
     * advances peer entries by the stride at +0x12 before native 0x32f10. */
    U16(availability, 0x10) = 20;
    U16(availability, 0x12) = 16;
    U8(availability, 0x14) = U8(cfg, 3);
    U8(availability, 0x15) = U8(cfg, 4);
    /* Internal native defaults retained from the symbolized reference.
     * Their remaining A2 consumers still need full scheduling validation. */
    U8(availability, 0x26) = 8;
    U8(availability, 0x27) = 4;
    U16(availability, 0x28) = 24;
    U8(availability, 0x34) = 2;
    PTR(nan, 0x4c) = availability;
    return 0;
}

int nan_availability_deinitialize(void *wlc, void *nan)
{
    (void)wlc;
    if (!nan)
        return -2;
    void *availability = PTR(nan, 0x4c);
    if (!availability)
        return 0;
    void *mac = PTR(nan, 0x14);
    if (mac && (PTR(mac, NAN_MAC_BSS_MANAGER) ||
                (PTR(mac, 0x38) && U8(PTR(mac, 0x38), 0x4a))))
        return -16;
    if (PTR(availability, 0x0c)) {
        if (!PTR(nan, 0x5c))
            return -30; /* Native peer cleanup updates the DAM bitmap. */
        for (size_t i = 0; i < U8(availability, 0x15); ++i)
            if (U8(PTR(availability, 0x0c), i * 16 + 0x0c))
                return -16; /* Native 0x32f10 refuses referenced entries. */
    }
    int error = 0;
    for (size_t offset = 0x18; offset <= 0x20; offset += 4) {
        void *handle = PTR(availability, offset);
        if (!handle)
            continue;
        int result = ((int (*)(void *, void *))0x1ffc9u)(nan, handle);
        if (result)
            error = result;
        else
            PTR(availability, offset) = 0;
    }
    if (error)
        return error;
    if (PTR(availability, 8)) {
        ((void (*)(void *, void *))0x30a55u)(nan, PTR(availability, 8));
        PTR(availability, 8) = 0;
    }
    if (PTR(availability, 0x0c)) {
        for (size_t i = 0; i < U8(availability, 0x15); ++i) {
            int result = ((int (*)(void *, void *))0x32f11u)(
                nan, (uint8_t *)PTR(availability, 0x0c) + i * 16);
            if (result)
                return result;
        }
        release_slot(availability, 0x0c);
    }
    release_slot(availability, 0x2c);
    release_slot(availability, 0x30);
    U32(availability, INITIALIZED) = 0;
    return 0;
}

int nan_availability_initialize(void *wlc, void *nan)
{
    if (!nan || !PTR(nan, 0x4c) || !PTR(nan, 0x5c))
        return -2;
    void *availability = PTR(nan, 0x4c);
    if (U32(availability, INITIALIZED) || PTR(availability, 8) ||
        PTR(availability, 0x0c) || PTR(availability, 0x2c) ||
        PTR(availability, 0x30) || PTR(availability, 0x18) ||
        PTR(availability, 0x1c) || PTR(availability, 0x20))
        return -16;
    int error = ((int (*)(void *, void *))0x316e1u)(wlc, nan);
    if (!error)
        U32(availability, INITIALIZED) = 1;
    else {
        int cleanup = nan_availability_deinitialize(wlc, nan);
        if (cleanup)
            return cleanup;
    }
    return error;
}

int nan_availability_context_detach(void *nan)
{
    if (!nan)
        return -2;
    int error = nan_availability_deinitialize(PTR(nan, 8), nan);
    if (!error)
        release_slot(nan, 0x4c);
    return error;
}

/* A2 discovery attributes retain the original ROM encoders and Bloom hashes.
 * B0 added Instant Mode parse callbacks and ecounters after the A2 fields;
 * neither is required by the hostap-owned discovery path. */
#include <stddef.h>
#include <stdint.h>
#include "nan_discovery.h"
#include "nan_mac.h"
#include "nan_peer.h"

#define FIELD(type, base, offset) (*(type *)((uint8_t *)(base) + (offset)))
#define PTR(base, offset) FIELD(void *, base, offset)
#define U32(base, offset) FIELD(uint32_t, base, offset)
#define U16(base, offset) FIELD(uint16_t, base, offset)
#define U8(base, offset) FIELD(uint8_t, base, offset)
#define INITIALIZED 0x68

typedef void *(*bloom_alloc)(void *, size_t);
typedef void (*bloom_free)(void *, void *, size_t);

/* Only ROM 0x25d3c is redirected here. The original Bloom constructor zeros
 * the hash array before checking allocation failure (0x9816 -> 0x981e).
 * Preserve its 20-byte ABI and native destructor, checking before every write.
 */
int nan_discovery_bloom_create(bloom_alloc allocate, bloom_free release,
                               void *context, unsigned hashes, unsigned bytes,
                               void **result)
{
    if (!allocate || !release || !result || !hashes || hashes > UINT32_MAX / 4)
        return -2;
    if (*result)
        return -16;
    void *bloom = allocate(context, 20);
    if (!bloom)
        return -27;
    ((void (*)(void *, size_t))0x18660bu)(bloom, 20);
    PTR(bloom, 0) = context;
    U32(bloom, 4) = hashes;
    PTR(bloom, 8) = allocate(context, hashes * 4);
    if (!PTR(bloom, 8))
        goto nomem;
    ((void (*)(void *, size_t))0x18660bu)(PTR(bloom, 8), hashes * 4);
    if (bytes) {
        PTR(bloom, 0x10) = allocate(context, bytes);
        if (!PTR(bloom, 0x10))
            goto nomem;
        U32(bloom, 0x0c) = bytes;
        ((void (*)(void *, size_t))0x18660bu)(PTR(bloom, 0x10), bytes);
    }
    *result = bloom;
    return 0;
nomem:
    ((void (*)(void **, bloom_free))0x9865u)(&bloom, release);
    return -27;
}

static void release_slot(void *owner, size_t offset)
{
    if (PTR(owner, offset)) {
        ((void (*)(void *))0x71491u)(PTR(owner, offset));
        PTR(owner, offset) = 0;
    }
}

static int idle(void *nan, void *discovery)
{
    void *mac = PTR(nan, 0x14);
    if (mac && PTR(mac, 0x38) && U8(PTR(mac, 0x38), 0x4a))
        return -16;
    /* Lists and counters used by ROM 0x26078, 0x249bc and 0x265c8.
     * Their protocol owners must stop them before backing storage is freed. */
    if (PTR(discovery, 8) || PTR(discovery, 0x0c) || U8(discovery, 0x10) ||
        U8(discovery, 0x50) || U8(discovery, 0x58))
        return -16;
    return nan_peer_check_idle(nan);
}

int nan_discovery_context_attach(void *nan)
{
    if (!nan || !PTR(nan, 8) || !PTR(nan, 0x10))
        return -2;
    if (PTR(nan, 0x18))
        return -16;
    /* A2 heartbeat consumers reach +0x64 (u16); the aligned native extent is
     * 0x68. The final word records this implementation's init ownership. */
    void *discovery = ((void *(*)(size_t))0x7146du)(0x6c);
    if (!discovery)
        return -27;
    U32(discovery, 0) = 0x5afec0de;
    PTR(discovery, 4) = nan;
    PTR(nan, 0x18) = discovery;
    return 0;
}

int nan_discovery_deinitialize(void *wlc, void *nan)
{
    if (!nan || PTR(nan, 8) != wlc)
        return -2;
    void *discovery = PTR(nan, 0x18);
    if (!discovery)
        return 0;
    int error = idle(nan, discovery);
    if (error)
        return error;
    const uint8_t attributes[] = {0x30, 0x60, 0x4c, 0x54};
    for (size_t i = 0; i < sizeof(attributes); ++i) {
        void *handle = PTR(discovery, attributes[i]);
        if (!handle)
            continue;
        int result = ((int (*)(void *, void *))0x1ffc9u)(nan, handle);
        if (result)
            error = result;
        else
            PTR(discovery, attributes[i]) = 0;
    }
    if (error)
        return error;
    for (size_t offset = 0x34; offset < 0x44; offset += 4)
        ((void (*)(void **, bloom_free))0x9865u)(
            &PTR(discovery, offset), (bloom_free)0x22e15u);
    release_slot(discovery, 0x5c);
    release_slot(discovery, 0x48);
    U32(discovery, INITIALIZED) = 0;
    return 0;
}

int nan_discovery_initialize(void *wlc, void *nan)
{
    if (!nan || PTR(nan, 8) != wlc || !PTR(nan, 0x10) || !PTR(nan, 0x18))
        return -2;
    void *discovery = PTR(nan, 0x18);
    /* A2 has no located Instant Mode SID parse implementation. Do not install
     * the B0 RAM callback or advertise that firmware-owned response policy. */
    if (U32(PTR(nan, 0x10), 0x44) & 0x00100000u)
        return -23;
    if (U32(discovery, INITIALIZED))
        return -16;
    int error = idle(nan, discovery);
    if (error)
        return error;
    const struct {
        uint8_t id, mask, offset;
        uintptr_t length, build;
    } attributes[] = {
        {2, 3, 0x30, 0x250d9u, 0x25045u},
        {0x28, 3, 0x60, 0x250d9u, 0x25045u},
        {0x0e, 4, 0x4c, 0x24c1du, 0x24ab1u},
        {3, 4, 0x54, 0x24a45u, 0x249bdu},
    };
    for (size_t i = 0; i < sizeof(attributes) / sizeof(attributes[0]); ++i)
        if (PTR(discovery, attributes[i].offset))
            return -16;
    for (size_t offset = 0x34; offset < 0x44; offset += 4)
        if (PTR(discovery, offset))
            return -16;
    if (PTR(discovery, 0x48) || PTR(discovery, 0x5c))
        return -16;
    error = -27;
    for (size_t i = 0; i < sizeof(attributes) / sizeof(attributes[0]); ++i) {
        void *handle = ((void *(*)(void *, unsigned, unsigned, unsigned,
                                   uintptr_t, uintptr_t, void *, unsigned))0x1ff41u)(
            nan, attributes[i].id, attributes[i].mask, attributes[i].id,
            attributes[i].length, attributes[i].build, discovery, 0);
        PTR(discovery, attributes[i].offset) = handle;
        if (!handle)
            goto failed;
    }
    error = ((int (*)(void *))0x25ccdu)(discovery);
    if (error)
        goto failed;
    error = -27;
    PTR(discovery, 0x48) = ((void *(*)(size_t))0x7146du)(0x2b);
    if (!PTR(discovery, 0x48))
        goto failed;
    PTR(discovery, 0x5c) = ((void *(*)(size_t))0x7146du)(0x20);
    if (!PTR(discovery, 0x5c))
        goto failed;
    /* Native reference defaults: service DW awake and unicast SDF permitted
     * during another peer's negotiation; heartbeat is 10 native DW ticks. */
    U32(PTR(nan, 0x10), 0x34) |= 0x400u;
    U32(PTR(nan, 0x10), 0x44) |= 1u;
    U16(discovery, 0x64) = 10;
    U32(discovery, INITIALIZED) = 1;
    return 0;
failed: {
    int cleanup = nan_discovery_deinitialize(wlc, nan);
    return cleanup ? cleanup : error;
}
}

int nan_discovery_context_detach(void *nan)
{
    if (!nan)
        return -2;
    int error = nan_discovery_deinitialize(PTR(nan, 8), nan);
    if (!error)
        release_slot(nan, 0x18);
    return error;
}

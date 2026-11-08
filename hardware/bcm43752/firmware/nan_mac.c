/* BCM43752A2 .146 only. All native entry points and field offsets below are
 * derived from the hash-locked A2 ROM, not the reference B0 structure layout.
 * This restores MAC storage/commands, beacon IE registries and pre-enable
 * initialization, including WLC template/pre-TBTT notifications. BSSCFG and
 * RX/TX attachment remain.
 */
#include "nan_mac.h"
#include "nan_commands.h"

#define FIELD(type, base, offset) (*(type *)((uint8_t *)(base) + (offset)))
#define PTR(base, offset) FIELD(void *, base, offset)
#define U32(base, offset) FIELD(uint32_t, base, offset)
#define U16(base, offset) FIELD(uint16_t, base, offset)
#define U8(base, offset) FIELD(uint8_t, base, offset)

static void *allocate(size_t size)
{
    return ((void *(*)(size_t))0x7146du)(size);
}

static void release_slot(void *owner, size_t offset)
{
    void *resource = PTR(owner, offset);
    if (resource) {
        ((void (*)(void *))0x71491u)(resource);
        PTR(owner, offset) = 0;
    }
}

#define HANDLER(address) ((nan_command_handler)((address) | 1u))
#define MAC_COMMAND(id_, get_, set_, smin_, smax_, gmin_, gmax_) \
    { .id = (id_), .validate = nan_command_validate, \
      .get = (get_), .set = (set_), .min_set = (smin_), .max_set = (smax_), \
      .min_get = (gmin_), .max_get = (gmax_) }

/* A2 0x29d84 clears the requested high-band channel when NAN is disabled.
 * Host-owned initialization must configure both DWs before creating the
 * NMI/MSCH owners. Validate the real PHY/CLM first, then use the same native
 * capability-list and DAM refresh as the original command. Once an NMI
 * exists, channel changes need its real stop/start lifecycle; never rewrite
 * channels underneath an existing BSS registration. */
static int social_channels(const struct nan_command_digest *digest,
                            const uint8_t *input, size_t length,
                            uint8_t *output, size_t *output_length)
{
    (void)output;
    *output_length = 0;
    if (length != 4 || input[0] != 6 || input[2] || input[3] ||
        (input[1] && input[1] != 44 && input[1] != 149))
        return -2;
    void *nan = ((void *const *)digest)[1];
    void *mac = PTR(nan, 0x14);
    void *config = PTR(mac, 0x20);
    void *wlc = PTR(nan, 8);
    uint16_t low = 0x1006;
    uint16_t high = input[1] ? 0xd000u | input[1] : 0;
    if (PTR(mac, NAN_MAC_BSS_MANAGER) || U8(PTR(mac, 0x38), 0x4a))
        return U16(config, 0x52) == low && U16(config, 0x54) == high &&
               U8(config, 0x50) == !!high ? 0 : -16;
    if (!PTR(config, 0x74) || !PTR(nan, 0x5c))
        return -25;
    if (high && ((unsigned (*)(void *))0x8bba1u)(wlc) < 2)
        return -3;
    int (*valid_channel)(void *, unsigned) = (int (*)(void *, unsigned))0xc815du;
    if (!valid_channel(PTR(nan, 0x2c), low) ||
        (high && !valid_channel(PTR(nan, 0x2c), high)))
        return -20;
    U16(config, 0x52) = low;
    U16(config, 0x54) = high;
    U8(config, 0x50) = !!high;
    int error = ((int (*)(void *))0x2f625u)(mac);
    if (error)
        return error;
    /* 0x31898 is the fixed A2 single-radio policy query (returns zero),
     * not the configured high-band DW flag. Preserve that native policy. */
    ((void (*)(void *, unsigned))0x2203du)(PTR(nan, 0x5c), 0);
    return 0;
}

static const struct nan_command mac_commands[] = {
    MAC_COMMAND(0x0201, HANDLER(0x2914c), HANDLER(0x2916c), 1, 1, 0, 0), /* host election */
    MAC_COMMAND(0x0202, HANDLER(0x29220), HANDLER(0x291f0), 4, 4, 0, 0), /* metrics config */
    MAC_COMMAND(0x0203, HANDLER(0x29248), 0, 0, 0, 0, 0), /* metrics state */
    MAC_COMMAND(0x0206, HANDLER(0x29274), HANDLER(0x292a8), 4, 4, 4, 4), /* RSSI */
    MAC_COMMAND(0x0401, HANDLER(0x29cc8), social_channels, 4, 4, 4, 4), /* social channels */
    MAC_COMMAND(0x0402, HANDLER(0x299b4), HANDLER(0x299e8), 4, 4, 4, 4), /* awake DWs */
    MAC_COMMAND(0x0403, HANDLER(0x29b38), HANDLER(0x29b60), 4, 4, 4, 4), /* beacon RSSI */
    MAC_COMMAND(0x011a, HANDLER(0x29cf4), HANDLER(0x29d30), 6, 6, 12, 12), /* scan */
    MAC_COMMAND(0x0119, HANDLER(0x29b8c), HANDLER(0x29bf0), 12, 12, 1, 1), /* device capability */
    MAC_COMMAND(0x0204, HANDLER(0x291a4), HANDLER(0x291d0), 1, 1, 0, 0), /* merge */
};

/* ROM 0x1feec is a void template-refresh notification. Configuration SETs
 * already update the real capability list before calling it, including when
 * NAN has not created its BSSCFG. Building a packet then dereferences NULL.
 * Keep those pre-enable values; native enable 0x2aa9c -> 0x2c51c builds both
 * templates from the current configuration after BSSCFG creation. Once the
 * BSSCFG exists, retain the original mask validation and template builder.
 */
void nan_mac_refresh_beacons(void *nan, uint32_t mask)
{
    void *mac = PTR(nan, 0x14);
    if (!(mask & 7u) || (mask & ~7u) || !mac || !PTR(mac, 8) ||
        !PTR(PTR(mac, 8), 0) || !PTR(PTR(PTR(mac, 8), 0), 0))
        return;
    void (*build)(void *, uint32_t) = (void (*)(void *, uint32_t))0x2e57du;
    if (mask & 1u)
        build(mac, 1);
    if (mask & 2u)
        build(mac, 2);
}

int nan_mac_merge_candidate(void *mac, void *bss)
{
    void *config = PTR(mac, 0x20);
    void *candidate = PTR(mac, 0x34);
    /* A2's 0x0204 command stores this flag, but 0x2b5ac ignores it. Both
     * DW-end and scan-end adoption enter here. Leave received proposals
     * owned by the native MAC while disabled; only the native adoption
     * routine may update BSSID, rank, templates and timing on re-enable.
     * Preserve the native host-election exclusion and success boolean. */
    if (!U8(config, 0x28) || U8(config, 0x5c) || !U8(candidate, 0x30))
        return 0;
    return ((int (*)(void *, void *, void *))0x2aa0du)(mac, candidate, bss) == 0;
}

static void configure(void *mac)
{
    void *cfg = PTR(mac, 0x20);
    U32(cfg, 0) = 0x5afec0de;
    U32(cfg, 4) = 0x004e4d49; /* NMI configuration tag */
    U16(cfg, 0x08) = 200; /* Discovery beacon interval, TU. */
    U16(cfg, 0x0a) = 512; /* DW period, TU; native division requires nonzero. */
    /* A2 0x2e08c uses this as the DW reservation duration as well as the
     * management TX lifetime. Reserving the full 512-TU period starves the
     * second discovery band and the negotiated data schedule. */
    U16(cfg, 0x0c) = 16; /* Discovery window duration, TU. */
    U32(cfg, 0x14) = 0x1000c; /* 20 MHz, legacy 6 Mbit/s management ratespec. */
    U8(cfg, 0x1f) = 120; /* Master preference, 1..254. Automatic election. */
    U16(cfg, 0x22) = 200; /* Random-factor refresh interval, DW count. */
    U16(cfg, 0x24) = 5; /* Warmup, seconds: ROM uses value * 1000000 / DW_us. */
    U16(cfg, 0x26) = 5; /* Maximum propagated hop count. */
    U8(cfg, 0x28) = 1; /* Permit native cluster merging. */
    const uint8_t cluster[6] = {0x50, 0x6f, 0x9a, 0x01, 0, 0};
    for (size_t i = 0; i < sizeof(cluster); ++i) {
        U8(cfg, 0x2e + i) = cluster[i];
        U8(cfg, 0x34 + i) = U8(PTR(mac, 0x14), 8 + i);
    }
    /* Keep the board address as the stable identity, with a locally
     * administered unicast NMI. A later NMI command uses the native setter. */
    U8(cfg, 0x34) = (U8(cfg, 0x34) | 2u) & ~1u;
    U16(cfg, 0x40) = 1024; /* Discovery beacon allocation capacity, bytes. */
    U16(cfg, 0x42) = 1024; /* Synchronization beacon allocation capacity, bytes. */
    U8(cfg, 0x50) = 1; /* A2 native init validates both channels with the CLM. */
    U16(cfg, 0x52) = 0x1006; /* 2.4 GHz channel 6, 20 MHz. */
    U16(cfg, 0x54) = 0xd095; /* 5 GHz channel 149, 20 MHz. */
    U8(cfg, 0x58) = (uint8_t)-60;
    U8(cfg, 0x59) = (uint8_t)-75;
    U8(cfg, 0x5a) = (uint8_t)-60;
    U8(cfg, 0x5b) = (uint8_t)-75; /* Native RSSI thresholds, dBm. */
    U8(cfg, 0x68) = (uint8_t)-75;
    U8(cfg, 0x69) = (uint8_t)-75; /* Beacon RSSI notification thresholds, dBm. */
    U8(cfg, 0x79) = 8; /* RSSI rolling window; native storage has eight samples. */
    for (size_t offset = 0x6c; offset <= 0x70; offset += 4)
        U16(PTR(cfg, offset), 0) = 128; /* Native scan dwell; zero period selects native default. */
    ((void (*)(void *))0x2a925u)(mac); /* Native runtime reset: 0x68 bytes. */
}

static int attach_beacon_registries(void *wlc, void *mac)
{
    void *(*create)(void *, uint16_t, uint16_t) =
        (void *(*)(void *, uint16_t, uint16_t))0x22f1fdu;
    int (*add)(void *, uint8_t, uintptr_t, uintptr_t, void *) =
        (int (*)(void *, uint8_t, uintptr_t, uintptr_t, void *))0x22f0b1u;
    /* A2 WLC attach stores its IE manager at +0x438 (0x225070/0x225074).
     * Native registry allocation enforces the manager's actual capacity. */
    PTR(mac, 0x24) = create(PTR(wlc, 0x438), 2, 0);
    if (!PTR(mac, 0x24))
        return -27;
    int error = add(PTR(mac, 0x24), 0xc5, 0x2a8e5u, 0x2dfb1u, mac);
    if (error)
        return error;
    PTR(mac, 0x28) = create(PTR(wlc, 0x438), 1, 0);
    if (!PTR(mac, 0x28))
        return -27;
    return add(PTR(mac, 0x28), 0xc5, 0x2a91du, 0x2dfb9u, mac);
}

/* The native A2 context ends at +0x113. Our extra word records ownership of
 * callbacks held by WLC, independently of the native MAC initialized flag. */
#define NOTIFICATIONS 0x114

static int attach_notifications(void *wlc, void *mac)
{
    int (*add)(void *, uintptr_t, void *) =
        (int (*)(void *, uintptr_t, void *))0x92a1u;
    void *bss_module = PTR(wlc, 0x60);
    int error = add(PTR(bss_module, 0x18), 0x2a6f5u, mac);
    if (error)
        return error;
    U32(mac, NOTIFICATIONS) |= 1;
    error = add(PTR(bss_module, 0x20), 0x2cf01u, mac);
    if (!error)
        U32(mac, NOTIFICATIONS) |= 2;
    return error;
}

static int detach_notifications(void *mac)
{
    int (*remove)(void *, uintptr_t, void *) =
        (int (*)(void *, uintptr_t, void *))0x9369u;
    const uintptr_t callbacks[] = {0x2a6f5u, 0x2cf01u};
    void *bss_module = PTR(PTR(mac, 0x10), 0x60);
    int error = 0;
    for (size_t i = 0; i < 2; ++i) {
        uint32_t bit = 1u << i;
        if (!(U32(mac, NOTIFICATIONS) & bit))
            continue;
        int result = remove(PTR(bss_module, 0x18 + 8 * i), callbacks[i], mac);
        if (result)
            error = result;
        else
            U32(mac, NOTIFICATIONS) &= ~bit;
    }
    return error;
}

/* This only releases base storage after initialization has been unwound.
 * Native capability-list free handles every partially allocated element. */
static int release_context(void *nan)
{
    void *mac = PTR(nan, 0x14);
    int error = detach_notifications(mac);
    if (error)
        return error;
    for (size_t offset = 0x24; offset <= 0x28; offset += 4) {
        if (PTR(mac, offset)) {
            ((void (*)(void *))0x22eff9u)(PTR(mac, offset));
            PTR(mac, offset) = 0;
        }
    }
    void *cfg = PTR(mac, 0x20);
    if (cfg) {
        ((void (*)(void *, void **))0x2b2d5u)(mac, &PTR(cfg, 0x74));
        release_slot(cfg, 0x64);
        release_slot(cfg, 0x6c);
        release_slot(cfg, 0x70);
        release_slot(mac, 0x20);
    }
    void *bands = PTR(mac, 8);
    if (bands) {
        release_slot(bands, 0);
        release_slot(bands, 4);
        release_slot(mac, 8);
    }
    release_slot(mac, 0x38);
    release_slot(nan, 0x14);
    return 0;
}

int nan_mac_context_attach(void *wlc, void *nan)
{
    if (!wlc || !nan || !PTR(nan, 0x28) || !PTR(nan, 0x6c) ||
        !U8(PTR(nan, 0x6c), 3) || !PTR(wlc, 0) || !PTR(wlc, 0x438) ||
        !PTR(wlc, 0x60) || !PTR(PTR(wlc, 0x60), 0x18) ||
        !PTR(PTR(wlc, 0x60), 0x20))
        return -2;
    if (PTR(nan, 0x14))
        return -16;
    uint8_t nonzero = 0;
    for (size_t i = 0; i < 6; ++i)
        nonzero |= U8(PTR(wlc, 0), 8 + i);
    if (!nonzero || (U8(PTR(wlc, 0), 8) & 1u))
        return -2;
    /* A2 consumers reach byte +0x110. Leave aligned tail capacity; B0's
     * 0xec allocation would overwrite its following heap object. */
    void *mac = allocate(NAN_MAC_CONTEXT_SIZE);
    if (!mac)
        return -27;
    PTR(nan, 0x14) = mac;
    U32(mac, 0) = 0x5afec0de;
    PTR(mac, 4) = nan;
    PTR(mac, 0x0c) = PTR(nan, 0x20);
    PTR(mac, 0x10) = wlc;
    PTR(mac, 0x14) = PTR(wlc, 0);
    PTR(mac, 0x18) = PTR(wlc, 4);
    U32(mac, 0xd4) = UINT32_MAX; /* No slotted-BSS table entry owned yet. */
    PTR(mac, 8) = allocate(8);
    if (!PTR(mac, 8))
        goto nomem;
    for (size_t offset = 0; offset < 8; offset += 4) {
        /* A2 band entries own BSSCFG, shared timer records and RSSI state.
         * Reserve 0x30 bytes; located A2 accesses extend through +0x1c. */
        PTR(PTR(mac, 8), offset) = allocate(0x30);
        if (!PTR(PTR(mac, 8), offset))
            goto nomem;
    }
    PTR(mac, 0x20) = allocate(0x7c); /* A2 config reaches byte +0x79. */
    if (!PTR(mac, 0x20))
        goto nomem;
    void *cfg = PTR(mac, 0x20);
    if (((int (*)(void *, void **))0x2b425u)(mac, &PTR(cfg, 0x74)))
        goto nomem;
    const uint8_t offsets[] = {0x64, 0x6c, 0x70};
    for (size_t i = 0; i < sizeof(offsets); ++i) {
        PTR(cfg, offsets[i]) = allocate(i ? 4 : 44);
        if (!PTR(cfg, offsets[i]))
            goto nomem;
    }
    PTR(mac, 0x38) = allocate(0x68);
    if (!PTR(mac, 0x38))
        goto nomem;
    configure(mac);
    int error = attach_beacon_registries(wlc, mac);
    if (!error)
        error = attach_notifications(wlc, mac);
    if (!error)
        error = nan_commands_register(nan, nan, mac_commands,
                                       sizeof(mac_commands) / sizeof(mac_commands[0]));
    if (!error)
        return 0;
    int cleanup = release_context(nan);
    return cleanup ? cleanup : error;
nomem:
    /* Notifications have not been registered on these allocation failures. */
    release_context(nan);
    return -27;
}

/* Do not clear an attribute/event handle unless native unregister found and
 * freed it. Preserve inconsistent ownership for a recoverable error. */
static int unregister_slot(void *nan, void *mac, size_t offset, uintptr_t entry)
{
    void *handle = PTR(mac, offset);
    if (!handle)
        return 0;
    int error = ((int (*)(void *, void *))entry)(nan, handle);
    if (!error)
        PTR(mac, offset) = 0;
    return error;
}

int nan_mac_deinitialize(void *wlc, void *nan)
{
    (void)wlc;
    if (!nan)
        return -2;
    void *mac = PTR(nan, 0x14);
    if (!mac)
        return 0;
    void *bands = PTR(mac, 8);
    /* BSSCFG and TX ownership belong to enable/disable, not this pre-enable
     * unwind. Never free backing storage under those live consumers. */
    if (U8(PTR(mac, 0x38), 0x4a) || PTR(mac, 0x3c) || PTR(mac, 0x40) ||
        PTR(mac, 0xe4) || PTR(mac, 0x10c) || U32(mac, 0xb4) ||
        PTR(mac, NAN_MAC_HOST_TX) || U32(mac, NAN_MAC_HOST_TX_COUNT) ||
        PTR(mac, NAN_MAC_HOST_STATIONS) || PTR(mac, NAN_MAC_HOST_GROUP_KEYS) ||
        PTR(PTR(bands, 0), 0) || PTR(PTR(bands, 4), 0) ||
        PTR(mac, NAN_MAC_BSS_MANAGER) || PTR(mac, NAN_MAC_BSS_NODE) ||
        PTR(mac, NAN_MAC_EVENT_BSS) || PTR(mac, NAN_MAC_EVENT_MANAGER))
        return -16;
    const uint8_t parse_slots[] = {0x2c, 0x30, 0x80};
    const uint8_t attribute_slots[] = {0x84, 0x7c};
    int error = 0;
    for (size_t i = 0; i < sizeof(parse_slots); ++i) {
        int result = unregister_slot(nan, mac, parse_slots[i], 0x2005du);
        if (result)
            error = result;
    }
    for (size_t i = 0; i < sizeof(attribute_slots); ++i) {
        int result = unregister_slot(nan, mac, attribute_slots[i], 0x1ffc9u);
        if (result)
            error = result;
    }
    if (error)
        return error;
    /* The native stats registry contains borrowed pointers. Remove this
     * module's entry before freeing its backing allocation. */
    void *stats = PTR(nan, 0x74);
    if (stats && PTR(stats, 4)) {
        if (PTR(stats, 4) != PTR(mac, 0xd8))
            return -30;
        ((void (*)(void *, size_t))0x18660bu)(stats, 12);
    }
    const uint16_t allocations[] = {0x34, 0x44, 0x48, 0x70, 0xa0, 0xc4,
                                     0xc8, 0xd0, 0xd8, 0x108};
    for (size_t i = 0; i < sizeof(allocations) / sizeof(allocations[0]); ++i)
        release_slot(mac, allocations[i]);
    ((void (*)(void *))0x2d931u)(mac);
    /* Band 1 borrows band 0's timer records; native free clears only band 0. */
    PTR(PTR(bands, 4), 4) = 0;
    PTR(PTR(bands, 4), 8) = 0;
    ((void (*)(void *))0x2a6c5u)(mac);
    ((void (*)(void *))0x2b4d1u)(mac);
    ((void (*)(void *, void *))0x2b4b1u)(mac, PTR(PTR(mac, 0x20), 0x74));
    U8(PTR(mac, 0x38), 0x66) = 0;
    U8(mac, 0x1c) = 0;
    return 0;
}

int nan_mac_initialize(void *wlc, void *nan)
{
    if (!wlc || !nan || !PTR(nan, 0x14) || !PTR(nan, 0x74))
        return -2;
    void *mac = PTR(nan, 0x14);
    if (U8(mac, 0x1c))
        return -16;
    /* Reject residual ownership before the native initializer overwrites it. */
    if (PTR(mac, 0x2c) || PTR(mac, 0x30) || PTR(mac, 0x34) ||
        PTR(mac, 0x7c) || PTR(mac, 0x80) || PTR(mac, 0x84))
        return -16;
    int error = ((int (*)(void *, void *))0x2f465u)(wlc, nan);
    if (error) {
        int cleanup = nan_mac_deinitialize(wlc, nan);
        if (cleanup)
            return cleanup;
    }
    return error;
}

int nan_mac_context_detach(void *nan)
{
    if (!nan)
        return -2;
    if (!PTR(nan, 0x14))
        return 0;
    if (PTR(nan, 0x28))
        return -16;
    int error = nan_mac_deinitialize(PTR(nan, 8), nan);
    if (!error)
        error = release_context(nan);
    return error;
}

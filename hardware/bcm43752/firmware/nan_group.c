#include "native_calls.h"
/* A2 group-key ownership. Native key objects/ciphers remain authoritative;
 * private selectors extend the missing peer GTK and BIGTK ID namespaces. */
#include "nan_group.h"
#include "nan_station.h"
#include "nan_host.h"
#include "nan_mac.h"
#include "nan_host_abi.h"
#define FIELD(t, p, o) (*(t *)((uint8_t *)(p) + (o)))
#define PTR(p, o) FIELD(void *, p, o)
#define U32(p, o) FIELD(uint32_t, p, o)
#define U16(p, o) FIELD(uint16_t, p, o)
#define U8(p, o) FIELD(uint8_t, p, o)

struct group_key {
    struct group_key *next;
    void *bss, *scb, *key;
    uint8_t local[6], peer[6];
    uint16_t index;
    uint8_t id, tx, ready, algo, bytes, draining;
    unsigned beacon_refs; /* Released only by the packet's native PCB. */
};
static unsigned get16(const uint8_t *p)
{
    return p[0] | (unsigned)p[1] << 8;
}
static int zero(const uint8_t *p, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
        if (p[i])
            return 0;
    return 1;
}
static int equal(const void *a, const void *b, unsigned n)
{
    return !((int (*)(const void *, const void *, unsigned))native_0xa7a9)(a, b, n);
}
static void copy(void *a, const void *b, unsigned n)
{
    ((void *(*)(void *, const void *, unsigned))native_0x151a21)(a, b, n);
}
static struct group_key **head(void *nan)
{
    return (struct group_key **)((uint8_t *)PTR(nan, 0x14) + NAN_MAC_HOST_GROUP_KEYS);
}
static void *context(void *km, void *bss)
{
    if (!km || !bss || U32(bss, 0x74) != 5 || (U32(bss, 0x78) != 10 && U32(bss, 0x78) != 11))
        return 0;
    void *wlc = PTR(km, 4), *wrapper = PTR(wlc, 0x520);
    void *nan = wrapper ? PTR(wrapper, 4) : 0;
    return nan && PTR(nan, 0x14) && PTR(bss, 0) == wlc ? nan : 0;
}
static void *lookup_bss(void *nan, const uint8_t *local, unsigned id)
{
    void *bss = id < 4 && PTR(nan, 0x30)
                    ? ((void *(*)(void *, const void *))native_0x22a1d)(PTR(nan, 0x30), local)
                    : nan_host_nmi(nan);
    return bss && equal((uint8_t *)bss + 0x66, local, 6) ? bss : 0;
}
static struct group_key **find(void *nan, void *bss, void *scb, unsigned id)
{
    struct group_key **p = head(nan);
    while (*p && ((*p)->bss != bss || (*p)->scb != scb || (*p)->id != id))
        p = &(*p)->next;
    return p;
}
static int owned(void *km, struct group_key *g)
{
    if (g->index >= U16(km, 0x0e))
        return 0;
    void *record = (uint8_t *)PTR(km, 0x10) + g->index * 16;
    if ((U16(record, 4) & 0x4000) == 0 || PTR(record, 0) != g->key)
        return 0;
    return !g->key || ((U16(record, 4) & 1) && PTR(record, 8) == (g->scb ? g->scb : g->bss) &&
                       U16(g->key, 4) == g->index && U8(g->key, 17) == g->id &&
                       equal((uint8_t *)g->key + 6, g->peer, 6));
}
static int usable(void *km, struct group_key *g)
{
    if (!g->ready || !g->key || !owned(km, g) || !PTR(g->key, 0x24))
        return 0;
    unsigned direction = g->tx ? 2 : 4;
    return (U32(g->key, 12) & 6) == direction && U8(g->key, 16) == g->algo &&
           U8(g->key, 19) == g->bytes && U8(g->key, 20) == 8 &&
           U8(g->key, 21) == (g->algo == 4 || g->algo == 13 ? 8 : 16);
}
static void *result(void *km, struct group_key *g, void *info)
{
    void *key = g && usable(km, g) ? g->key : PTR(km, 0x24);
    ((void (*)(void *, void *))native_0x151e71)(key, info);
    return key;
}
/* Checked RAM-entry patches also cover direct calls which bypass the ROM
 * trampolines. These continuations reproduce only the overwritten prologue. */
NATIVE_ENTRY static void *original_bss(void *km __attribute__((unused)),
                                                 void *bss __attribute__((unused)),
                                                 unsigned id __attribute__((unused)),
                                                 void *info __attribute__((unused)))
{
    __asm__ volatile("push {r3-r7,lr}\ncbnz r0,1f\nldr pc,=0x1876d7\n1: cbnz r1,2f\nldr "
                     "pc,=0x1876d7\n2: ldr r5,[r0,#0x14]\nldr pc,=0x187691");
}
NATIVE_ENTRY static void *original_scb(void *km __attribute__((unused)),
                                                 void *scb __attribute__((unused)),
                                                 unsigned id __attribute__((unused)),
                                                 unsigned flags __attribute__((unused)),
                                                 void *info __attribute__((unused)))
{
    __asm__ volatile("push {r4-r11,lr}\nldr pc,=0x188fd1");
}
NATIVE_ENTRY static unsigned original_tx(void *km __attribute__((unused)),
                                                   void *bss __attribute__((unused)),
                                                   unsigned management __attribute__((unused)))
{
    __asm__ volatile("ldr r3,[r1,#0x48]\npush {r4,lr}\nldr pc,=0x1876f7");
}
void *nan_group_bss_key(void *km, void *bss, unsigned id, void *info)
{
    void *nan = context(km, bss);
    return nan ? result(km, *find(nan, bss, 0, id), info) : original_bss(km, bss, id, info);
}
void *nan_group_scb_key(void *km, void *scb, unsigned id, unsigned flags, void *info)
{
    void *bss = PTR(scb, 12), *nan = context(km, bss);
    if (nan && (flags & 0x120))
        return result(km, *find(nan, bss, scb, id), info);
    return original_scb(km, scb, id, flags, info);
}
unsigned nan_group_tx_id(void *km, void *bss, unsigned management)
{
    void *nan = context(km, bss);
    if (!nan)
        return original_tx(km, bss, management);
    for (struct group_key *g = *head(nan); g; g = g->next)
        if (g->bss == bss && g->tx && (management ? g->id >= 4 && g->id < 6 : g->id < 4) &&
            usable(km, g))
            return g->id;
    return 0xff;
}
static int syntax(const uint8_t *p, size_t n)
{
    if (n != BCM_NAN_HOST_GROUP_KEY_SIZE || get16(p) != BCM_NAN_HOST_VERSION || get16(p + 4) != n ||
        get16(p + 2) < 1 || get16(p + 2) > 3 || p[14] > 7 || p[22] || p[23] || p[31] ||
        (p[24] & 1) || zero(p + 24, 6) || (p[30] != 1 && p[30] != 2))
        return 0;
    if (p[30] == 1 ? !zero(p + 8, 6) : ((p[8] & 1) || zero(p + 8, 6) || equal(p + 8, p + 24, 6)))
        return 0;
    if (get16(p + 2) != 1)
        return !get16(p + 6) && !p[15] && zero(p + 16, 6) && zero(p + 32, 32);
    unsigned bytes = get16(p + 6);
    return p[14] < 4
               ? ((p[15] == 4 && bytes == 16 && zero(p + 48, 16)) || (p[15] == 16 && bytes == 32))
               : ((p[15] == 13 && bytes == 16 && zero(p + 48, 16)) || (p[15] == 19 && bytes == 32));
}
int nan_group_validate(const struct nan_command_digest *d, uint32_t action, const uint8_t *p,
                       size_t n, uint8_t *out, size_t *len)
{
    int error = nan_command_validate(d, action, p, n, out, len);
    if (error)
        return error;
    return syntax(p, n) && ((action & 1) ? get16(p + 2) != 3 : get16(p + 2) == 3) ? 0 : -2;
}
static int release(void *nan, struct group_key **slot)
{
    struct group_key *g = *slot;
    void *km = PTR(PTR(nan, 8), 0x420);
    if (lookup_bss(nan, g->local, g->id) != g->bss || !owned(km, g))
        return -30;
    int error = g->id < 4 ? nan_station_quiesce(nan, g->bss, g->scb) : 0;
    if (!error && g->beacon_refs) {
        g->draining = 1;
        g->ready = 0; /* Stop creating copies while the real PCBs drain. */
        return -16;
    }
    if (!error)
        error = nan_station_group_busy(nan, g->bss, g->scb);
    if (error)
        return error;
    g->ready = 0;
    if (g->key) {
        error = ((int (*)(void *))native_0x19119)(g->key);
        if (error || U8(g->key, 16))
            return error ? error : -16;
    }
    unsigned index = g->index;
    ((void (*)(void *, unsigned, void *, unsigned))native_0x158a1)(km, 0, &g->index, 1);
    void *record = (uint8_t *)PTR(km, 0x10) + index * 16;
    if (g->index != 0xffff || PTR(record, 0) || U16(record, 4))
        return -30;
    *slot = g->next;
    ((void (*)(void *))native_0x71491)(g);
    return 0;
}
static int resolve(void *nan, const uint8_t *p, void **bss, void **scb)
{
    *bss = lookup_bss(nan, p + 24, p[14]);
    *scb = 0;
    if (!*bss)
        return -30;
    return p[30] == 2 ? nan_station_group_owner(nan, *bss, p + 8, scb) : 0;
}
int nan_group_sequence(const struct nan_command_digest *d, const uint8_t *p, size_t n, uint8_t *out,
                       size_t *len)
{
    if (!syntax(p, n) || get16(p + 2) != 3)
        return -2;
    if (*len < 6)
        return -14;
    void *nan = ((void *const *)d)[1], *bss, *scb;
    int error = resolve(nan, p, &bss, &scb);
    if (error)
        return error;
    void *km = PTR(PTR(nan, 8), 0x420);
    struct group_key *g = *find(nan, bss, scb, p[14]);
    if (!g || !usable(km, g))
        return -30;
    error =
        ((int (*)(void *, void *, unsigned, unsigned, unsigned))native_0x1901d)(g->key, out, 6, 0, g->tx);
    if (error != 6)
        return error < 0 ? error : -30;
    *len = 6;
    return 0;
}
int nan_group_command(const struct nan_command_digest *d, const uint8_t *p, size_t n, uint8_t *out,
                      size_t *len)
{
    (void)out;
    *len = 0;
    if (!syntax(p, n) || get16(p + 2) == 3)
        return -2;
    void *nan = ((void *const *)d)[1], *bss, *scb;
    int error = resolve(nan, p, &bss, &scb);
    if (error)
        return error;
    struct group_key **slot = find(nan, bss, scb, p[14]);
    if (get16(p + 2) == 2)
        return *slot ? release(nan, slot) : -30;
    if (!nan_host_active(nan))
        return -25;
    if (*slot)
        return -16;
    error = nan_station_group_busy(nan, bss, scb);
    if (error)
        return error;
    unsigned count = 0;
    for (struct group_key *g = *head(nan); g; g = g->next) {
        ++count;
        if (!scb && g->bss == bss && g->tx &&
            (g->id < 4 ? 0 : g->id / 2) == (p[14] < 4 ? 0 : p[14] / 2))
            return -16;
    }
    if (count >= BCM_NAN_HOST_GROUP_KEY_LIMIT)
        return -16;
    void *km = PTR(PTR(nan, 8), 0x420);
    if (!km)
        return -30;
    struct group_key *g = ((void *(*)(size_t))native_0x7146d)(sizeof(*g));
    if (!g)
        return -27;
    g->index = 0xffff;
    error = ((int (*)(void *, unsigned, void *, unsigned))native_0x15815)(km, 0, &g->index, 1);
    if (error) {
        ((void (*)(void *))native_0x71491)(g);
        return error;
    }
    g->bss = bss;
    g->scb = scb;
    g->id = p[14];
    g->tx = p[30] == 1;
    g->algo = p[15];
    g->bytes = get16(p + 6);
    copy(g->local, p + 24, 6);
    copy(g->peer, p + 8, 6);
    *slot = g;
    uint32_t words[5];
    uint8_t *info = (uint8_t *)words;
    ((void (*)(void *, unsigned))native_0x150891)(words, sizeof(words));
    U16(info, 0) = g->index;
    copy(info + 2, g->peer, 6);
    words[2] = (g->id >= 4 ? 0x40 : 0x20) | (g->tx ? 2 : 4);
    info[13] = g->id;
    error = ((int (*)(void *, void *, void *))native_0x18da9)(km, info, &g->key);
    if (error)
        return error;
    unsigned flags = (scb ? 0x20 : 2) | (g->id >= 4 ? 0x80 : 0) | (g->tx ? 4 : 0);
    ((void (*)(void *, void *, unsigned, void *, unsigned, void *, void *))native_0x1cbe9)(
        km, (uint8_t *)PTR(km, 0x10) + g->index * 16, 0, g->key, flags, bss, scb);
    error = ((int (*)(void *, unsigned, const void *, unsigned))native_0x1925d)(g->key, g->algo, p + 32,
                                                                          g->bytes);
    if (error) {
        /* A2 changes algo/callbacks before allocating AES context. On this
         * exact allocation failure its normal reset dereferences NULL+6.
         * No cipher context or hardware slot exists: restore the native
         * disabled-key initializer, but retain the published owner/index and
         * original install error until explicit REMOVE completes. */
        if (error == -27 && U8(g->key, 16) == g->algo && !PTR(g->key, 0x24) &&
            U16(g->key, 0x1c) == 0xffff) {
            U8(g->key, 16) = 0;
            ((int (*)(void *))native_0x19df5)(g->key);
        }
        return error;
    }
    error = ((int (*)(void *, const void *, unsigned, unsigned, unsigned))native_0x193d9)(
        g->key, p + 16, 6, g->id >= 4 ? 0 : 0x7f, g->tx);
    if (error)
        return error;
    g->ready = 1;
    if (!usable(km, g)) {
        g->ready = 0;
        return -30;
    }
    return 0;
}
int nan_group_idle(void *nan, void *bss, void *scb)
{
    if (!nan || !PTR(nan, 0x14))
        return 0;
    for (struct group_key *g = *head(nan); g; g = g->next)
        if ((!bss || bss == g->bss) && (!scb || scb == g->scb))
            return -16;
    return 0;
}

int nan_group_data_ready(void *nan, void *bss, void *scb)
{
    void *km = PTR(PTR(nan, 8), 0x420);
    unsigned local = 0;
    for (struct group_key *g = *head(nan); g; g = g->next) {
        if (g->bss != bss || g->id >= 4 || (g->scb && g->scb != scb))
            continue;
        if (!usable(km, g))
            return -30;
        if (g->tx)
            local = 1;
    }
    /* Local GTK is required by this NDP backend. A peer GTK is optional
     * when not negotiated, but any partially installed peer key blocks use. */
    return local ? 0 : -30;
}
int nan_group_stop(void *nan)
{
    struct group_key **p = head(nan);
    while (*p) {
        int error = release(nan, p);
        if (error)
            return error;
    }
    return 0;
}

static int transmit_sequence(void *key)
{
    uint8_t pn[6];
    int bytes = ((int (*)(void *, void *, unsigned, unsigned, unsigned))native_0x1901d)(key, pn, 6, 0, 1);
    if (bytes != 6)
        return -30;
    for (unsigned i = 0; i < 6; ++i)
        if (pn[i] != 255)
            return 0;
    return -51; /* Exhausted PN must be replaced, never wrapped to zero. */
}

int nan_group_tx_crypto(void *nan, unsigned *iv, unsigned *icv)
{
    void *km = PTR(PTR(nan, 8), 0x420), *bss = nan_host_nmi(nan);
    unsigned id = nan_group_tx_id(km, bss, 1);
    struct group_key *g = *find(nan, bss, 0, id);
    if (!g || !usable(km, g))
        return -30;
    int error = transmit_sequence(g->key);
    if (error)
        return error;
    *iv = 0;
    *icv = 10 + U8(g->key, 21); /* MMIE header, ID, IPN, MIC. */
    return 0;
}

NATIVE_ENTRY static int
original_bip_tx(void *key __attribute__((unused)), void *packet __attribute__((unused)),
                const void *header __attribute__((unused)), void *body __attribute__((unused)),
                unsigned length __attribute__((unused)), void *descriptor __attribute__((unused)))
{
    __asm__ volatile("push.w {r0,r1,r4,r5,r6,r7,r8,r10,r11,lr}\nldr pc,=0x19d65");
}

int nan_group_bip_tx(void *key, void *packet, const uint8_t *header, void *body, unsigned length,
                     void *descriptor)
{
    void *wlc = PTR(key, 0), *wrapper = PTR(wlc, 0x520);
    void *nan = wrapper ? PTR(wrapper, 4) : 0;
    if (nan && PTR(nan, 0x14)) {
        for (struct group_key *g = *head(nan); g; g = g->next) {
            if (g->key != key)
                continue;
            if (!g->tx || !usable(PTR(wlc, 0x420), g))
                return -30;
            int error = transmit_sequence(key);
            if (error)
                return error;
            break;
        }
    }
    return original_bip_tx(key, packet, header, body, length, descriptor);
}

NATIVE_ENTRY static int
original_bip_mic(void *key __attribute__((unused)), void *packet __attribute__((unused)),
                 const void *header __attribute__((unused)), void *body __attribute__((unused)),
                 unsigned length __attribute__((unused)), void *mmie __attribute__((unused)))
{
    __asm__ volatile("push {r0-r6,lr}\nldrb r5,[r0,#0x10]\nldr pc,=0x19c05");
}

int nan_group_bip_mic(void *key, void *packet, const uint8_t *header, void *body, unsigned length,
                      void *mmie)
{
    if ((get16(header) & 0xfc) != 0x80)
        return original_bip_mic(key, packet, header, body, length, mmie);
    if (length < 12u + 10u + U8(key, 21))
        return -24;
    /* TSF is filled by hardware after signing. BIP authenticates zero here,
     * in both TX and RX, while the native beacon parser retains the real TSF. */
    uint8_t timestamp[8];
    copy(timestamp, body, 8);
    for (unsigned i = 0; i < 8; ++i)
        ((uint8_t *)body)[i] = 0;
    int error = original_bip_mic(key, packet, header, body, length, mmie);
    copy(body, timestamp, 8);
    return error;
}

static void beacon_complete(void *wlc, uint32_t status, struct group_key *g)
{
    (void)status;
    if (PTR(g->bss, 0) != wlc || !g->beacon_refs)
        return;
    --g->beacon_refs;
    if (!g->beacon_refs && g->draining) {
        void *nan = context(PTR(wlc, 0x420), g->bss);
        uint8_t words[BCM_NAN_HOST_HEADER_SIZE] __attribute__((aligned(4)));
        ((void (*)(void *, unsigned))native_0x150891)(words, sizeof(words));
        U16(words, 0) = BCM_NAN_HOST_VERSION;
        U16(words, 2) = BCM_NAN_HOST_KEY_DRAINED;
        U16(words, 4) = sizeof(words);
        U32(words, 8) = (unsigned)g->index + 1;
        U16(words, 16) = U16(PTR(g->bss, 0x98), 0x36);
        ((void (*)(void *, unsigned, unsigned, const void *, unsigned, unsigned))native_0x1fcb5)(
            nan, BCM_NAN_HOST_EVENT, BCM_NAN_HOST_REASON, words, sizeof(words), 0);
    }
}

NATIVE_ENTRY static void *
original_beacon_copy(void *mac __attribute__((unused)), void *bss __attribute__((unused)),
                      void *template __attribute__((unused)))
{
    __asm__ volatile("push.w {r0,r1,r4-r8,lr}\nldr pc,=0x2b5f5");
}

void *nan_group_beacon_copy(void *mac, void *bss, void *template)
{
    void *wlc = PTR(bss, 0), *km = PTR(wlc, 0x420), *nan = context(km, bss);
    if (!nan || nan_host_nmi(nan) != bss || PTR(nan, 0x14) != mac)
        return original_beacon_copy(mac, bss, template);
    struct group_key *g = *head(nan);
    while (g && !(g->bss == bss && g->tx && g->id >= 6))
        g = g->next;
    if (!g)
        return original_beacon_copy(mac, bss, template);
    if (!template || !nan_host_active(nan) || !usable(km, g) || transmit_sequence(g->key))
        return 0;
    void *osh = PTR(wlc, 4);
    unsigned length = ((unsigned (*)(void *, void *))native_0x150da1)(osh, template);
    if (length < 36 || length > BCM_NAN_HOST_MAX_FRAME)
        return 0;
    /* Original A2 copy has headroom but no MMIE tailroom. Allocate a fresh
     * native packet for every send; the cached beacon remains unsigned. */
    void *packet = ((void *(*)(void *, unsigned))native_0x11ea5)(
        osh, length + 0xcc + 10 + U8(g->key, 21));
    if (!packet)
        return 0;
    uint8_t *header = (uint8_t *)PTR(packet, 8) + 0xcc;
    PTR(packet, 8) = header;
    ((void (*)(void *, void *, unsigned, unsigned, void *))native_0xb979)(
        osh, template, 0, length, header);
    ((void (*)(void *, void *, void *))native_0x8bcf9)(wlc, template, packet);
    U16(packet, 12) = length;
    U32(packet, 0x10) = (U32(packet, 0x10) & ~7u) | (U32(template, 0x10) & 7u);
    if ((get16(header) & ~0x0800u) != 0x80 ||
        nan_group_bip_tx(g->key, packet, header, header + 24, length - 24, 0) ||
        ((int (*)(void *, uintptr_t, void *, void *))native_0x102161)(
            PTR(wlc, 0x2c4), (uintptr_t)beacon_complete, g, packet)) {
        ((void (*)(void *, void *, unsigned))native_0x151ae1)(osh, packet, 1);
        return 0;
    }
    ++g->beacon_refs;
    return packet;
}

NATIVE_ENTRY static unsigned
original_beacon_rx(void *mac __attribute__((unused)), void *bss __attribute__((unused)),
                    void *rx __attribute__((unused)), void *plcp __attribute__((unused)),
                    const void *header __attribute__((unused)), void *body __attribute__((unused)),
                    unsigned length __attribute__((unused)), unsigned band __attribute__((unused)))
{
    __asm__ volatile("push.w {r4-r11,lr}\nldr pc,=0x2f9e9");
}

unsigned nan_group_beacon_rx(void *mac, void *bss, void *rx, void *plcp, const uint8_t *header,
                              void *body, unsigned length, unsigned band)
{
    void *nan = PTR(mac, 4), *nmi = nan_host_nmi(nan);
    struct group_key *g = *head(nan);
    while (g && !(g->bss == nmi && !g->tx && g->id >= 6 && equal(g->peer, header + 10, 6)))
        g = g->next;
    /* Discovery before pairing has no peer BIGTK. Once a peer owns one,
     * its beacons must verify before native cluster/clock processing. */
    if (!g)
        return original_beacon_rx(mac, bss, rx, plcp, header, body, length, band);
    const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
    void *owner = 0, *km = PTR(PTR(nan, 8), 0x420);
    if (!nan_host_active(nan) || (get16(header) & ~0x0800u) != 0x80 ||
        body != header + 24 || (header[22] & 15u) || !equal(header + 4, broadcast, 6) ||
        nan_station_group_owner(nan, nmi, header + 10, &owner))
        return 1;
    for (unsigned trailer = 18; trailer <= 26; trailer += 8) {
        if (length < 12 + trailer || length > BCM_NAN_HOST_MAX_BODY + trailer)
            continue;
        uint8_t *mmie = (uint8_t *)body + length - trailer;
        unsigned id = get16(mmie + 2);
        if (mmie[0] != 76 || mmie[1] != trailer - 2 || id < 6 || id > 7)
            continue;
        g = *find(nan, nmi, owner, id);
        if (!g || !usable(km, g) || U8(g->key, 21) + 10u != trailer)
            continue;
        uint32_t status[2] = {0, 0};
        /* A2 software BIP RX does not dereference its packet argument. The
         * native beacon route supplies the complete header/body directly. */
        if (((int (*)(void *, void *, const void *, void *, unsigned, void *))native_0x19c45)(
                g->key, 0, header, body, length, status))
            return 1;
        return original_beacon_rx(mac, bss, rx, plcp, header, body, length - trailer, band);
    }
    return 1;
}

NATIVE_ENTRY static int
original_mfp_rx(void *mfp __attribute__((unused)), void *bss __attribute__((unused)),
                void *scb __attribute__((unused)), void *rx __attribute__((unused)),
                const uint8_t *header __attribute__((unused)), void *packet __attribute__((unused)))
{
    __asm__ volatile("push.w {r4-r11,lr}\nldr pc,=0xf05a1");
}

int nan_group_mfp_rx(void *mfp, void *bss, void *scb, void *rx, const uint8_t *header, void *packet)
{
    void *wlc = PTR(mfp, 0), *km = PTR(wlc, 0x420);
    void *nan = context(km, bss);
    uint8_t *body = PTR(packet, 8);
    unsigned length = U16(packet, 12);
    if (!nan || nan_host_nmi(nan) != bss || !(header[4] & 1) || (get16(header) & 0xfc) != 0xd0 ||
        length < 6 || (body[0] != 4 && body[0] != 9) || body[1] != 9 || body[2] != 0x50 ||
        body[3] != 0x6f || body[4] != 0x9a || (body[5] != 0x13 && (body[5] != 0x18 || length < 7)))
        return original_mfp_rx(mfp, bss, scb, rx, header, packet);
    /* Public NAN discovery remains unprotected even after installing IGTK.
     * Protected Dual multicast is verified here with the sender's key; A2's
     * original MFP code would select the local BSS IGTK instead. */
    if (body[0] == 4)
        return !(get16(header) & 0x4000);
    void *owner = 0;
    if (!nan_host_active(nan) || (get16(header) & ~0x0800u) != 0xd0 || body != header + 24 ||
        (header[22] & 15u) || !nan_host_group_address(header + 4) ||
        !equal(header + 16, (uint8_t *)bss + 0x60, 6) ||
        nan_station_group_owner(nan, bss, header + 10, &owner) || owner != scb ||
        !(U32(scb, 4) & 0x200u))
        return 0;
    for (unsigned trailer = 18; trailer <= 26; trailer += 8) {
        if (length < 7 + trailer || length > BCM_NAN_HOST_MAX_BODY + trailer)
            continue;
        const uint8_t *mmie = body + length - trailer;
        unsigned id = get16(mmie + 2);
        if (mmie[0] != 76 || mmie[1] != trailer - 2 || id < 4 || id > 5)
            continue;
        struct group_key *g = *find(nan, bss, scb, id);
        if (!g || !usable(km, g) || U8(g->key, 21) + 10u != trailer)
            continue;
        uint32_t status[2];
        status[0] = status[1] = 0; /* Force actual software integrity/replay. */
        int error = ((int (*)(void *, void *, const void *, void *, unsigned, void *))native_0x19c45)(
            g->key, packet, header, body, length, status);
        if (!error)
            nan_host_group_received(nan, header, body, length - trailer, (const uint8_t *)rx - 12);
        return 0; /* Consumed; never reverify the same PN or run STA policy. */
    }
    return 0;
}

NATIVE_ENTRY static unsigned
original_sendmgmt(void *wlc __attribute__((unused)), void *packet __attribute__((unused)),
                  void *queue __attribute__((unused)), void *scb __attribute__((unused)),
                  void *bss __attribute__((unused)), unsigned short_header __attribute__((unused)),
                  void *key __attribute__((unused)), void *parameters __attribute__((unused)))
{
    __asm__ volatile("push.w {r4-r11,lr}\nldr pc,=0x1a979d");
}

unsigned nan_group_sendmgmt(void *wlc, void *packet, void *queue, void *scb, void *bss,
                            unsigned short_header, void *key, void *parameters)
{
    void *km = PTR(wlc, 0x420), *nan = context(km, bss);
    const uint8_t *header = PTR(packet, 8);
    if (nan && nan_host_nmi(nan) == bss && (U32(packet, 0x1c) & 0x20000000u) &&
        U16(packet, 12) >= 24 && (header[4] & 1u)) {
        unsigned id = nan_group_tx_id(km, bss, 1);
        struct group_key *g = *find(nan, bss, 0, id);
        if (!g || !usable(km, g) || (key && key != g->key) || transmit_sequence(g->key)) {
            /* Match sendmgmt's rejection ownership: actual packet free invokes
             * its PCB, then the caller can finish its submission transaction. */
            ((void (*)(void *, void *, unsigned))native_0x151ae1)(PTR(wlc, 4), packet, 1);
            return 0;
        }
        key = g->key;
    }
    return original_sendmgmt(wlc, packet, queue, scb, bss, short_header, key, parameters);
}

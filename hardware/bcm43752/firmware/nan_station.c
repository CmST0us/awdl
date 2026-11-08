/* Host-owned NMI/NDI SCBs and pairwise keys in the hash-locked A2 native
 * key manager. Native NDP records bind negotiated NDI identities to peers. */
#include "nan_station.h"
#include "nan_host.h"
#include "nan_mac.h"
#include "nan_host_abi.h"
#include "nan_link.h"
#include "nan_group.h"
#include "nan_data.h"

#define FIELD(type, base, offset) (*(type *)((uint8_t *)(base) + (offset)))
#define PTR(base, offset) FIELD(void *, base, offset)
#define U32(base, offset) FIELD(uint32_t, base, offset)
#define U16(base, offset) FIELD(uint16_t, base, offset)
#define U8(base, offset) FIELD(uint8_t, base, offset)

struct station {
    struct station *next;
    void *scb, *bss;
    uint8_t address[6], flags, key_owned, pinned;
    uint8_t data;
    void *record, *peer, *ies;
    uint8_t local[6], nmi[6], id, ndp_ref, closing;
    uint16_t ie_length;
    uint8_t rx_failure_reports;
};

static unsigned get16(const uint8_t *p)
{
    return p[0] | (unsigned)p[1] << 8;
}

static int zero(const uint8_t *p, unsigned length)
{
    for (unsigned i = 0; i < length; ++i)
        if (p[i])
            return 0;
    return 1;
}

static void copy(void *to, const void *from, size_t length)
{
    ((void *(*)(void *, const void *, size_t))0x151a21u)(to, from, length);
}

static int equal(const void *a, const void *b, size_t length)
{
    return !((int (*)(const void *, const void *, size_t))0xa7a9u)(a, b, length);
}

static struct station **head(void *nan)
{
    return (struct station **)((uint8_t *)PTR(nan, 0x14) + NAN_MAC_HOST_STATIONS);
}

static struct station **find(void *nan, void *bss, const uint8_t *address)
{
    struct station **slot = head(nan);
    while (*slot && ((*slot)->bss != bss || !equal((*slot)->address, address, 6)))
        slot = &(*slot)->next;
    return slot;
}

static void *native_find(void *nan, void *bss, const uint8_t *address)
{
    return ((void *(*)(void *, void *, const uint8_t *))0x123d05u)(
        PTR(nan, 8), bss, address);
}

static int syntax(const uint8_t *input, size_t length, unsigned key)
{
    if (length != (key == 2 ? BCM_NAN_HOST_DATA_KEY_SIZE :
                   key ? BCM_NAN_HOST_KEY_SIZE : BCM_NAN_HOST_STATION_SIZE) ||
        get16(input) != BCM_NAN_HOST_VERSION || get16(input + 4) != length ||
        (get16(input + 2) != BCM_NAN_HOST_ADD &&
         get16(input + 2) != BCM_NAN_HOST_REMOVE) ||
        (input[8] & 1u) || zero(input + 8, 6))
        return 0;
    if (!key)
        return !(get16(input + 6) & ~BCM_NAN_HOST_MFP) && zero(input + 14, 10) &&
            (get16(input + 2) != BCM_NAN_HOST_REMOVE || !get16(input + 6));
    if (input[14] || !zero(input + 22, 10))
        return 0;
    if (get16(input + 2) == BCM_NAN_HOST_REMOVE)
        return !get16(input + 6) && zero(input + 15, 49);
    unsigned bytes = get16(input + 6);
    return (input[15] == 4 && bytes == 16 && zero(input + 48, 16)) ||
        (input[15] == 16 && bytes == 32);
}

int nan_station_validate(const struct nan_command_digest *digest, uint32_t action,
                           const uint8_t *input, size_t length,
                           uint8_t *output, size_t *output_length)
{
    int error = nan_command_validate(digest, action, input, length, output, output_length);
    if (error)
        return error;
    const struct nan_command *command = ((const struct nan_command *const *)digest)[3];
    return syntax(input, length, command->id == BCM_NAN_HOST_KEY_COMMAND) ? 0 : -2;
}

static int station_present(void *nan, struct station *entry)
{
    /* Never dereference an SCB before checking its native lookup table. */
    void *bss = entry->data ? ((void *(*)(void *, const void *))0x22a1du)(
        PTR(nan, 0x30), entry->local) : nan_host_nmi(nan);
    return entry->pinned && bss == entry->bss &&
        native_find(nan, entry->bss, entry->address) == entry->scb &&
        PTR(entry->scb, 0x0c) == entry->bss && U8(entry->scb, 0x1d);
}

static int station_busy(void *nan, struct station *entry)
{
    if (entry->data && entry->scb) {
        if (!station_present(nan, entry))
            return -30;
        if (U8(entry->scb, 0x1d) != 1)
            return -16;
        /* NDI data and NMI management use different BSS/SCB/key owners,
         * even when their peer MACs are equal. An NDP termination frame
         * still using the NMI must not block removal of the drained PTK. */
        return 0;
    }
    return nan_host_peer_busy(nan, entry->address);
}

static int deauthorize(void *nan, struct station *entry)
{
    if (!entry->data || !entry->scb)
        return 0;
    if (!station_present(nan, entry))
        return -30;
    ((void (*)(void *, void *, unsigned))0x123821u)(PTR(nan, 8), entry->scb, 0x10);
    return U8(entry->scb, 0x1c) & 0x10 ? -16 : 0;
}

int nan_station_quiesce(void *nan, void *bss, void *scb)
{
    for (struct station *entry = *head(nan); entry; entry = entry->next) {
        if ((bss && entry->bss != bss) || (scb && entry->scb != scb))
            continue;
        int error = deauthorize(nan, entry);
        if (error)
            return error;
    }
    return 0;
}

static int key_info(void *nan, struct station *entry, uint8_t info[20])
{
    void *manager = PTR(PTR(nan, 8), 0x420);
    if (!manager || !station_present(nan, entry))
        return -30;
    /* A2 wlc_keymgmt_get_scb_key, key ID 0, pairwise flags 0. Its native
     * wlc_key_get_info fills 20 bytes, even for the null/disabled key. */
    ((void *(*)(void *, void *, unsigned, unsigned, void *))0x151ec1u)(
        manager, entry->scb, 0, 0, info);
    return 0;
}

static int key_usable(const uint8_t info[20], const uint8_t *address,
                         unsigned direction)
{
    /* A2 key info: index+0, peer+2, flags+8, algo+12, ID+13,
     * key bytes+15, IV+16, ICV+17. Never accept a disabled or partial key. */
    return get16(info) != 0xffff && equal(info + 2, address, 6) &&
        (info[8] & direction) == direction && !info[13] && info[16] == 8 &&
        ((info[12] == 4 && info[15] == 16 && info[17] == 8) ||
         (info[12] == 16 && info[15] == 32 && info[17] == 16));
}

static int iovar(void *nan, struct station *entry, const char *name,
                   void *data, unsigned length)
{
    return ((int (*)(void *, const char *, void *, unsigned, void *, unsigned,
                      unsigned, void *))0xdf6d5u)(
        PTR(nan, 8), name, 0, 0, data, length, 1, PTR(entry->bss, 0x0c));
}

static int key_replay_window(void *nan, struct station *entry, unsigned enable)
{
    void *manager = PTR(PTR(nan, 8), 0x420);
    uint8_t info[20];
    if (!manager || !station_present(nan, entry))
        return -30;
    /* NAN has no infrastructure association transition to enable IVTW.
     * Resolve/allocate the native pairwise key after the BSS security policy
     * is set, before wsec_key sets its receive PN. A2 otherwise marks the
     * installed key for IVTW but returns BADKEYIDX for its missing window. */
    void *key;
    if (enable)
        key = ((void *(*)(void *, void *, const void *, unsigned, unsigned,
                         void *))0x1ca65u)(manager, entry->bss, entry->address, 0, 0, info);
    else
        key = ((void *(*)(void *, void *, unsigned, unsigned, void *))0x151ec1u)(
            manager, entry->scb, 0, 0, info);
    unsigned index = get16(info);
    if (!enable && index == 0xffff)
        return 0; /* The install failed before native key allocation. */
    if (index >= U16(manager, 0x0e) || info[12] || info[13] ||
        !equal(info + 2, entry->address, 6)) {
        ((int (*)(const char *, ...))0xa815u)(
            "NAN key replay lookup: data=%u index=%u algo=%u id=%u wsec=%x\n",
            entry->data, index, info[12], info[13], U32(entry->bss, 0x48));
        return -30;
    }
    void *record = (uint8_t *)PTR(manager, 0x10) + index * 16;
    void *ivtw = PTR(manager, 0x3c);
    if (PTR(record, 0) != key || PTR(record, 8) != entry->scb ||
        (U16(record, 4) & 0x4001) != 0x4001 || !ivtw ||
        index >= U16(ivtw, 8) || !PTR(ivtw, 12)) {
        ((int (*)(const char *, ...))0xa815u)(
            "NAN key replay owner: data=%u index=%u flags=%x scb=%p owner=%p\n",
            entry->data, index, U16(record, 4), entry->scb, PTR(record, 8));
        return -30;
    }
    int error = ((int (*)(void *, unsigned, unsigned))0x18bb1u)(ivtw, index, enable);
    if (error)
        return error;
    /* Allocation/free must actually finish; retain the station and key
     * cleanup obligation if the native global policy prevented the change. */
    return !!PTR(PTR(ivtw, 12), index * 4) == !!enable ? 0 : -16;
}

static int key_write(void *nan, struct station *entry, const uint8_t *input)
{
    uint32_t words[41];
    uint8_t *key = (uint8_t *)words;
    ((void (*)(void *, size_t))0x150891u)(words, sizeof(words));
    /* Exact A2 wl_wsec_key_t: 164 bytes. The native NAN setter 0x394f8
     * uses the same primary flag, algorithm, length and peer offsets. */
    words[0x74 / 4] = 2;
    copy(key + 0x9c, entry->address, 6);
    if (input) {
        words[1] = get16(input + 6);
        words[0x70 / 4] = input[15];
        copy(key + 8, input + 32, words[1]);
        words[0x84 / 4] = 1;
        /* wl_wsec_key_t stores receive PN as { u32 high, u16 low }. */
        copy(key + 0x8c, input + 18, 4);
        copy(key + 0x90, input + 16, 2);
    }
    /* The generic wsec_key wrapper authorizes NAN SCBs immediately. Clearing
     * that bit afterwards invokes A2's key-manager state callback, which
     * deletes the just-installed PTK. For NDI ADD call the same native key
     * installer below that wrapper, leaving authorization to the explicit
     * hostap AUTHORIZE command after all negotiated keys are installed. */
    if (entry->data)
        ((void (*)(void *, void *, unsigned))0x123821u)(PTR(nan, 8), entry->scb, 0x10);
    int error = entry->data && input
        ? ((int (*)(void *, void *, unsigned, void *, void *))0x18009u)(
              PTR(PTR(nan, 8), 0x420), entry->bss, 0, key, key)
        : iovar(nan, entry, (const char *)0x157160u, key, sizeof(words));
    /* Do not retain temporary key material on a reusable firmware stack. */
    for (unsigned i = 0; i < sizeof(words); ++i)
        ((volatile uint8_t *)key)[i] = 0;
    return error;
}

static int remove_key(void *nan, struct station *entry)
{
    int error = deauthorize(nan, entry);
    if (error)
        return error;
    if (!entry->key_owned)
        return 0;
    error = station_busy(nan, entry);
    if (error)
        return error;
    uint8_t info[20];
    error = key_info(nan, entry, info);
    if (error)
        return error;
    /* An install can fail before creating a key. Observation of the native
     * disabled key then releases cleanup ownership without a fabricated SET. */
    if (info[12]) {
        /* Reset the exact owned key. The generic wsec_key DELETE resolves
         * its BSS/peer again; use the verified SCB owner for both NMI and
         * NDI instead. Native reset emits the normal
         * key-manager notifications and erases the real cipher context. */
        void *manager = PTR(PTR(nan, 8), 0x420);
        void *key = ((void *(*)(void *, void *, unsigned, unsigned, void *))
                     0x151ec1u)(manager, entry->scb, 0, 0, info);
        unsigned index = get16(info);
        if (!key || index >= U16(manager, 0x0e) ||
            !equal(info + 2, entry->address, 6) ||
            PTR((uint8_t *)PTR(manager, 0x10) + index * 16, 8) != entry->scb)
            return -30;
        error = ((int (*)(void *))0x19119u)(key);
        if (error)
            return error;
        error = key_info(nan, entry, info);
        if (error || info[12])
            return error ? error : -16;
    }
    error = key_replay_window(nan, entry, 0);
    if (error)
        return error;
    entry->key_owned = 0;
    return 0;
}

static int remove_data_station(void *nan, struct station **slot);

static int remove_station(void *nan, struct station **slot)
{
    struct station *entry = *slot;
    if (entry->data)
        return remove_data_station(nan, slot);
    if (!station_present(nan, entry))
        return -30;
    int error = nan_group_idle(nan, entry->bss, entry->scb);
    if (!error)
        error = nan_host_peer_busy(nan, entry->address);
    if (!error)
        error = remove_key(nan, entry);
    if (error)
        return error;
    uint8_t info[20];
    error = key_info(nan, entry, info);
    if (error || info[12])
        return error ? error : -16;
    /* Pin exactly one native reference while owned. Native SCB free rejects
     * nonzero +0x1d; additional packet/flow owners must drain first. */
    if (U8(entry->scb, 0x1d) != 1)
        return -16;
    --U8(entry->scb, 0x1d);
    unsigned freed = ((unsigned (*)(void *, void *))0x123d59u)(PTR(nan, 8), entry->scb);
    void *remaining = native_find(nan, entry->bss, entry->address);
    if (remaining == entry->scb) {
        ++U8(entry->scb, 0x1d);
        return -16;
    }
    /* Success requires both native completion and disappearance. A native
     * inconsistency retains metadata for diagnosis, never a stale dereference. */
    if (!freed || remaining)
        return -30;
    *slot = entry->next;
    ((void (*)(void *))0x71491u)(entry);
    return 0;
}

int nan_station_command(const struct nan_command_digest *digest,
                          const uint8_t *input, size_t length,
                          uint8_t *output, size_t *output_length)
{
    (void)output;
    *output_length = 0;
    if (!syntax(input, length, 0))
        return -2;
    void *nan = ((void *const *)digest)[1];
    void *bss = nan_host_nmi(nan);
    if (!bss)
        return -25;
    struct station **slot = find(nan, bss, input + 8);
    if (get16(input + 2) == BCM_NAN_HOST_REMOVE) {
        if (*slot)
            return remove_station(nan, slot);
        /* A rejected ADD may have allocated nothing. Confirm absence in
         * the native table; never discard an untracked SCB or TX owner. */
        return native_find(nan, bss, input + 8) ||
            nan_host_peer_busy(nan, input + 8) ? -16 : 0;
    }
    if (!nan_host_active(nan))
        return -25;
    if (equal(input + 8, (uint8_t *)bss + 0x66, 6))
        return -2;
    unsigned flags = get16(input + 6);
    if (*slot) {
        if (!station_present(nan, *slot))
            return -30;
        if ((*slot)->flags == flags)
            return 0;
        /* Enabling MFP on an unkeyed SCB cannot invalidate an unprotected
         * discovery/Auth response. Disabling it still drains every TX. */
        if ((*slot)->key_owned || (flags & BCM_NAN_HOST_MFP
                ? nan_host_peer_protected_busy(nan, input + 8)
                : nan_host_peer_busy(nan, input + 8)))
            return -16;
        (*slot)->flags = flags;
        U32((*slot)->scb, 4) = (U32((*slot)->scb, 4) & ~0x200u) |
            (flags & BCM_NAN_HOST_MFP ? 0x200u : 0);
        return 0;
    }
    unsigned count = 0;
    for (struct station *entry = *head(nan); entry; entry = entry->next)
        ++count;
    if (count >= BCM_NAN_HOST_STATION_LIMIT || native_find(nan, bss, input + 8))
        return -16;
    /* A queued public discovery response has no station/key owner yet.
     * Creating this first SCB cannot invalidate an existing reference;
     * key mutation and station removal still wait for their TX owners. */
    struct station *entry = ((void *(*)(size_t))0x7146du)(sizeof(*entry));
    if (!entry)
        return -27;
    entry->bss = bss;
    copy(entry->address, input + 8, 6);
    entry->scb = ((void *(*)(void *, void *, const uint8_t *))0x123e65u)(
        PTR(nan, 8), bss, entry->address);
    if (!entry->scb) {
        ((void (*)(void *))0x71491u)(entry);
        return -27;
    }
    *slot = entry;
    /* Pin our own reference independently of any native cubby owner. A
     * conflicting allocation or saturated counter retains its metadata. */
    if (PTR(entry->scb, 0x0c) != bss || U8(entry->scb, 0x1d) == 255)
        return -30;
    ++U8(entry->scb, 0x1d);
    entry->pinned = 1;
    entry->flags = flags;
    U32(entry->scb, 4) = (U32(entry->scb, 4) & ~0x200u) |
        (flags & BCM_NAN_HOST_MFP ? 0x200u : 0);
    return 0;
}

int nan_station_key_command(const struct nan_command_digest *digest,
                              const uint8_t *input, size_t length,
                              uint8_t *output, size_t *output_length)
{
    (void)output;
    *output_length = 0;
    const struct nan_command *command = ((const struct nan_command *const *)digest)[3];
    unsigned data = command->id == BCM_NAN_HOST_DATA_KEY_COMMAND;
    if (!syntax(input, length, data ? 2 : 1) ||
        (data && (
                  !zero(input + 70, 10) || (input[64] & 1u) || zero(input + 64, 6))))
        return -2;
    void *nan = ((void *const *)digest)[1];
    if (!nan_host_nmi(nan))
        return -25;
    void *bss = data ? ((void *(*)(void *, const void *))0x22a1du)(
        PTR(nan, 0x30), input + 64) : nan_host_nmi(nan);
    struct station *entry = *find(nan, bss, input + 8);
    if (!entry)
        return -30;
    if (get16(input + 2) == BCM_NAN_HOST_REMOVE)
        return remove_key(nan, entry);
    if (!nan_host_active(nan))
        return -25;
    if (!(entry->flags & BCM_NAN_HOST_MFP))
        return -2;
    if (entry->closing || entry->key_owned || !station_present(nan, entry) ||
        (data ? station_busy(nan, entry) : nan_host_peer_protected_busy(nan, entry->address)))
        return -16;
    uint8_t info[20];
    int error = key_info(nan, entry, info);
    if (error || info[12])
        return error ? error : -16;
    entry->key_owned = 1;
    /* A2's hardware key matcher uses TA, which cannot distinguish the two
     * keys when a peer's NMI equals its NDI. Keep low-volume NMI protection
     * in the native software cipher via WSEC_SWFLAG, leaving hardware key
     * slots to NDI data. Native CCMP/GCMP integrity and PN checks still run.
     * Each owned BSS keeps this policy until actual BSSCFG destruction. */
    uint32_t value = data ? 4 : 4 | 8;
    unsigned stage = 1;
    error = iovar(nan, entry, (const char *)0x1571aau, &value, sizeof(value));
    if (!error) {
        stage = 2;
        value = 0x80;
        error = iovar(nan, entry, (const char *)0x15786eu, &value, sizeof(value));
    }
    if (!error) {
        stage = 3;
        /* DHD enables buf_key_b4_m4 by default. NDI inherits that BSS
         * policy, but its four-way exchange is carried by NAN management
         * frames and completed by hostap, not the firmware EAPOL parser.
         * Apply this only to the owned NAN BSS through its native wlcif. */
        value = 0;
        error = iovar(nan, entry, (const char *)0x157176u, &value, sizeof(value));
    }
    if (!error) {
        stage = 4;
        error = key_replay_window(nan, entry, 1);
    }
    if (!error) {
        stage = 5;
        error = key_write(nan, entry, input);
    }
    if (error) {
        ((int (*)(const char *, ...))0xa815u)(
            "NAN key install: data=%u stage=%u error=%d\n", entry->data, stage, error);
        return error;
    }
    error = key_info(nan, entry, info);
    if (error || info[12] != input[15] || !key_usable(info, entry->address, 6)) {
        ((int (*)(const char *, ...))0xa815u)(
            "NAN key verify: data=%u error=%d index=%u algo=%u flags=%x\n",
            entry->data, error, error ? 0xffff : get16(info),
            error ? 0 : info[12], error ? 0 : U32(info, 8));
        return error ? error : -30;
    }
    entry->key_owned = 2;
    return 0;
}

static int data_syntax(const uint8_t *input, size_t length, unsigned key)
{
    if (key)
        return length == BCM_NAN_HOST_DATA_KEY_SIZE &&
            syntax(input, length, 2) &&
            !(input[64] & 1u) && !zero(input + 64, 6) && zero(input + 70, 10);
    if (length < BCM_NAN_HOST_DATA_STATION_SIZE ||
        length > BCM_NAN_HOST_DATA_STATION_SIZE + BCM_NAN_HOST_DATA_IE_MAX ||
        get16(input) != BCM_NAN_HOST_VERSION || get16(input + 4) != length ||
        !input[26] || input[27] || get16(input + 30) ||
        get16(input + 28) != length - BCM_NAN_HOST_DATA_STATION_SIZE)
        return 0;
    for (unsigned off = 8; off <= 20; off += 6)
        if ((input[off] & 1u) || zero(input + off, 6))
            return 0;
    /* A peer may use the same MAC for NMI and NDI. Their native SCBs
     * remain separate because the owning BSS is part of every lookup. */
    if (equal(input + 8, input + 14, 6) || equal(input + 14, input + 20, 6))
        return 0;
    if (get16(input + 2) >= BCM_NAN_HOST_REMOVE &&
        get16(input + 2) <= BCM_NAN_HOST_GET_INACTIVITY)
        return !get16(input + 6) && length == BCM_NAN_HOST_DATA_STATION_SIZE;
    if (get16(input + 2) != BCM_NAN_HOST_ADD || get16(input + 6) != BCM_NAN_HOST_MFP)
        return 0;
    unsigned seen = 0;
    for (size_t off = BCM_NAN_HOST_DATA_STATION_SIZE; off < length;) {
        if (length - off < 2 || input[off + 1] > length - off - 2)
            return 0;
        unsigned id = input[off], bytes = input[off + 1], bit = 0;
        if (id == 45) {
            if (bytes != 26)
                return 0;
            bit = 1;
        } else if (id == 191) {
            if (bytes != 12)
                return 0;
            bit = 2;
        } else if (id == 255) {
            if (!bytes)
                return 0;
            if (input[off + 2] == 35) {
                /* HE MCS maps grow by four bytes for each wide-band bit.
                 * If PPE exists, its header and NSS/RU pairs must fit too. */
                if (bytes < 22)
                    return 0;
                unsigned need = 22 + ((input[off + 9] & 8) ? 4 : 0) +
                    ((input[off + 9] & 16) ? 4 : 0);
                if (bytes < need)
                    return 0;
                if (input[off + 15] & 0x80) {
                    if (bytes <= need)
                        return 0;
                    unsigned ppe = input[off + 2 + need], ru = (ppe >> 3) & 15;
                    unsigned count = 0;
                    for (; ru; ru >>= 1)
                        count += ru & 1;
                    if (!count || bytes < need + (7 + 6 * ((ppe & 7) + 1) * count + 7) / 8)
                        return 0;
                }
                bit = 4;
            }
        }
        if (seen & bit)
            return 0;
        seen |= bit;
        off += bytes + 2;
    }
    return (seen & 1) != 0;
}

int nan_data_station_validate(const struct nan_command_digest *digest, uint32_t action,
                              const uint8_t *input, size_t length,
                              uint8_t *output, size_t *output_length)
{
    int error = nan_command_validate(digest, action, input, length, output, output_length);
    if (error)
        return error;
    const struct nan_command *command = ((const struct nan_command *const *)digest)[3];
    unsigned key = command->id == BCM_NAN_HOST_DATA_KEY_COMMAND;
    if (!data_syntax(input, length, key))
        return -2;
    unsigned query = !key && get16(input + 2) == BCM_NAN_HOST_GET_INACTIVITY;
    return ((action & 1) ? !query : query) ? 0 : -2;
}

static int data_record_present(void *nan, struct station *entry)
{
    void *ndp = PTR(nan, 0x34);
    if (!ndp || !PTR(ndp, 0x10))
        return 0;
    for (unsigned i = 0; i < U32(ndp, 0x30); ++i)
        if (PTR(PTR(ndp, 0x10), i * 4) == entry->record)
            return 1;
    return 0;
}

int nan_data_station_inactivity(const struct nan_command_digest *digest,
                                const uint8_t *input, size_t length,
                                uint8_t *output, size_t *output_length)
{
    size_t capacity = *output_length;
    *output_length = 0;
    if (!data_syntax(input, length, 0) ||
        get16(input + 2) != BCM_NAN_HOST_GET_INACTIVITY)
        return -2;
    if (!output || capacity < 4)
        return -14;
    void *nan = ((void *const *)digest)[1];
    if (!nan_host_active(nan) || !PTR(nan, 0x30))
        return -25;
    void *bss = ((void *(*)(void *, const void *))0x22a1du)(PTR(nan, 0x30), input + 14);
    if (!bss || U32(bss, 0x74) != 5 || U32(bss, 0x78) != 11)
        return -30;
    struct station *entry = *find(nan, bss, input + 20);
    if (!entry || !entry->data || entry->closing || entry->id != input[26] ||
        !equal(entry->nmi, input + 8, 6) || !station_present(nan, entry) ||
        !entry->ndp_ref || !data_record_present(nan, entry) ||
        !U8(entry->record, 0x2a) ||
        ((void *(*)(void *, const void *, unsigned))0x37189u)(nan, entry->nmi, 0) != entry->peer ||
        ((void *(*)(void *, void *))0x37511u)(nan, entry->scb) != entry->peer)
        return -30;
    /* Hash-locked A2 sta_info (ROM 0x8eb40) uses pub->now - scb->used.
     * Native RX updates scb+0x20, including the NAN data receive path.
     * Read only that verified native state; no host timer, polling side
     * effect, or fabricated activity can postpone the public NDL cleanup. */
    uint32_t idle = U32(PTR(PTR(nan, 8), 0), 0x28) - U32(entry->scb, 0x20);
    for (unsigned i = 0; i < 4; ++i)
        output[i] = (uint8_t)(idle >> (8 * i));
    *output_length = 4;
    return 0;
}

static int remove_data_station(void *nan, struct station **slot)
{
    struct station *entry = *slot;
    if (((void *(*)(void *, const void *, unsigned))0x37189u)(
            nan, entry->nmi, 0) != entry->peer ||
        (entry->record && !data_record_present(nan, entry)) ||
        PTR(entry->peer, 0x38) != entry->ies || U32(entry->peer, 0x34) != entry->ie_length)
        return -30;
    int error = deauthorize(nan, entry);
    if (!error)
        error = nan_group_idle(nan, entry->bss, entry->scb);
    if (!error)
        error = station_busy(nan, entry);
    if (error)
        return error;
    entry->closing = 1;
    if (entry->scb) {
        error = remove_key(nan, entry);
        uint8_t info[20];
        if (!error)
            error = key_info(nan, entry, info);
        if (error || info[12])
            return error ? error : -16;
        if (entry->ndp_ref) {
            unsigned left = ((unsigned (*)(void *, void *, unsigned))0x376e9u)(
                nan, entry->record, 1);
            if (left || U8(entry->record, 0x2a))
                return -16;
            entry->ndp_ref = 0;
        }
        unsigned freed = ((unsigned (*)(void *, void *))0x127c5du)(
            PTR(PTR(nan, 8), 0x678), entry->scb);
        void *remaining = native_find(nan, entry->bss, entry->address);
        if (remaining == entry->scb) {
            /* Native slotted delete clears its own pin before cleanup. If
             * cleanup refuses, retain that pin and the actual host record. */
            if (!U8(entry->scb, 0x1d))
                U8(entry->scb, 0x1d) = 1;
            return -16;
        }
        if (!freed || remaining)
            return -30;
        entry->scb = 0;
        entry->pinned = 0;
    }
    if (entry->record) {
        error = nan_ndp_entry_release(PTR(nan, 0x34), entry->record);
        if (error)
            return error;
        entry->record = 0;
    }
    PTR(entry->peer, 0x38) = 0;
    U32(entry->peer, 0x34) = 0;
    ((void (*)(void *))0x71491u)(entry->ies);
    *slot = entry->next;
    ((void (*)(void *))0x71491u)(entry);
    return 0;
}

static int authorize_data_station(void *nan, struct station *entry)
{
    if (!nan_host_active(nan))
        return -25;
    if (entry->closing || !station_present(nan, entry) || !entry->ndp_ref ||
        !data_record_present(nan, entry) || !U8(entry->record, 0x2a) ||
        !PTR(entry->peer, 0x0c) || U8(entry->peer, 9) < 2 ||
        ((void *(*)(void *, void *))0x37511u)(nan, entry->scb) != entry->peer)
        return -30;
    uint8_t info[20];
    int error = key_info(nan, entry, info);
    if (error || entry->key_owned != 2 || !key_usable(info, entry->address, 6) ||
        !(U32(entry->scb, 4) & 0x200u) || (U8(entry->scb, 0x1c) & 3) != 3)
        return error ? error : -30;
    error = nan_group_data_ready(nan, entry->bss, entry->scb);
    if (error)
        return error;
    if (U8(entry->scb, 0x1c) & 0x10)
        return 0;
    error = station_busy(nan, entry);
    if (error)
        return error;
    ((void (*)(void *, void *, unsigned))0x123a7du)(PTR(nan, 8), entry->scb, 0x10);
    return U8(entry->scb, 0x1c) & 0x10 ? 0 : -16;
}

int nan_data_station_command(const struct nan_command_digest *digest,
                             const uint8_t *input, size_t length,
                             uint8_t *output, size_t *output_length)
{
    (void)output;
    *output_length = 0;
    if (!data_syntax(input, length, 0) ||
        get16(input + 2) == BCM_NAN_HOST_GET_INACTIVITY)
        return -2;
    void *nan = ((void *const *)digest)[1], *dp = PTR(nan, 0x30);
    if (!dp || !PTR(nan, 0x34))
        return -25;
    unsigned index = ((unsigned (*)(void *, const void *))0x22a8du)(dp, input + 14);
    if (index >= 2)
        return -30;
    void *bss = ((void *(*)(void *, const void *))0x22a1du)(dp, input + 14);
    if (!bss || U32(bss, 0x74) != 5 || U32(bss, 0x78) != 11)
        return -30;
    struct station **slot = find(nan, bss, input + 20);
    unsigned operation = get16(input + 2);
    if (operation != BCM_NAN_HOST_ADD) {
        if (operation == BCM_NAN_HOST_REMOVE && !*slot) {
            /* The host retains attempted ADDs, including preallocation
             * failures. Release that attempt only after checking the
             * actual SCB and NDP tables, without modifying either. */
            if (native_find(nan, bss, input + 20))
                return -16;
            void *ndp = PTR(nan, 0x34);
            for (unsigned i = 0; i < U32(ndp, 0x30); ++i) {
                void *record = PTR(PTR(ndp, 0x10), i * 4);
                if (record &&
                    (equal((uint8_t *)record + 0x14, input + 20, 6) ||
                     (equal((uint8_t *)record + 0x0e, input + 8, 6) &&
                      U8(record, 9) == input[26])))
                    return -16;
            }
            return 0;
        }
        if (!*slot || !(*slot)->data || (*slot)->id != input[26] ||
            !equal((*slot)->nmi, input + 8, 6))
            return -30;
        if (operation == BCM_NAN_HOST_REMOVE)
            return remove_data_station(nan, slot);
        if (operation == BCM_NAN_HOST_DEAUTHORIZE)
            return deauthorize(nan, *slot);
        return authorize_data_station(nan, *slot);
    }
    if (!nan_host_active(nan))
        return -25;
    void *peer = ((void *(*)(void *, const void *, unsigned))0x37189u)(nan, input + 8, 0);
    if (!peer || !PTR(peer, 0x0c))
        return -30;
    if (*slot || U8(peer, 9) != 1 || PTR(peer, 0x38) || U32(peer, 0x34) ||
        native_find(nan, bss, input + 20) || nan_host_peer_busy(nan, input + 8))
        return -16;
    void *ndp = PTR(nan, 0x34);
    for (unsigned i = 0; i < U32(ndp, 0x30); ++i) {
        void *record = PTR(PTR(ndp, 0x10), i * 4);
        if (record && equal((uint8_t *)record + 0x14, input + 20, 6))
            return -16; /* Native cubby binding looks up peer NDI globally. */
    }
    int error = nan_data_enable(nan, bss, index);
    if (error)
        return error;
    struct station *entry = ((void *(*)(size_t))0x7146du)(sizeof(*entry));
    if (!entry)
        return -27;
    entry->ie_length = get16(input + 28);
    entry->ies = ((void *(*)(size_t))0x7146du)(entry->ie_length);
    if (!entry->ies) {
        ((void (*)(void *))0x71491u)(entry);
        return -27;
    }
    entry->record = nan_ndp_entry_allocate(PTR(nan, 0x34), input + 8, input + 26, 1);
    if (!entry->record) {
        ((void (*)(void *))0x71491u)(entry->ies);
        ((void (*)(void *))0x71491u)(entry);
        return -27;
    }
    entry->bss = bss;
    entry->peer = peer;
    entry->data = 1;
    entry->closing = 1; /* No key programming before native create completes. */
    entry->id = input[26];
    entry->flags = BCM_NAN_HOST_MFP;
    copy(entry->nmi, input + 8, 6);
    copy(entry->local, input + 14, 6);
    copy(entry->address, input + 20, 6);
    copy(entry->ies, input + 32, entry->ie_length);
    U8(entry->record, 0x0b) = index;
    copy((uint8_t *)entry->record + 0x14, entry->address, 6);
    PTR(peer, 0x38) = entry->ies;
    U32(peer, 0x34) = entry->ie_length;
    *slot = entry;
    /* Native add allocates/hash-registers the NDI SCB, invokes its NAN
     * cubby bind, installs peer PHY capabilities, and owns exactly one pin. */
    error = ((int (*)(void *, const void *, const void *))0x37589u)(
        nan, entry->local, entry->address);
    entry->scb = native_find(nan, bss, entry->address);
    if (entry->scb)
        entry->pinned = U8(entry->scb, 0x1d) != 0;
    if (error)
        return error;
    if (!station_present(nan, entry) || U8(entry->scb, 0x1d) != 1 ||
        (U8(entry->scb, 0x1c) & 3) != 3 ||
        ((void *(*)(void *, void *))0x37511u)(nan, entry->scb) != peer)
        return -30;
    U32(entry->scb, 4) |= 0x200u;
    unsigned refs = ((unsigned (*)(void *, void *, unsigned))0x376e9u)(
        nan, entry->record, 0);
    entry->ndp_ref = U8(entry->record, 0x2a) != 0;
    if (refs != 1 || !entry->ndp_ref)
        return -30;
    entry->closing = 0;
    return 0;
}

int nan_station_idle(void *nan)
{
    return nan && PTR(nan, 0x14) && *head(nan) ? -16 : 0;
}

static int packet_crypto(void *nan, const uint8_t *address, unsigned direction,
                            unsigned *iv_length, unsigned *icv_length)
{
    struct station *entry = *find(nan, nan_host_nmi(nan), address);
    if (!entry || entry->key_owned != 2 || !(entry->flags & BCM_NAN_HOST_MFP))
        return -30;
    uint8_t info[20];
    int error = key_info(nan, entry, info);
    if (error)
        return error;
    if (!(U32(entry->scb, 4) & 0x200u) ||
        !key_usable(info, entry->address, direction))
        return -30;
    *iv_length = info[16];
    *icv_length = info[17];
    return 0;
}

int nan_station_tx_crypto(void *nan, const uint8_t *address,
                             unsigned *iv_length, unsigned *icv_length)
{
    return packet_crypto(nan, address, 2, iv_length, icv_length);
}

int nan_station_rx_crypto(void *nan, const uint8_t *address)
{
    unsigned iv, icv;
    return packet_crypto(nan, address, 4, &iv, &icv);
}

void *nan_station_tx_owner(void *wlc, void *bss, void *packet)
{
    void *wrapper = PTR(wlc, 0x520);
    void *nan = wrapper ? PTR(wrapper, 4) : 0;
    const uint8_t *ethernet = PTR(packet, 8);
    if (!nan || !nan_host_active(nan) || U16(packet, 12) < 14 ||
        !U8(bss, 4) || !U8(bss, 5) || !U8(bss, 8) ||
        !equal(ethernet + 6, (uint8_t *)bss + 0x66, 6))
        return 0;
    for (struct station *s = *head(nan); s; s = s->next) {
        if (!s->data || s->bss != bss || s->closing ||
            (!(ethernet[0] & 1u) && !equal(ethernet, s->address, 6)) ||
            !station_present(nan, s) || s->key_owned != 2 ||
            (U8(s->scb, 0x1c) & 0x13) != 0x13 || !s->ndp_ref ||
            !data_record_present(nan, s) || !U8(s->record, 0x2a))
            continue;
        uint8_t info[20];
        if (key_info(nan, s, info) || !key_usable(info, s->address, 6) ||
            nan_group_data_ready(nan, bss, s->scb))
            return 0;
        if (!(ethernet[0] & 1u))
            return s->scb;
        void *group = PTR(bss, 0x38);
        return group && PTR(group, 0x0c) == bss && (U8(group, 0x10) & 1u)
            ? group : 0;
    }
    return 0;
}

__attribute__((naked)) void nan_station_tx_gate(void)
{
    /* The stock sendpkt rejects every NDI before generic SCB allocation.
     * Supply only an existing authorized NAN owner, then rejoin its native
     * key selection, scheduling, encryption, TX accounting and completion.
     * Failure uses the original packet-drop path and frees the real packet. */
    __asm__ volatile(
        "ldr r3,[r6,#0x74]\ncmp r3,#5\nbne 1f\n"
        "ldr r3,[r6,#0x78]\ncmp r3,#11\nbne 1f\n"
        "push.w {r0-r3,r12,lr}\nmov r0,r5\nmov r1,r6\nmov r2,r4\n"
        "bl nan_station_tx_owner\nmov r7,r0\npop.w {r0-r3,r12,lr}\n"
        "cmp r7,#0\nbeq 2f\nldr pc,=0x1f8813\n"
        "2: ldr pc,=0x1f87a3\n"
        "1: ldr r3,[r6,#0x74]\ncmp r3,#5\nldr pc,=0x1f8799\n");
}

void *nan_station_rx_lookup(void **wlc, const uint8_t *sender, void **bss)
{
    /* Only the three checked RX call sites pass header + 10 here. The
     * generic A2 IBSS lookup uses TA alone, so equal peer NMI/NDI addresses
     * select whichever BSS happens to be visited first. Select the actual
     * NAN receiver and frame role before duplicate, key and data handling. */
    const uint8_t *header = sender - 10;
    void *wrapper = PTR(*wlc, 0x520);
    void *nan = wrapper ? PTR(wrapper, 4) : 0;
    if (nan && nan_host_active(nan)) {
        unsigned type = get16(header) & 0x0cu;
        void *nmi = nan_host_nmi(nan);
        if (!type && nmi &&
            (equal(header + 4, (uint8_t *)nmi + 0x66, 6) ||
             (nan_host_group_address(header + 4) &&
              equal(header + 16, (uint8_t *)nmi + 0x60, 6)))) {
            *bss = nmi;
            return native_find(nan, nmi, sender);
        }
        /* BA / other unicast management frames addressed to the NDI must
         * use its SCB too. The fallback searches only by sender and can
         * return the NMI when the peer uses equal NMI/NDI addresses. Keep
         * the native management handler, key selection and replay checks;
         * only resolve the actual receiver before entering that handler. */
        if (!type || (type == 8 && !(get16(header) & 0x300u)))
            for (struct station *s = *head(nan); s; s = s->next)
                if (s->data && !s->closing && equal(s->address, sender, 6) &&
                    (equal(header + 4, s->local, 6) ||
                     (type == 8 && (header[4] & 1u) &&
                      equal(header + 16, (uint8_t *)s->bss + 0x60, 6))) &&
                    station_present(nan, s)) {
                    *bss = s->bss;
                    return s->scb;
                }
    }
    return ((void *(*)(void **, const void *, void **))0x123dc1u)(wlc, sender, bss);
}

__attribute__((naked)) static int original_rx_key(void *, void *, void *)
{
    /* The original entry loads a PC-relative stack-canary pointer before
     * its six-byte prologue. Read the unchanged original literal. */
    __asm__ volatile("ldr r3,1f\nldr r3,[r3]\n"
                     "push.w {r4-r11,lr}\nldr pc,2f\n.balign 4\n"
                     "1: .word 0x18837c\n2: .word 0x1880b7");
}

/* A2's hardware address matcher uses TA alone. When a peer's NMI equals
 * its NDI, the hardware can apply the NDI PTK to an NMI management frame.
 * Reversing that known CCMP CTR operation restores ciphertext, not trusted
 * plaintext. The original NMI key manager must still verify MIC and PN. */
static int restore_nmi_ciphertext(void *nan, struct station *owner,
                                  void *key, void *packet, void *rx)
{
    uint8_t *frame = PTR(packet, 8);
    unsigned length = U16(packet, 12), status = U16(rx, 0x14);
    if (owner->data || owner->closing || owner->key_owned != 2 ||
        !(owner->flags & BCM_NAN_HOST_MFP) || (U32(key, 12) & 8u) ||
        (status & 0x18u) != 0x18u || length < 40 || length > 2352 ||
        (get16(frame) & 0x470fu) != 0x4000u || (frame[22] & 15u) ||
        (frame[4] & 1u) || !equal(frame + 4, (uint8_t *)owner->bss + 0x66, 6) ||
        !equal(frame + 10, owner->address, 6) || frame[26] || frame[27] != 0x20)
        return 0;
    void *manager = PTR(PTR(nan, 8), 0x420);
    for (struct station *s = *head(nan); s; s = s->next) {
        if (!s->data || s->closing || s->key_owned != 2 ||
            !equal(s->address, owner->address, 6) || !station_present(nan, s))
            continue;
        uint8_t info[20];
        void *data_key = ((void *(*)(void *, void *, unsigned, unsigned, void *))
                         0x151ec1u)(manager, s->scb, 0, 0, info);
        if (!data_key || !key_usable(info, s->address, 4) || info[12] != 4 ||
            !(U32(data_key, 12) & 8u) ||
            U16(data_key, 0x1c) != ((status >> 6) & 0x7fu) ||
            !PTR(data_key, 0x24))
            continue;
        uint32_t expanded[60], counter_words[4];
        ((void (*)(void *, unsigned))0x150891u)(counter_words, sizeof(counter_words));
        uint8_t *counter = (uint8_t *)counter_words;
        counter[0] = 1; /* CCM L=2; protected management nonce priority=16. */
        counter[1] = 16;
        copy(counter + 2, frame + 10, 6);
        counter[8] = frame[31]; counter[9] = frame[30];
        counter[10] = frame[29]; counter[11] = frame[28];
        counter[12] = frame[25]; counter[13] = frame[24];
        counter[15] = 1;
        ((void (*)(void *, const void *, unsigned))0x6f7b9u)(
            expanded, (uint8_t *)PTR(data_key, 0x24) + 6, 128);
        int (*ctr)(const void *, unsigned, const void *, unsigned, const void *, void *) =
            (void *)0x865u;
        int error = ctr(expanded, 16, counter, length - 40, frame + 32, frame + 32);
        counter[15] = 0; /* CCM encrypts its MIC with counter zero. */
        if (!error)
            error = ctr(expanded, 16, counter, 8, frame + length - 8, frame + length - 8);
        for (unsigned i = 0; i < 60; ++i)
            ((volatile uint32_t *)expanded)[i] = 0;
        if (error)
            return -48;
        U16(rx, 0x14) = status & ~0x18u;
        return 0;
    }
    return 0;
}

int nan_station_rx_key(void *key, void *packet, void *rx)
{
    void *wlc = PTR(key, 0), *wrapper = PTR(wlc, 0x520);
    void *nan = wrapper ? PTR(wrapper, 4) : 0;
    struct station *owner = 0;
    if (nan && PTR(nan, 0x14))
        for (struct station *s = *head(nan); s; s = s->next)
            if (s->scb == PTR(packet, 0x2c) && station_present(nan, s)) {
                owner = s;
                break;
            }
    unsigned fc = U16(PTR(packet, 8), 0), status = U16(rx, 0x14);
    int error = owner ? restore_nmi_ciphertext(nan, owner, key, packet, rx) : 0;
    if (!error)
        error = original_rx_key(key, packet, rx);
    /* Bounded metadata only: no keys, PN or packet contents are retained. */
    if (error && owner && owner->rx_failure_reports < 4) {
        ++owner->rx_failure_reports;
        ((int (*)(const char *, ...))0xa815u)(
            "NAN RX key: data=%u fc=%x index=%u hw=%u status=%x error=%d\n",
            owner->data, fc, U16(key, 4), U16(key, 0x1c), status, error);
    }
    return error;
}

int nan_station_stop(void *nan)
{
    struct station **slot = head(nan);
    while (*slot) {
        int error = remove_station(nan, slot);
        if (error)
            return error;
    }
    return 0;
}

/* Group selectors never manufacture SCBs or use an unowned native peer. */
int nan_station_group_owner(void *nan, void *bss, const uint8_t *address, void **scb)
{
    struct station *entry = *find(nan, bss, address);
    if (!entry || !station_present(nan, entry))
        return -30;
    if (entry->closing || !(entry->flags & BCM_NAN_HOST_MFP))
        return -16;
    *scb = entry->scb;
    return 0;
}

int nan_station_group_busy(void *nan, void *bss, void *scb)
{
    /* Only local protected group TX uses this BSS's transmit IGTK.
     * Public discovery and unicast NDP traffic cannot reference a new
     * receive IGTK/GTK and must not prevent the NDP key exchange. Native
     * beacon references are checked separately by the group-key owner. */
    if (!scb && nan_host_group_busy(nan, bss))
        return -16;
    for (struct station *entry = *head(nan); entry; entry = entry->next) {
        if (entry->bss != bss || (scb && entry->scb != scb))
            continue;
        if (!station_present(nan, entry))
            return -30;
        if (U8(entry->scb, 0x1d) != 1 ||
            (entry->data && (U8(entry->scb, 0x1c) & 0x10)))
            return -16;
    }
    return 0;
}

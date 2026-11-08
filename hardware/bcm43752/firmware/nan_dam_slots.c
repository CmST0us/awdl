/* Single-radio A2 DAM timeline updates. Native allocation/copy/marking uses
 * four-byte auxiliary cells, not the reference firmware's eight-byte cells.
 * Keep A2 0x206e8/0x210f0 reference flags in byte 1 bits 6/7. The restored
 * updater owns its conditional/committed reference bytes; no static owners.
 */
#include <stdint.h>
#include "nan_dam.h"

#define PTR(base, offset) (*(void **)((uint8_t *)(base) + (offset)))
#define U8(base, offset) (*(uint8_t *)((uint8_t *)(base) + (offset)))
#define U16(base, offset) (*(uint16_t *)((uint8_t *)(base) + (offset)))

struct slot {
    uint8_t flags, nss;
    uint16_t channel;
};

struct slot_refs {
    uint8_t conditional, native_flags, committed, reserved;
};

_Static_assert(sizeof(struct slot) == 4, "A2 availability slot");
_Static_assert(sizeof(struct slot_refs) == 4, "A2 DAM auxiliary slot");

/* Return one when the availability entry changes, zero for reference-only
 * work or a conflicting channel, and a real error before publishing either
 * record. Operation numbers come from the native 0x20114/0x21bb8 callers. */
static int update_slot(struct slot *entry, struct slot_refs *refs,
                       unsigned channel, unsigned flags, unsigned operation)
{
    if (operation == 0) {
        if (refs->committed || refs->conditional)
            return -16;
        refs->committed = !!(flags & 1u);
        refs->conditional = !!((flags & 1u) && (flags & 0x10u));
    } else if (operation == 1) {
        if (refs->committed || (channel == 0xff && (refs->native_flags & 0x80)))
            return 0;
    } else {
        if ((entry->flags & 1u) &&
            !((int (*)(unsigned, unsigned))0xbd71u)(entry->channel, channel))
            return 0;
        if (operation == 2) {
            if (refs->committed == 255 ||
                ((flags & 0x10u) && refs->conditional == 255))
                return -16;
            refs->committed++;
            if (flags & 0x10u)
                refs->conditional++;
            flags |= entry->flags & ~0x20u;
        } else {
            if (entry->flags & 1u) {
                if (!refs->committed)
                    return -30;
                refs->committed--;
            }
            if (flags & 0x10u) {
                if (!refs->conditional)
                    return -30;
                if (!--refs->conditional) {
                    flags &= ~0x10u;
                    entry->flags &= ~0x10u;
                }
            }
            if (refs->committed)
                return 0;
            channel = 0xff;
        }
    }
    entry->flags = flags;
    entry->channel = channel;
    return 1;
}

static int normalize_channel(void *dam, unsigned *channel, unsigned committed)
{
    if ((*channel & 0xff00u) == 0xff00u)
        return (*channel >= 0xff01 && *channel <= 0xff03) ? 0 : -2;
    if (!*channel || *channel == 0xff ||
        ((*channel & 0xc000u) != 0 && (*channel & 0xc000u) != 0xc000u))
        return -2;
    if (committed && !nan_dam_valid_chanspec(dam, 0, *channel, 1))
        return -20;
    struct slot entry;
    entry.channel = *channel;
    /* Actual entry is 0x21420, including the initial channel-band test. */
    ((void (*)(void *, void *))0x21421u)(PTR(dam, 4), &entry);
    *channel = entry.channel;
    return *channel && *channel != 0xff && (*channel & 0xff00u) != 0xff00u ? 0 : -20;
}

int nan_dam_update_schedule(void *dam, unsigned radio, unsigned first,
                            unsigned last, unsigned channel, unsigned flags,
                            unsigned operation, unsigned commit)
{
    if (!dam || !PTR(dam, 4) || radio || U8(dam, 0x10) != 1 ||
        !PTR(dam, 0x18) || channel > 0xffff || flags > 255 ||
        operation > 3 || commit > 1 || (operation == 2 && !(flags & 1u)) ||
        (operation != 0 && operation != 2 && (flags & 1u)) ||
        ((flags & 1u) && (channel == 0xff || (channel & 0xff00u) == 0xff00u)))
        return -2;
    void *container = PTR(dam, 0x18);
    void *grid = PTR(container, 8);
    unsigned slots = U16(container, 0), period = U16(container, 2);
    if (!grid || !slots || slots > 512 || first > last || last >= slots ||
        U8(container, 4) != 16 || period != slots * 16 ||
        U16(grid, 0) != slots || U16(grid, 2) != period || U8(grid, 4) != 16 ||
        U16(dam, 0x12) != period || U16(dam, 0x14) != 16)
        return -2;
    /* This adapter supports one physical NAN timeline. An infrastructure
     * BSS owner is outside the permitted operating mode, not a free slot. */
    if (U8(container, 0x0e) != 0xff)
        return -23;
    void *nan = PTR(dam, 4), *mac = PTR(nan, 0x14);
    if (!mac || !PTR(mac, 8) || !PTR(PTR(mac, 8), 0) ||
        !PTR(PTR(PTR(mac, 8), 0), 0))
        return -25;
    void *bss = PTR(PTR(PTR(mac, 8), 0), 0);
    if (!PTR(bss, 0) || !PTR(PTR(bss, 0), 0x2c0))
        return -25;
    unsigned nss = ((unsigned (*)(void *, unsigned))0x2f00du)(mac, 0);
    if (!nss || nss > 2)
        return -2;
    unsigned reference = ((unsigned (*)(void *, unsigned))0x20ee9u)(dam, 0);
    unsigned fallback = ((unsigned (*)(void *, unsigned))0x20c95u)(dam, 0);
    if (reference == 0xff)
        reference = fallback;
    unsigned selected = channel;
    int error = normalize_channel(dam, &reference, 0);
    if (!error)
        error = normalize_channel(dam, &fallback, 0);
    /* Release must work even after a channel becomes prohibited. Only a
     * new committed selection is subject to the current CLM decision.
     * Potential entries do not authorize transmission on their channel. */
    if (!error && operation != 3 && channel != 0xff)
        error = normalize_channel(dam, &selected, flags & 1u);
    if (error)
        return error;
    struct slot *entries = (struct slot *)((uint8_t *)grid + 8);
    struct slot_refs *references = (struct slot_refs *)((uint8_t *)container + 16);
    /* Validate the whole range first, including count exhaustion. Native
     * channel decisions are read-only; each slot has independent counters. */
    for (unsigned pass = 0; pass < 2; pass++) {
        for (unsigned i = first; i <= last; i++) {
            struct slot entry = entries[i];
            struct slot_refs refs = references[i];
            int changed = update_slot(&entry, &refs, channel, flags, operation);
            if (changed < 0)
                return changed;
            if (!pass)
                continue;
            if (changed) {
                entry.nss = (entry.nss & 0xf0u) | nss;
                if (entry.channel == 0xff) {
                    entry.flags = 0x20;
                    entry.channel = refs.native_flags & 0x40 ? reference : fallback;
                } else {
                    entry.channel = selected;
                }
            }
            entries[i] = entry;
            references[i] = refs;
        }
    }
    /* Preserve the actual grid/reference state on an attribute/MSCH failure;
     * the native caller still owns the peer and can release its references.
     * Do not manufacture a successful commit or free a live scheduler. */
    return ((int (*)(void *, unsigned))0x21771u)(dam, commit);
}

static int coexist(unsigned a, unsigned b)
{
    return ((int (*)(unsigned, unsigned))0xbd71u)(a, b);
}

static int restore_slot(void *container, const struct slot *local,
                        const struct slot *peer, uint8_t *native_flags)
{
    if ((peer->flags & 0x41u) != 1)
        return 0;
    unsigned state = U8(container, 0x0f);
    unsigned reference = U16(container, 0x0c);
    int usable;
    if (state == 1 || state == 0x10) {
        usable = (local->flags & 0x20u) ||
            ((local->flags & 1u) && coexist(peer->channel, local->channel));
        if (usable && (*native_flags & 0x40))
            usable = reference != 0xff && (reference & 0xff00u) != 0xff00u &&
                     coexist(peer->channel, reference);
    } else if (state & 0x11u) {
        *native_flags |= 0x80;
        return 0;
    } else if ((*native_flags & 0x40) && state == 4) {
        usable = reference != 0xff && (reference & 0xff00u) != 0xff00u &&
                 coexist(peer->channel, reference);
    } else {
        usable = (local->flags & 0x20u) ||
            ((local->flags & 1u) && coexist(peer->channel, local->channel));
    }
    *native_flags &= ~0x80u;
    return usable;
}

int nan_dam_update_peer_ndl_slot(void *dam, void *peer, unsigned radio,
                               unsigned index, unsigned operation, unsigned apply)
{
    if (!dam || !peer || !PTR(dam, 4) || radio || U8(dam, 0x10) != 1 ||
        !PTR(dam, 0x18) || operation < 2 || operation > 6 || apply > 1)
        return -2;
    void *nan = PTR(dam, 4), *container = PTR(dam, 0x18);
    void *local = PTR(container, 8);
    void *grid = ((void *(*)(void *, void *, unsigned))0x30bd9u)(
        nan, PTR(peer, 0x4c), radio);
    if (!grid)
        return 0;
    unsigned slots = U16(container, 0), period = U16(container, 2);
    if (!local || !slots || slots > 512 || index >= slots || period != slots * 16 ||
        U16(local, 0) != slots || U16(grid, 0) != slots ||
        U16(local, 2) != period || U16(grid, 2) != period ||
        U8(container, 4) != 16 || U8(local, 4) != 16 || U8(grid, 4) != 16 ||
        U16(dam, 0x12) != period || U16(dam, 0x14) != 16)
        return -2;
    if (U8(container, 0x0e) != 0xff)
        return -23;
    struct slot *entry = (struct slot *)((uint8_t *)grid + 8) + index;
    struct slot *timeline = (struct slot *)((uint8_t *)local + 8) + index;
    struct slot_refs *refs = (struct slot_refs *)((uint8_t *)container + 16) + index;
    struct slot before = *timeline;
    struct slot_refs old_refs = *refs;
    uint8_t native_flags = refs->native_flags;
    unsigned update = 3, done = 0, invalidate = 0;
    if (operation == 2) {
        if (((int (*)(void *))0x20845u)(dam))
            return 0; /* An in-use proposal owns the competing update. */
        if (!restore_slot(container, timeline, entry, &native_flags)) {
            refs->native_flags = native_flags;
            return 0;
        }
        update = 2;
        done = 1;
    } else if (operation == 3) {
        if ((entry->flags & 0x41u) != 0x41)
            return 0;
    } else {
        if (!(entry->flags & 1u))
            return 0;
        unsigned channel = entry->channel;
        if (operation == 4) {
            if (!PTR(dam, 0x44))
                return -25;
            if ((channel & 0xff00u) == 0xff00u || (channel & 0xc000u) ||
                nan_dam_compatible_with_bt(dam, channel))
                return 0;
        } else if (operation == 5) {
            if ((channel & 0xc000u) != 0xc000u)
                return 0;
            if (!(entry->flags & 0x40u)) {
                entry->flags &= ~1u;
                return 0;
            }
        } else {
            if (!PTR(dam, 0x28))
                return -25;
            if (!((int (*)(void *, unsigned, void *))0x3180du)(
                    nan, channel, PTR(dam, 0x28)))
                return 0;
        }
        if (operation != 5)
            apply = !!(entry->flags & 0x40u);
        invalidate = 1;
    }
    unsigned usage = U8(peer, 0x68) + U8(peer, 0x69);
    if (usage > 255)
        return -30; /* The native uint8_t sum must not wrap. */
    usage = ((unsigned (*)(void *, void *))0x37555u)(nan, peer);
    /* The native range updater treats a conflicting channel as unchanged
     * success. That is useful when combining a proposal, but cannot prove
     * that this peer acquired/released its references. Never publish a peer
     * completion bit for a skipped update, including fixed-reference slots. */
    if (apply && usage) {
        if (update == 2) {
            if ((timeline->flags & 1u) &&
                !coexist(timeline->channel, entry->channel))
                return -16;
        } else if (!(timeline->flags & 1u) ||
                   !coexist(timeline->channel, entry->channel)) {
            return -30;
        }
    }
    unsigned flags = update == 2 ? entry->flags & 0x11u : (entry->flags & 0x10u) | 0x20u;
    int error = 0;
    if (apply) {
        for (unsigned i = 0; i < usage && !error; i++)
            error = nan_dam_update_schedule(dam, radio, index, index,
                                            entry->channel, flags, update, 0);
    }
    if (!error && operation == 3)
        error = nan_dam_update_schedule(dam, radio, index, index,
                                        entry->channel, 0x20, 1, 0);
    if (error) {
        /* No commit was requested: only these two owned records changed.
         * Restore them exactly before returning a failed multi-use update;
         * peer applied/committed flags are not changed until success. */
        *timeline = before;
        *refs = old_refs;
        return error;
    }
    if (operation == 3)
        native_flags |= 0x80;
    refs->native_flags = native_flags;
    if (invalidate)
        entry->flags &= ~1u;
    ((void (*)(void *, unsigned, unsigned))0x2111du)(grid, index, done);
    return 1;
}

int nan_dam_update_peer_ndl_slots(void *dam, void *peer, unsigned radio,
                                unsigned first, unsigned last,
                                unsigned operation, unsigned apply)
{
    if (!dam || !peer || radio || U8(dam, 0x10) != 1 || !PTR(dam, 0x18) ||
        U16(dam, 0x14) != 16 || !U16(dam, 0x12) ||
        U16(dam, 0x12) % 16 || U16(dam, 0x12) > 8192 ||
        first >= 32 || last < first || last >= 64 ||
        operation < 2 || operation > 6 || apply > 1)
        return -2;
    unsigned slots = U16(dam, 0x12) / 16;
    int changed = 0;
    /* Repeat the requested discovery-window phase through the whole period.
     * A wrapping interval includes slot zero even when last % 32 is zero;
     * overlapping halves must not apply the same reference twice. */
    for (unsigned index = 0; index < slots; index++) {
        unsigned phase = index % 32;
        if (!((phase >= first && phase <= last) ||
              (last >= 32 && phase <= last % 32)))
            continue;
        int result = nan_dam_update_peer_ndl_slot(dam, peer, radio, index,
                                                 operation, apply);
        if (result < 0)
            return result;
        changed += !!result;
    }
    /* On a later-slot failure, earlier slots retain their real completion
     * bits and references. Retrying skips those already completed slots. */
    return changed;
}

int nan_dam_update_for_excursion(void *dam, void *peer, unsigned radio,
                               unsigned operation, unsigned apply)
{
    if (!dam || !peer || radio || U8(dam, 0x10) != 1 || !PTR(dam, 0x18) ||
        U16(dam, 0x14) != 16 || !U16(dam, 0x12) ||
        U16(dam, 0x12) % 16 || U16(dam, 0x12) > 8192 ||
        operation < 2 || operation > 6 || apply > 1)
        return -2;
    unsigned state = U8(PTR(dam, 0x18), 0x0f);
    if (state == 1 || (state & 0x10u))
        return nan_dam_update_peer_ndl_slots(dam, peer, radio, 0, 31, operation, apply);
    unsigned fast = U16(dam, 0x40);
    if (!fast || fast % 16 || fast > 512)
        return -2;
    unsigned width = fast / 16;
    int changed = 0;
    /* A2 0x2149c starts at phase zero on its single physical radio. The
     * reference firmware's dual-radio leading interval is not A2 layout. */
    for (unsigned start = width; start < 32; start += 2 * width) {
        unsigned end = start + width - 1;
        if (end >= 32)
            end = 31;
        int result = nan_dam_update_peer_ndl_slots(dam, peer, radio, start, end,
                                                  operation, apply);
        if (result < 0)
            return result;
        changed += result;
    }
    return changed;
}

int nan_dam_watchdog_restore_slots(void *dam, void *peer)
{
    if (!dam || !peer || U8(dam, 0x10) != 1 || !PTR(dam, 0x18))
        return -2;
    void *container = PTR(dam, 0x18);
    if (U8(container, 0x0e) != 0xff)
        return -23;
    if (U8(container, 0x0f))
        return 0; /* The active excursion owns this timeline. */
    int changed = nan_dam_update_peer_ndl_slots(dam, peer, 0, 0, 31, 2, 1);
    return changed < 0 ? changed : !!changed;
}

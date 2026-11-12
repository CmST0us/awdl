#include "native_calls.h"
/* A2 DAM proposal -> native NDC ownership. The native encoder/lookup at
 * 0x32560 creates one reference for a new table entry, but merely returns
 * an existing entry on a match. A second peer must acquire its own reference.
 * The proposal's attribute pointer is borrowed only during this call.
 */
#include <stdint.h>
#include "nan_dam.h"

#define PTR(base, offset) (*(void **)((uint8_t *)(base) + (offset)))
#define U8(base, offset) (*(uint8_t *)((uint8_t *)(base) + (offset)))
#define U16(base, offset) (*(uint16_t *)((uint8_t *)(base) + (offset)))

static int entry_index(void *table, unsigned count, void *entry)
{
    uintptr_t offset = (uintptr_t)entry - (uintptr_t)table;
    if (offset >= count * 16u || (offset & 15u))
        return -1;
    return (int)(offset / 16u);
}

int nan_dam_update_ndc_entry(void *dam, void *peer, int copy_peer_attribute)
{
    if (!dam || !peer || !PTR(dam, 4) || !PTR(dam, 0x38))
        return -2;
    void *nan = PTR(dam, 4);
    void *proposal = PTR(dam, 0x38);
    void *availability = PTR(nan, 0x4c);
    if (!availability || !PTR(availability, 0x0c) ||
        U16(availability, 0x12) != 16 || !U8(availability, 0x15) ||
        U8(availability, 0x15) > 16)
        return -2; /* A2 DAM participation bitmap is 16 bits. */
    if (proposal == PTR(peer, 0x0c) || PTR(proposal, 8) || PTR(proposal, 0x0c))
        return -16; /* Do not overwrite another attribute/reference owner. */
    void *table = PTR(availability, 0x0c);
    unsigned count = U8(availability, 0x15), active = 0;
    for (unsigned i = 0; i < count; ++i) {
        void *entry = (uint8_t *)table + i * 16;
        if (U16(entry, 0x0a) & 1u) {
            if (!PTR(entry, 0) || !U8(entry, 0x0c) || U8(entry, 0x0e) != i)
                return -30;
            active |= 1u << i;
        } else if (U8(entry, 0x0c)) {
            return -30;
        }
    }
    void *old = PTR(peer, 0x5c);
    int index = old ? entry_index(table, count, old) : -1;
    if (old && (index < 0 || !(active & (1u << index))))
        return -30;
    if (copy_peer_attribute && PTR(peer, 0x0c))
        PTR(proposal, 8) = PTR(PTR(peer, 0x0c), 8);
    int error = ((int (*)(void *, void *, unsigned))native_0x32561)(
        nan, proposal, old ? (unsigned)index : 0xffu);
    PTR(proposal, 8) = 0; /* End the temporary borrow, including on failure. */
    if (error)
        return error;
    void *entry = PTR(proposal, 0x0c);
    if (!entry)
        return -1; /* No NDC slots is not a successful NDC assignment. */
    index = entry_index(table, count, entry);
    if (index < 0 || !(U16(entry, 0x0a) & 1u) || !U8(entry, 0x0c))
        return -30; /* Retain the proposal's actual result for cleanup. */
    if (entry != old && (active & (1u << index))) {
        if (U8(entry, 0x0c) == 0xff) {
            PTR(proposal, 0x0c) = 0; /* Lookup returned a borrow, not an owner. */
            return -16;
        }
        ++U8(entry, 0x0c);
    }
    if (old && old != entry) {
        error = ((int (*)(void *, void *))native_0x31cfd)(nan, old);
        if (error)
            return error; /* Proposal retains the new reference on failure. */
    }
    PTR(peer, 0x5c) = entry;
    PTR(proposal, 0x0c) = 0; /* Transfer the reference, never release it twice. */
    return 0;
}

/* A2 common vendor sub-attributes consumed by ROM 0x1e5a4. Each entry has
 * a u16 ID/pad and two Thumb callbacks (write, measure), total 12 bytes.
 * These A2 callbacks measure when output is NULL and otherwise emit bytes.
 * B0-only proprietary FTM/version entries are not A2 capabilities.
 */
#include <stddef.h>
#include <stdint.h>

typedef uint16_t (*vendor_builder)(void *nan, void *frame, uint8_t *output);
struct vendor_attribute {
    uint16_t id;
    uint16_t reserved;
    vendor_builder write;
    vendor_builder measure;
};
_Static_assert(sizeof(struct vendor_attribute) == 12, "A2 vendor entry ABI");

#define ATTRIBUTE(id_, address) \
    { (id_), 0, (vendor_builder)((address) | 1u), (vendor_builder)((address) | 1u) }
static const struct vendor_attribute attributes[] = {
    ATTRIBUTE(0, 0x1e218), /* Native BSS flags, four bytes. */
    ATTRIBUTE(2, 0x1e278), /* Common vendor payload, filtered by frame mask. */
    ATTRIBUTE(3, 0x1e1f4), /* Per-frame vendor payload. */
};

const struct vendor_attribute *nan_vendor_attributes(void)
{
    return attributes;
}

size_t nan_vendor_attribute_count(void)
{
    return sizeof(attributes) / sizeof(attributes[0]);
}

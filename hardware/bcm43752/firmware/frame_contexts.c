/* BCM43752 A2 ROM callback replacements, compiled freestanding in Thumb mode.
 * No host ABI, firmware module registration, or radio mode changes here.
 * ROM allocation is zero-filled; every component is reached through offsets.
 * See PATCHING.md for the checked A2 consumers and remaining dependencies.
 */
#include <stddef.h>
#include <stdint.h>

struct frame_header {
    uint16_t packet, build, specific, end, reserved, bytes;
};

struct packet_context {
    uint32_t completion, owner, frame, packet, type;
    uint8_t reserved[12];
};

struct build_context {
    uint8_t destination[6], source[6], cluster[6];
    uint16_t length;
    uint32_t attributes;
};

struct discovery_frame {
    struct frame_header header;
    struct packet_context packet;
    struct build_context build;
    uint32_t attributes, service, peer;
    uint8_t instance, flags, reserved[2];
};

struct mac_frame {
    struct frame_header header;
    struct packet_context packet;
    struct build_context build;
    uint16_t subtype, reserved;
    uint32_t attributes;
};

_Static_assert(sizeof(struct packet_context) == 32, "packet context ABI");
_Static_assert(sizeof(struct build_context) == 24, "build context ABI");
_Static_assert(sizeof(struct discovery_frame) == 84, "discovery frame ABI");
_Static_assert(sizeof(struct mac_frame) == 76, "MAC frame ABI");

uint32_t nan_disc_frm_ctx_sz_get(void)
{
    return sizeof(struct discovery_frame);
}

void nan_disc_frm_ctx_init(struct discovery_frame *frame, uint16_t bytes)
{
    frame->header.packet = offsetof(struct discovery_frame, packet);
    frame->header.build = offsetof(struct discovery_frame, build);
    frame->header.specific = offsetof(struct discovery_frame, attributes);
    frame->header.end = sizeof(*frame);
    frame->header.reserved = 0;
    frame->header.bytes = bytes;
}

uint32_t nan_mac_frm_ctx_sz_get(void)
{
    return sizeof(struct mac_frame);
}

void nan_mac_frm_ctx_init(struct mac_frame *frame, uint16_t bytes)
{
    frame->header.packet = offsetof(struct mac_frame, packet);
    frame->header.build = offsetof(struct mac_frame, build);
    frame->header.specific = offsetof(struct mac_frame, subtype);
    frame->header.end = sizeof(*frame);
    frame->header.reserved = 0;
    frame->header.bytes = bytes;
}

/* A2 ROM 0x28dac writes the fixed prefix through 0x4d, places the
 * implementation's variable area at 0x4e, and obtains its requested extent
 * from the FSM context at +0x24 (copied by ROM 0x28c90 from its descriptor).
 * Keep two padding bytes in
 * the allocation, without changing the ROM's offsets or its ownership.
 */
uint32_t nan_fsm_frm_ctx_sz_get(const void *context)
{
    const uint8_t *bytes = context;
    uint16_t extent = *(const uint16_t *)(bytes + 0x24);
    return 0x50u + extent;
}

/* A2 0x28a0c allocates a 0x1c-byte result header, followed by this cubby.
 * It partitions the allocation at 0x1c, 0x24, 0xd0, 0xd8 and 0xe0;
 * the final record occupies 0x14 bytes. 0x28fb8 clears only the cubby,
 * retaining the header's pointers. The complete allocation is 0xf4 bytes.
 */
uint32_t nan_fsm_res_ctx_cubby_sz_get(void)
{
    return 0xd8u;
}

/* A2 0x26a1c consumes eight-byte frame/event entries; 0x2860c consumes
 * two-byte frame/attribute-type entries. Frame subtypes 5..13 are the
 * NDP request/response/confirm/security/terminate and schedule exchange.
 * Attribute types are the low nibble checked by A2 0x33f24, not NAN
 * attribute IDs. Unsupported frame subtypes retain the ROM lookup's
 * default event 0 / attribute type 4.
 */
struct ndpe_frame_event {
    uint8_t subtype, reserved[3];
    uint32_t event;
};
struct ndpe_frame_attribute {
    uint8_t subtype, type;
};
_Static_assert(sizeof(struct ndpe_frame_event) == 8, "A2 frame/event stride");
_Static_assert(sizeof(struct ndpe_frame_attribute) == 2, "A2 frame/type stride");

static const struct ndpe_frame_event ndpe_events[] = {
    {5, {0}, 4}, {6, {0}, 5}, {7, {0}, 6},
    {8, {0}, 7}, {9, {0}, 8}, {10, {0}, 9},
    {11, {0}, 10}, {12, {0}, 11}, {13, {0}, 12},
};
static const struct ndpe_frame_attribute ndpe_attributes[] = {
    {5, 0}, {6, 1}, {7, 2}, {8, 3}, {9, 4},
    {10, 0}, {11, 1}, {12, 2}, {13, 2},
};

const struct ndpe_frame_event *nan_ndpe_frame_events(void)
{
    return ndpe_events;
}

uint32_t nan_ndpe_frame_event_count(void)
{
    return sizeof(ndpe_events) / sizeof(ndpe_events[0]);
}

const struct ndpe_frame_attribute *nan_ndpe_frame_attributes(void)
{
    return ndpe_attributes;
}

uint32_t nan_ndpe_frame_attribute_count(void)
{
    return sizeof(ndpe_attributes) / sizeof(ndpe_attributes[0]);
}

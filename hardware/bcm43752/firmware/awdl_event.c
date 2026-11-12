#include "native_calls.h"
#include "awdl_event.h"
#include "awdl_native.h"
#include "awdl_frame.h"
#include "nan_host_abi.h"

#define PTR(base, offset) (*(void **)((uint8_t *)(base) + (offset)))

/* Share the native byte-copy implementation with the NAN patch instead of
 * spending the fixed executable reservation on per-call byte loops. */
static void copy(void *to, const void *from, unsigned bytes)
{
    ((void *(*)(void *, const void *, unsigned))native_0x151a21)(to, from, bytes);
}

static int equal(const uint8_t *a, const uint8_t *b, unsigned size)
{
    return !((int (*)(const void *, const void *, unsigned))native_0xa7a9)(a, b, size);
}

NATIVE_ENTRY static void original_action_header(
    void *, void *, const uint8_t *, const uint8_t *, unsigned,
    const void *, const void *, const void *)
{
    /* A2 ROM 0x11b284 begins with this four-byte, position-independent
     * push. Restore it and continue before mov r6,r3; stack arguments
     * remain at their native offsets. */
    __asm__ volatile("push.w {r4-r11,lr}\nb.w native_0x11b289\n");
}

void awdl_action_header(void *wlc, void *scb, const uint8_t *header,
                         const uint8_t *body, unsigned length, const void *rx,
                         const void *metadata, const void *channel_info)
{
    void *wrapper = wlc ? PTR(wlc, 0x520) : 0;
    void *nan = wrapper ? PTR(wrapper, 4) : 0;
    /* Intercept before category 127 diverges from the NAN public-action
     * branch. This is still the normal native management RX path. */
    if (nan && awdl_receive_action(nan, header, body, length, rx))
        return;
    original_action_header(wlc, scb, header, body, length, rx, metadata, channel_info);
}

int awdl_event_emit(struct awdl_session *s, unsigned kind, uint64_t timestamp,
                    uint64_t cookie, int result, uint16_t channel, unsigned flags,
                    const uint8_t *header, const uint8_t *body, unsigned body_bytes)
{
    if (!s || awdl_session_get(s->nan) != s || kind < BCM_AWDL_RX ||
        kind > BCM_AWDL_SENDER || (flags & ~BCM_AWDL_TX_ACK) ||
        (flags && kind != BCM_AWDL_TX_DONE))
        return -2;
    unsigned frame_bytes = 0, header_bytes = 0;
    if (kind == BCM_AWDL_RX) {
        if (!header || !body || body_bytes > BCM_AWDL_TX_FRAME_MAX - 24 ||
            cookie || result || !channel)
            return -2;
        frame_bytes = 24 + body_bytes;
        header_bytes = 24;
    } else if (kind == BCM_AWDL_SENDER) {
        if (!header || body || body_bytes || cookie || result || channel)
            return -2;
        frame_bytes = header_bytes = 6;
    } else if (header || body || body_bytes) {
        return -2;
    }
    if ((kind == BCM_AWDL_TX_DONE && !cookie) ||
        (kind != BCM_AWDL_TX_DONE && cookie))
        return -2;
    unsigned bytes = BCM_AWDL_EVENT_HEADER_SIZE + frame_bytes;
    uint8_t *event = ((void *(*)(unsigned))native_0x7146d)(bytes);
    if (!event)
        return -27;
    /* A2's native endian is the wire ABI endian. Encode one complete
     * aligned header, then copy bytes so the event allocation need not
     * satisfy uint64_t alignment. The size assertion excludes padding. */
    const struct {
        uint16_t version, kind, bytes, flags;
        uint64_t generation, timestamp, cookie;
        int32_t result;
        uint16_t channel, frame_bytes;
    } encoded = {BCM_AWDL_ABI_VERSION, kind, bytes, flags,
                 s->generation, timestamp, cookie, result, channel, frame_bytes};
    _Static_assert(sizeof(encoded) == BCM_AWDL_EVENT_HEADER_SIZE);
    copy(event, &encoded, sizeof(encoded));
    if (frame_bytes) {
        copy(event + BCM_AWDL_EVENT_HEADER_SIZE, header, header_bytes);
        if (body_bytes)
            copy(event + BCM_AWDL_EVENT_HEADER_SIZE + header_bytes, body, body_bytes);
    }
    /* A2 0x1fcb4 preserves r0 from 0xcf760. That routine returns -27
     * when its event object/payload allocation fails; a queued event
     * leaves -1, not zero. Only propagate the proven allocation failure,
     * rather than interpreting its other register values as errno. */
    int delivery = ((int (*)(void *, unsigned, unsigned, const void *, unsigned, unsigned))native_0x1fcb5)(
        s->nan, BCM_NAN_HOST_EVENT, BCM_AWDL_EVENT_REASON, event, bytes, 0);
    ((void (*)(void *))native_0x71491)(event);
    return delivery == -27 ? delivery : 0;
}

int awdl_receive_action(void *nan, const uint8_t *header, const uint8_t *body,
                        unsigned length, const void *rx)
{
    static const uint8_t prefix[5] = {0x7f, 0, 0x17, 0xf2, 8};
    static const uint8_t bssid[6] = BCM_AWDL_BSSID_BYTES;
    if (!header || !body || !rx || length < sizeof(prefix) ||
        length > BCM_AWDL_TX_FRAME_MAX - 24 || header[0] != 0xd0 ||
        (header[1] & 0x43) || !equal(body, prefix, sizeof(prefix)) ||
        !equal(header + 16, bssid, 6))
        return 0;
    struct awdl_session *s = awdl_session_get(nan);
    if (!s || s->stopping ||
        (!(header[4] & 1u) && !equal(header + 4, s->local, 6)))
        return 0;
    uint64_t now;
    int error = awdl_native_rx_clock(nan, rx, &now);
    if (error) {
        s->error = error;
        return 1;
    }
    unsigned channel = ((unsigned (*)(void *, const void *))native_0x119465)(PTR(nan, 8), rx);
    if (!channel || channel > 196)
        return 1;
    channel |= channel > 14 ? 0xd000u : 0x1000u;
    ++s->received;
    error = awdl_event_emit(s, BCM_AWDL_RX, now, 0, 0, channel, 0, header, body, length);
    if (error)
        s->error = error;
    /* Recognized frames have one protocol owner even if event allocation
     * fails. Never send a native rejected-action response in that case. */
    return 1;
}

void awdl_event_nan_dw(void *nan, uint16_t channel)
{
    struct awdl_session *s = awdl_session_get(nan);
    if (!s || s->stopping)
        return;
    uint64_t now;
    int error = awdl_native_clock(nan, &now);
    if (!error)
        error = awdl_event_emit(s, BCM_AWDL_NAN_DW, now, 0, 0, channel, 0, 0, 0, 0);
    if (error)
        s->error = error;
}

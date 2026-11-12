#include "native_calls.h"
#include "awdl_tx.h"
#include "awdl_event.h"
#include "awdl_native.h"
#include "awdl_schedule.h"
#include "awdl_frame.h"
#include "nan_data.h"

#define PTR(b, o) (*(void **)((uint8_t *)(b) + (o)))
#define U32(b, o) (*(uint32_t *)((uint8_t *)(b) + (o)))
#define U8(b, o) (*(uint8_t *)((uint8_t *)(b) + (o)))

enum state { WAITING, SUBMITTING, SUBMITTED, COMPLETE };
struct awdl_tx {
    struct awdl_tx *next;
    struct awdl_session *session;
    void *packet;
    uint8_t *body;
    uint64_t cookie, origin;
    unsigned length;
    uint16_t channel;
    enum state state;
    uint32_t status;
    int submit_error;
    uint8_t frame[];
};

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

static void finish(struct awdl_tx *tx, int result, unsigned flags)
{
    struct awdl_session *s = tx->session;
    struct awdl_tx **slot = (struct awdl_tx **)&s->tx;
    while (*slot && *slot != tx)
        slot = &(*slot)->next;
    if (*slot != tx)
        return;
    uint64_t now = 0;
    int error = awdl_native_clock(s->nan, &now);
    if (error)
        s->error = error;
    if (tx->frame[4] & 1)
        flags = 0;
    error = awdl_event_emit(s, BCM_AWDL_TX_DONE, now, tx->cookie, result,
                            tx->channel, flags, 0, 0, 0);
    if (error)
        s->error = error;
    *slot = tx->next;
    if (tx->state == WAITING)
        --s->queued;
    else
        --s->submitted;
    ++s->completed;
    ((void (*)(void *))native_0x71491)(tx);
}

static void completed(void *wlc, uint32_t status, struct awdl_tx *tx)
{
    if (!tx || PTR(tx->session->nan, 8) != wlc ||
        (tx->state != SUBMITTING && tx->state != SUBMITTED))
        return;
    enum state before = tx->state;
    tx->state = COMPLETE;
    tx->status = status;
    /* A synchronous PCB completion cannot free the submitting stack's tx. */
    if (before == SUBMITTED)
        finish(tx, tx->submit_error ? tx->submit_error : (int)status,
               !tx->submit_error && (status & 2u) ? BCM_AWDL_TX_ACK : 0);
}

static void submit(struct awdl_tx *tx)
{
    struct awdl_session *s = tx->session;
    void *wlc = PTR(s->nan, 8), *body = 0;
    if (nan_data_owned_bss(s->nan, s->local, 0) != s->bss) {
        finish(tx, -30, 0);
        return;
    }
    void *packet = ((void *(*)(void *, unsigned, const void *, const void *,
                               const void *, unsigned, void **))native_0x8a74d)(
        wlc, 0xd0, tx->frame + 4, tx->frame + 10, tx->frame + 16,
        tx->length - 24, &body);
    if (!packet) {
        finish(tx, -27, 0);
        return;
    }
    copy(body, tx->frame + 24, tx->length - 24);
    tx->packet = packet;
    tx->body = body;
    PTR(packet, 0x34) = packet;
    U32(packet, 0x10) = (U32(packet, 0x10) & ~7u) | 5u;
    U8(packet, 0x25) = U8(s->bss, 0x3c);
    int error = ((int (*)(void *, uintptr_t, void *, void *))native_0x102161)(
        PTR(wlc, 0x2c4), (uintptr_t)completed, tx, packet);
    if (error) {
        ((void (*)(void *, void *, unsigned))native_0x151ae1)(PTR(wlc, 4), packet, 1);
        finish(tx, error, 0);
        return;
    }
    --s->queued;
    ++s->submitted;
    tx->state = SUBMITTING;
    error = ((int (*)(void *, void *, void *, unsigned))native_0x2d6b5)(
        PTR(s->nan, 0x14), s->bss, packet, 1);
    if (tx->state == COMPLETE) {
        finish(tx, error ? error : (int)tx->status,
               !error && (tx->status & 2u) ? BCM_AWDL_TX_ACK : 0);
        return;
    }
    tx->submit_error = error;
    tx->state = SUBMITTED;
}

int awdl_tx_stamp(struct awdl_session *s, void *packet)
{
    for (struct awdl_tx *tx = s->tx; tx; tx = tx->next) {
        if (tx->packet != packet ||
            (tx->state != SUBMITTING && tx->state != SUBMITTED))
            continue;
        /* Native preparation prepends descriptors to this local allocation.
         * Its body remains owned until the registered completion consumes tx.
         * Stamp at DMA admission, including native queue waits and retries. */
        uint64_t now;
        int error = awdl_native_clock(s->nan, &now);
        if (error || now < tx->origin) {
            s->error = error ? error : -2;
            return s->error;
        }
        uint32_t physical = (uint32_t)(now - tx->origin + 1);
        uint8_t *body = tx->body;
        for (unsigned i = 0; i < 4; ++i)
            body[8 + i] = (uint8_t)(physical >> (8 * i));
        break;
    }
    return 0;
}

void awdl_tx_kick(struct awdl_session *s)
{
    if (!s || s->stopping || !s->timing.on_channel)
        return;
    uint64_t now, end;
    int error = awdl_native_clock(s->nan, &now);
    if (error) {
        s->error = error;
        return;
    }
    end = awdl_schedule_deadline(s, now);
    /* Discovery actions do not require an established peer window, but
     * they still obey the installed plan and pre-switch drain interval. */
    if (end <= now ||
        end - now <= s->timing.drain_us)
        return;
    unsigned active = s->timing.active;
    unsigned primary = (active & 255u) - 6 + 4 * ((active >> 8) & 7u);
    struct awdl_tx *tx = s->tx;
    while (tx) {
        struct awdl_tx *next = tx->next;
        if (tx->state == WAITING && (tx->channel & 255u) == primary)
            submit(tx);
        tx = next;
    }
}

int awdl_tx_enqueue(struct awdl_session *s, uint64_t cookie, uint16_t channel,
                    uint64_t origin, const uint8_t *frame, unsigned length)
{
    static const uint8_t prefix[5] = {0x7f, 0, 0x17, 0xf2, 8};
    static const uint8_t bssid[6] = BCM_AWDL_BSSID_BYTES;
    if (!s || s->stopping || !cookie || !origin || !frame || length < 36 ||
        length > BCM_AWDL_TX_FRAME_MAX || frame[0] != 0xd0 || frame[1] ||
        (frame[22] & 15u) || !equal(frame + 10, s->local, 6) ||
        !equal(frame + 16, bssid, 6) || !equal(frame + 24, prefix, 5) ||
        (channel != 0xd095 && channel != 0xd0a1))
        return -2;
    if (s->queued + s->submitted >= 16)
        return -16;
    struct awdl_tx **slot = (struct awdl_tx **)&s->tx;
    while (*slot) {
        if ((*slot)->cookie == cookie)
            return -16;
        slot = &(*slot)->next;
    }
    uint64_t now;
    int error = awdl_native_clock(s->nan, &now);
    if (error)
        return error;
    if (origin > now)
        return -2;
    struct awdl_tx *tx = ((void *(*)(unsigned))native_0x7146d)(sizeof(*tx) + length);
    if (!tx)
        return -27;
    tx->session = s;
    tx->cookie = cookie;
    tx->origin = origin;
    tx->channel = channel;
    tx->length = length;
    copy(tx->frame, frame, length);
    *slot = tx;
    ++s->queued;
    awdl_tx_kick(s);
    return 0;
}

int awdl_tx_cancel(struct awdl_session *s, uint64_t cookie)
{
    for (struct awdl_tx *tx = s->tx; tx; tx = tx->next) {
        if (tx->cookie != cookie)
            continue;
        if (tx->state != WAITING)
            return -16;
        finish(tx, -1, 0);
        return 0;
    }
    return -30;
}

int awdl_tx_drain(struct awdl_session *s)
{
    struct awdl_tx *tx = s->tx;
    while (tx) {
        struct awdl_tx *next = tx->next;
        if (tx->state == WAITING)
            finish(tx, -1, 0);
        tx = next;
    }
    return s->tx || s->queued || s->submitted ? -16 : 0;
}

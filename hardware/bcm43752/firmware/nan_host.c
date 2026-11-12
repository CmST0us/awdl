#include "native_calls.h"
/* Hash-locked A2 NMI management transport. Keep native NAN BSS, scheduler,
 * packet allocation and PCB machinery; hostap owns the negotiation frames. */
#include "nan_host.h"
#include "nan_mac.h"
#include "nan_host_abi.h"
#include "nan_station.h"
#include "nan_schedule.h"
#include "nan_group.h"
#include "awdl_event.h"

#define FIELD(type, base, offset) (*(type *)((uint8_t *)(base) + (offset)))
#define PTR(base, offset) FIELD(void *, base, offset)
#define U32(base, offset) FIELD(uint32_t, base, offset)
#define U16(base, offset) FIELD(uint16_t, base, offset)
#define U8(base, offset) FIELD(uint8_t, base, offset)

enum tx_state { TX_WAITING, TX_SUBMITTING, TX_SUBMITTED, TX_COMPLETE };
struct host_tx {
    struct host_tx *next;
    void *nan, *bss;
    uint32_t cookie_low, cookie_high;
    uint16_t channel, length;
    enum tx_state state;
    uint32_t status;
    int submit_error;
    uint8_t frame[];
};

static unsigned get16(const uint8_t *p)
{
    return p[0] | (unsigned)p[1] << 8;
}

static uint32_t get32(const uint8_t *p)
{
    return get16(p) | (uint32_t)get16(p + 2) << 16;
}

static void put16(uint8_t *p, unsigned value)
{
    p[0] = value;
    p[1] = value >> 8;
}

static void put32(uint8_t *p, uint32_t value)
{
    put16(p, value);
    put16(p + 2, value >> 16);
}

static void copy(void *to, const void *from, size_t length)
{
    ((void *(*)(void *, const void *, size_t))native_0x151a21)(to, from, length);
}

static int equal(const void *a, const void *b, size_t length)
{
    return !((int (*)(const void *, const void *, size_t))native_0xa7a9)(a, b, length);
}

void *nan_tx_packet_header(void *wlc, void *packet)
{
    void *offload, *descriptor, *header;
    /* Native A2 TX parsing accounts for offload-prefix size, descriptor
     * length and split packet headers. TX-status only calls this for packets
     * with WLF_TXHDR; do not mistake the D11 descriptor for an 802.11 header. */
    ((void (*)(void *, void *, void **, void **, void **))native_0x1f78f1)(
        wlc, packet, &offload, &descriptor, &header);
    return header;
}

static struct host_tx **owners(void *nan)
{
    return (struct host_tx **)((uint8_t *)PTR(nan, 0x14) + NAN_MAC_HOST_TX);
}

void *nan_host_nmi(void *nan)
{
    if (!nan || !PTR(nan, 0x14))
        return 0;
    void *mac = PTR(nan, 0x14);
    void *bss = PTR(mac, NAN_MAC_EVENT_BSS);
    if (!bss || U32(bss, 0x74) != 5 || U32(bss, 0x78) != 10 ||
        PTR(bss, 0) != PTR(nan, 8))
        return 0;
    return bss;
}

int nan_host_active(void *nan)
{
    void *bss = nan_host_nmi(nan);
    if (!bss || !PTR(nan, 0x10) || U8(PTR(nan, 0x10), 0x30))
        return 0;
    void *mac = PTR(nan, 0x14);
    void *wlc = PTR(nan, 8);
    return U8(bss, 5) && PTR(wlc, 0) && PTR(PTR(wlc, 0), 4) &&
        U8(PTR(PTR(wlc, 0), 4), 0x30) && PTR(mac, 0x38) &&
        U8(PTR(mac, 0x38), 0x4a) && !U32(mac, NAN_MAC_HOST_STOPPING);
}

static int discovery_channel(void *nan, unsigned channel)
{
    void *mac = PTR(nan, 0x14);
    void *config = PTR(mac, 0x20);
    void *bands = PTR(mac, 8);
    unsigned band = (channel & 0xc000u) == 0xc000u;
    /* Only the two configured 20 MHz discovery channels, with a real DW
     * registration and the same native NMI BSS. This is not an off-channel
     * request and cannot create a second radio mode. */
    if (!config || !bands || (channel & 0x3800u) != 0x1000u ||
        (channel & 0xc000u) != (band ? 0xc000u : 0) ||
        (band && !U8(config, 0x50)) ||
        channel != U16(config, 0x52 + band * 2))
        return 0;
    void *context = PTR(bands, band * 4);
    return context && PTR(context, 0) == nan_host_nmi(nan) && PTR(context, 0x0c);
}

static int tx_context_valid(struct host_tx *tx)
{
    void *bss = nan_host_nmi(tx->nan);
    return bss && bss == tx->bss && discovery_channel(tx->nan, tx->channel) &&
        equal(tx->frame + 10, (uint8_t *)bss + 0x66, 6) &&
        equal(tx->frame + 16, (uint8_t *)bss + 0x60, 6);
}

static int unicast_window_open(struct host_tx *tx)
{
    void *nan = tx->nan, *mac = PTR(nan, 0x14), *wlc = PTR(nan, 8);
    unsigned band = (tx->channel & 0xc000u) == 0xc000u;
    void *context = PTR(PTR(mac, 8), band * 4);
    if ((tx->frame[4] & 1u) || !(U32(wlc, 0x16c) & 8u) ||
        !(U8(context, 0x20) & 3u) || !PTR(tx->bss, 0x98))
        return 0;
    /* These flags are set/cleared by native NAN DW/data MSCH events.
     * Respond in an already open NAN window instead of unconditionally
     * delaying Auth2 until the next DW. Never retune or create a new mode.
     * NMI shares the active 80 MHz data channel's primary 20 MHz. */
    unsigned current = U16(PTR(tx->bss, 0x98), 0x36);
    if ((current & 0xc000u) != (tx->channel & 0xc000u))
        return 0;
    unsigned primary = current & 255u;
    if ((current & 0x3800u) == 0x2000u && ((current >> 8) & 7u) < 4)
        primary = primary - 6 + 4 * ((current >> 8) & 7u);
    else if ((current & 0x3800u) != 0x1000u)
        return 0;
    return primary == (tx->channel & 255u);
}

static void event_header(uint8_t *event, unsigned kind, unsigned frame_length,
                          uint32_t low, uint32_t high, unsigned channel,
                          unsigned flags, uint32_t status)
{
    put16(event, BCM_NAN_HOST_VERSION);
    put16(event + 2, kind);
    put16(event + 4, BCM_NAN_HOST_HEADER_SIZE + frame_length);
    put16(event + 6, frame_length);
    put32(event + 8, low);
    put32(event + 12, high);
    put16(event + 16, channel);
    put16(event + 18, flags);
    put32(event + 20, status);
}

static void send_event(void *nan, const void *data, unsigned length)
{
    /* Native event construction copies this payload before returning, as for
     * original NAN events that release their temporary buffer immediately. */
    ((void (*)(void *, unsigned, unsigned, const void *, unsigned, unsigned))native_0x1fcb5)(
        nan, BCM_NAN_HOST_EVENT, BCM_NAN_HOST_REASON, data, length, 0);
}

static void finish(struct host_tx *tx, uint32_t result, unsigned flags)
{
    void *nan = tx->nan;
    struct host_tx **slot = owners(nan);
    while (*slot && *slot != tx)
        slot = &(*slot)->next;
    if (*slot != tx)
        return;
    /* The host completion below carries the actual result and ACK state. */
    if (tx->frame[4] & 1u)
        flags &= ~BCM_NAN_HOST_ACK; /* Group TX completion is not a peer ACK. */
    uint8_t event[BCM_NAN_HOST_HEADER_SIZE];
    event_header(event, BCM_NAN_HOST_TX_STATUS, 0, tx->cookie_low,
                   tx->cookie_high, tx->channel, flags, result);
    /* Keep the owner through event construction. No deadline or failed
     * cancel may fabricate a completed PCB reference. */
    send_event(nan, event, sizeof(event));
    *slot = tx->next;
    --U32(PTR(nan, 0x14), NAN_MAC_HOST_TX_COUNT);
    ((void (*)(void *))native_0x71491)(tx);
}

void nan_host_tx_complete(void *wlc, uint32_t status, struct host_tx *tx)
{
    if (!tx || PTR(tx->nan, 8) != wlc ||
        (tx->state != TX_SUBMITTING && tx->state != TX_SUBMITTED))
        return;
    enum tx_state state = tx->state;
    tx->state = TX_COMPLETE;
    tx->status = status;
    /* sendmgmt may synchronously free a rejected packet and invoke PCB.
     * The submitting stack still owns tx until that native call returns. */
    if (state == TX_SUBMITTED)
        finish(tx, tx->submit_error ? (uint32_t)tx->submit_error : status,
               !tx->submit_error && (status & 2u) ? BCM_NAN_HOST_ACK : 0);
}

static void submit(struct host_tx *tx)
{
    void *nan = tx->nan;
    void *wlc = PTR(nan, 8);
    void *mac = PTR(nan, 0x14);
    void *body = 0;
    const uint8_t *frame = tx->frame;
    unsigned iv = 0, icv = 0;
    int protected = get16(frame) & 0x4000u;
    /* Cluster adoption or a discovery-channel change can invalidate an
     * accepted queue entry before its DW. Fail it without allocating a
     * packet, selecting a key, or consuming a packet number. */
    if (!nan_host_active(nan) || !tx_context_valid(tx)) {
        finish(tx, (uint32_t)-25, 0);
        return;
    }
    int error = !protected ? 0 : (frame[4] & 1u) ? nan_group_tx_crypto(nan, &iv, &icv) :
        nan_station_tx_crypto(nan, frame + 4, &iv, &icv);
    if (error) {
        finish(tx, (uint32_t)error, 0);
        return;
    }
    void *packet;
    if (protected) {
        /* Same allocator and IV/ICV reservation as native MFP. Its legacy
         * STA/AP policy selector does not know host-owned NMI PASN keys.
         * Native key_prep_tx_mpdu sets Protected while authenticating the
         * actual header; no payload encryption is implemented by this patch. */
        packet = ((void *(*)(void *, unsigned, const void *, const void *,
                            const void *, unsigned, void **, unsigned, unsigned))native_0x8a769)(
            wlc, get16(frame) & ~0x4000u, frame + 4, frame + 10, frame + 16,
            tx->length - 24, &body, iv, icv);
    } else {
        packet = ((void *(*)(void *, unsigned, const void *, const void *,
                               const void *, unsigned, void **))native_0x8a74d)(
            wlc, get16(frame), frame + 4, frame + 10, frame + 16,
            tx->length - 24, &body);
    }
    if (!packet) {
        finish(tx, (uint32_t)-27, 0);
        return;
    }
    copy(body, frame + 24, tx->length - 24);
    /* Preserve requested FC but let native sequencing/duration own their
     * header fields. Packet tag layout is A2-specific, not the host ABI. */
    PTR(packet, 0x34) = packet;
    U32(packet, 0x10) = (U32(packet, 0x10) & ~7u) | 5u;
    U8(packet, 0x25) = U8(tx->bss, 0x3c);
    if (protected)
        U32(packet, 0x1c) |= 0x20000000u; /* native WLF_MFP key selection */
    error = ((int (*)(void *, uintptr_t, void *, void *))native_0x102161)(
        PTR(wlc, 0x2c4), (uintptr_t)nan_host_tx_complete, tx, packet);
    if (error) {
        /* PCB registration failed, so no callback can own this packet. */
        ((void (*)(void *, void *, unsigned))native_0x151ae1)(PTR(wlc, 4), packet, 1);
        finish(tx, (uint32_t)error, 0);
        return;
    }
    tx->state = TX_SUBMITTING;
    error = ((int (*)(void *, void *, void *, unsigned))native_0x2d6b5)(
        mac, tx->bss, packet, 1);
    if (tx->state == TX_COMPLETE) {
        uint32_t status = tx->status;
        finish(tx, error ? (uint32_t)error : status,
               !error && (status & 2u) ? BCM_NAN_HOST_ACK : 0);
        return;
    }
    /* 2d6b4 returns -22 after sendmgmt has freed its packet. Keep the PCB
     * reference until its callback even if this completion is deferred.
     * Validation prevents -23 (non-management frame), the only no-free case. */
    tx->submit_error = error;
    tx->state = TX_SUBMITTED;
}

static int valid_frame(const uint8_t *frame, unsigned length)
{
    if (length < 30 || length > BCM_NAN_HOST_MAX_FRAME)
        return 0;
    unsigned fc = get16(frame);
    /* Only unfragmented management Action and Authentication. Protected
     * requests must be Protected Dual NAN actions with an owned key. */
    if ((fc & ~0x4800u) != 0x00d0 && (fc & ~0x0800u) != 0x00b0)
        return 0;
    if ((fc & 0x4000u) && frame[24] != 9)
        return 0;
    if (frame[22] & 15u || frame[10] & 1u)
        return 0;
    if ((fc & 0xf0u) == 0xb0)
        return !(frame[4] & 1u);
    if (frame[24] == 9 && !(fc & 0x4000u))
        return 0;
    return (frame[24] == 4 || frame[24] == 9) && frame[25] == 9 &&
        frame[26] == 0x50 && frame[27] == 0x6f && frame[28] == 0x9a &&
        (frame[29] == 0x13 || (frame[29] == 0x18 && length >= 31));
}

static int request_header(const uint8_t *input, size_t length, unsigned kind)
{
    return length >= BCM_NAN_HOST_HEADER_SIZE &&
        get16(input) == BCM_NAN_HOST_VERSION && get16(input + 2) == kind &&
        get16(input + 4) == length &&
        get16(input + 6) == length - BCM_NAN_HOST_HEADER_SIZE &&
        (get32(input + 8) || get32(input + 12)) &&
        !get16(input + 18) && !get32(input + 20);
}

int nan_host_validate(const struct nan_command_digest *digest, uint32_t action,
                       const uint8_t *input, size_t length,
                       uint8_t *output, size_t *output_length)
{
    int error = nan_command_validate(digest, action, input, length, output, output_length);
    if (error)
        return error;
    const struct nan_command *command = ((const struct nan_command *const *)digest)[3];
    if (command->id == BCM_NAN_HOST_TX_COMMAND)
        return request_header(input, length, BCM_NAN_HOST_TX) &&
            valid_frame(input + BCM_NAN_HOST_HEADER_SIZE,
                        length - BCM_NAN_HOST_HEADER_SIZE) ? 0 : -2;
    return request_header(input, length, BCM_NAN_HOST_CANCEL) &&
        length == BCM_NAN_HOST_HEADER_SIZE && !get16(input + 16) ? 0 : -2;
}

int nan_host_tx_command(const struct nan_command_digest *digest,
                         const uint8_t *input, size_t length,
                         uint8_t *output, size_t *output_length)
{
    (void)output;
    *output_length = 0;
    if (!request_header(input, length, BCM_NAN_HOST_TX) ||
        !valid_frame(input + BCM_NAN_HOST_HEADER_SIZE,
                    length - BCM_NAN_HOST_HEADER_SIZE))
        return -2;
    void *nan = ((void *const *)digest)[1];
    if (!nan_host_active(nan))
        return -25;
    void *mac = PTR(nan, 0x14);
    void *bss = nan_host_nmi(nan);
    const uint8_t *frame = input + BCM_NAN_HOST_HEADER_SIZE;
    /* This is the NMI transport. NDI addresses and independent off-channel
     * operations must not borrow this BSS or change its discovery schedule. */
    unsigned channel = get16(input + 16);
    if (!equal(frame + 10, (uint8_t *)bss + 0x66, 6) ||
        !equal(frame + 16, (uint8_t *)bss + 0x60, 6) ||
        !discovery_channel(nan, channel))
        return -2;
    if (get16(frame) & 0x4000u) {
        unsigned iv, icv;
        int error = (frame[4] & 1u) ? nan_group_tx_crypto(nan, &iv, &icv) :
            nan_station_tx_crypto(nan, frame + 4, &iv, &icv);
        if (error)
            return error;
    }
    if (U32(mac, NAN_MAC_HOST_TX_COUNT) >= BCM_NAN_HOST_TX_LIMIT)
        return -16;
    struct host_tx **slot = owners(nan);
    while (*slot) {
        if ((*slot)->cookie_low == get32(input + 8) &&
            (*slot)->cookie_high == get32(input + 12))
            return -16;
        slot = &(*slot)->next;
    }
    unsigned frame_length = length - BCM_NAN_HOST_HEADER_SIZE;
    struct host_tx *tx = ((void *(*)(size_t))native_0x7146d)(sizeof(*tx) + frame_length);
    if (!tx)
        return -27;
    tx->nan = nan;
    tx->bss = bss;
    tx->cookie_low = get32(input + 8);
    tx->cookie_high = get32(input + 12);
    tx->channel = channel;
    tx->length = frame_length;
    tx->state = TX_WAITING;
    copy(tx->frame, frame, frame_length);
    *slot = tx;
    ++U32(mac, NAN_MAC_HOST_TX_COUNT);
    if (unicast_window_open(tx))
        submit(tx);
    return 0;
}

int nan_host_cancel_command(const struct nan_command_digest *digest,
                             const uint8_t *input, size_t length,
                             uint8_t *output, size_t *output_length)
{
    (void)output;
    *output_length = 0;
    if (!request_header(input, length, BCM_NAN_HOST_CANCEL) ||
        length != BCM_NAN_HOST_HEADER_SIZE || get16(input + 16))
        return -2;
    void *nan = ((void *const *)digest)[1];
    for (struct host_tx *tx = *owners(nan); tx; tx = tx->next) {
        if (tx->cookie_low != get32(input + 8) || tx->cookie_high != get32(input + 12))
            continue;
        if (tx->state != TX_WAITING)
            return -16;
        finish(tx, (uint32_t)-1, 0);
        return 0;
    }
    return -30;
}

int nan_host_idle(void *nan)
{
    if (!nan || !PTR(nan, 0x14))
        return 0;
    void *mac = PTR(nan, 0x14);
    return PTR(mac, NAN_MAC_HOST_TX) || U32(mac, NAN_MAC_HOST_TX_COUNT) ?
        -16 : nan_station_idle(nan) ? -16 : nan_group_idle(nan, 0, 0);
}

int nan_host_peer_busy(void *nan, const uint8_t *address)
{
    for (struct host_tx *tx = *owners(nan); tx; tx = tx->next)
        if (equal(tx->frame + 4, address, 6))
            return -16;
    return 0;
}

int nan_host_peer_protected_busy(void *nan, const uint8_t *address)
{
    for (struct host_tx *tx = *owners(nan); tx; tx = tx->next)
        if ((get16(tx->frame) & 0x4000u) && equal(tx->frame + 4, address, 6))
            return -16;
    return 0;
}

int nan_host_group_busy(void *nan, void *bss)
{
    for (struct host_tx *tx = *owners(nan); tx; tx = tx->next)
        if (tx->bss == bss && (tx->frame[4] & 1u) && (get16(tx->frame) & 0x4000u))
            return -16;
    return 0;
}

int nan_host_stop(void *nan)
{
    void *mac = PTR(nan, 0x14);
    U32(mac, NAN_MAC_HOST_STOPPING) = 1;
    int error = nan_station_quiesce(nan, 0, 0);
    if (error)
        return error;
    struct host_tx *tx = *owners(nan);
    while (tx) {
        struct host_tx *next = tx->next;
        if (tx->state == TX_WAITING)
            finish(tx, (uint32_t)-1, 0);
        tx = next;
    }
    if (PTR(mac, NAN_MAC_HOST_TX) || U32(mac, NAN_MAC_HOST_TX_COUNT))
        return -16;
    error = nan_group_stop(nan);
    if (!error)
        error = nan_station_stop(nan);
    return error ? error : nan_schedule_stop(nan);
}

void nan_host_dw_start(void *wlc, void *mac, void *bss, void *runtime,
                        void *event, unsigned band, unsigned flags)
{
    ((void (*)(void *, void *, void *, void *, void *, unsigned, unsigned))native_0x2b729)(
        wlc, mac, bss, runtime, event, band, flags);
    void *nan = PTR(mac, 4);
    if (band > 1 || !nan_host_active(nan) || nan_host_nmi(nan) != bss ||
        !PTR(bss, 0x98))
        return;
    unsigned channel = U16(event, 4);
    if (band != ((channel & 0xc000u) == 0xc000u) ||
        !discovery_channel(nan, channel) || channel != U16(PTR(bss, 0x98), 0x36))
        return;
    awdl_event_nan_dw(nan, (uint16_t)channel);
    uint8_t notification[BCM_NAN_HOST_HEADER_SIZE];
    event_header(notification, BCM_NAN_HOST_DW_START, 0, 0, 0, channel, 0, 0);
    send_event(nan, notification, sizeof(notification));
    struct host_tx *tx = *owners(nan);
    while (tx) {
        struct host_tx *next = tx->next;
        if (tx->state == TX_WAITING) {
            if (!tx_context_valid(tx))
                finish(tx, (uint32_t)-25, 0);
            else if (tx->channel == channel)
                submit(tx);
        }
        tx = next;
    }
}

static void host_receive(void *nan, const uint8_t *header, const uint8_t *body,
                           unsigned length, unsigned channel, unsigned flags)
{
    unsigned bytes = BCM_NAN_HOST_HEADER_SIZE + 24 + length;
    uint8_t *event = ((void *(*)(size_t))native_0x7146d)(bytes);
    if (!event)
        return;
    event_header(event, BCM_NAN_HOST_RX, 24 + length, 0, 0, channel, flags, 0);
    copy(event + BCM_NAN_HOST_HEADER_SIZE, header, 24);
    copy(event + BCM_NAN_HOST_HEADER_SIZE + 24, body, length);
    send_event(nan, event, bytes);
    ((void (*)(void *))native_0x71491)(event);
}

/* Preserve the original RAM flashpatch for every non-NMI authentication.
 * Its first instruction only saves registers; no PC-relative relocation is
 * involved. The build verifies those four bytes before interposing them. */
NATIVE_ENTRY static int original_auth(
    void *, uintptr_t, const uint8_t *, void *, void *,
    const uint8_t *, unsigned, const void *, void *)
{
    __asm__ volatile ("push.w {r4, r5, r6, r7, r8, r9, r10, r11, lr}\n"
                      "ldr pc, 1f\n.balign 4\n1: .word 0x1cf5fdu\n");
}

int nan_host_rx_auth(void *wlc, uintptr_t arg2, const uint8_t *header,
                      void *bss, void *scb, const uint8_t *body,
                      unsigned length, const void *rx, void *packet)
{
    void *nan = wlc && PTR(wlc, 0x520) ? PTR(PTR(wlc, 0x520), 4) : 0;
    void *own = nan_host_nmi(nan);
    if (!own || !header || !equal(header + 4, (uint8_t *)own + 0x66, 6) ||
        (bss && bss != own))
        return original_auth(
            wlc, arg2, header, bss, scb, body, length, rx, packet);
    /* Once addressed to our NMI, malformed/unsupported authentication must
     * not fall into the STA/AP association state machine. */
    if (!nan_host_active(nan) || !body || !rx || length < 6 ||
        length > BCM_NAN_HOST_MAX_BODY ||
        (get16(header) & ~0x0800u) != 0xb0 || (header[10] & 1u) ||
        (header[22] & 15u) ||
        !equal(header + 16, (uint8_t *)own + 0x60, 6))
        return 0;
    /* The real caller 0x11af1c passes the D11 subheader (+12) to auth;
     * Action receives the enclosing receive descriptor. Channel decoding
     * expects that enclosing descriptor in both cases. */
    unsigned channel = ((unsigned (*)(void *, const void *))native_0x119465)(
        wlc, (const uint8_t *)rx - 12);
    if (channel && channel <= 196)
        host_receive(nan, header, body, length, channel | (channel > 14 ? 0xd000u : 0x1000u), 0);
    return 0;
}

int nan_host_group_address(const uint8_t *address)
{
    const uint8_t discovery[6] = {0x51, 0x6f, 0x9a, 1, 0, 0};
    const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
    return equal(address, discovery, 6) || equal(address, broadcast, 6);
}

int nan_host_rx_action(void *mac, void *bss, const uint8_t *header,
                        const uint8_t *body, unsigned length, const void *rx)
{
    if (!mac || !bss || !header || !body || !rx || length < 6 ||
        length > BCM_NAN_HOST_MAX_BODY ||
        (body[0] != 4 && body[0] != 9) || body[1] != 9 ||
        body[2] != 0x50 || body[3] != 0x6f || body[4] != 0x9a ||
        (body[5] != 0x13 && (body[5] != 0x18 || length < 7)))
        return 0;
    void *nan = PTR(mac, 4);
    if (!nan_host_active(nan) || nan_host_nmi(nan) != bss)
        return 0;
    if (!equal(header + 4, (uint8_t *)bss + 0x66, 6) &&
        !(nan_host_group_address(header + 4) &&
          equal(header + 16, (uint8_t *)bss + 0x60, 6)))
        return 0;
    unsigned flags = 0;
    if (get16(header) & 0x4000u || body[0] == 9) {
        /* Native MFP RX verifies tag/replay and strips IV/ICV before this
         * Action dispatcher. RX status is in D11 subheader +0x14, i.e.
         * enclosing descriptor +0x20. Require decrypt-attempt without error
         * and the same owned RX key; Protected alone is not verification. */
        if (!(get16(header) & 0x4000u) || body[0] != 9 || (header[4] & 1u) ||
            (U16(rx, 0x20) & 0x18u) != 8 || nan_station_rx_crypto(nan, header + 10))
            return 1;
        flags = BCM_NAN_HOST_RX_DECRYPTED;
    }
    unsigned channel = ((unsigned (*)(void *, const void *))native_0x119465)(PTR(nan, 8), rx);
    if (!channel || channel > 196)
        return 1;
    channel |= channel > 14 ? 0xd000u : 0x1000u;
    host_receive(nan, header, body, length, channel, flags);
    /* Consume valid NAN actions even when event allocation fails. Running
     * firmware negotiation after a host-delivery failure creates two owners. */
    return 1;
}

/* Only the native MFP wrapper calls this after sender-specific BIP succeeds.
 * The ordinary Action hook continues to reject unverified group actions. */
void nan_host_group_received(void *nan, const uint8_t *header, const uint8_t *body,
                              unsigned length, const void *rx)
{
    unsigned channel = ((unsigned (*)(void *, const void *))native_0x119465)(PTR(nan, 8), rx);
    if (channel && channel <= 196)
        host_receive(nan, header, body, length,
                     channel | (channel > 14 ? 0xd000u : 0x1000u), BCM_NAN_HOST_RX_DECRYPTED);
}

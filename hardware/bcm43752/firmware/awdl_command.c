#include "awdl_command.h"
#include "awdl_session.h"
#include "awdl_schedule.h"
#include "awdl_peer.h"
#include "awdl_tx.h"

static unsigned read16(const uint8_t *p)
{
    return p[0] | (unsigned)p[1] << 8;
}

static uint64_t read64(const uint8_t *p)
{
    /* A2 executes little-endian; byte access keeps unaligned ABI input safe. */
    uint64_t value;
    for (unsigned i = 0; i < sizeof(value); ++i)
        ((uint8_t *)&value)[i] = p[i];
    return value;
}

static int unicast(const uint8_t *p)
{
    unsigned any = 0;
    for (unsigned i = 0; i < 6; ++i)
        any |= p[i];
    return any && !(p[0] & 1);
}

static int syntax(const uint8_t *p, size_t length)
{
    if (!p || length < BCM_AWDL_HEADER_SIZE ||
        read16(p) != BCM_AWDL_ABI_VERSION || read16(p + 4) != length || read16(p + 6))
        return -2;
    unsigned op = read16(p + 2);
    if (op != BCM_AWDL_QUERY && !read64(p + 8))
        return -2;
    switch (op) {
    case BCM_AWDL_START:
        return length == BCM_AWDL_START_SIZE && unicast(p + 16) &&
            read16(p + 22) && !read16(p + 26) ? 0 : -2;
    case BCM_AWDL_QUERY:
    case BCM_AWDL_STOP:
        return length == BCM_AWDL_HEADER_SIZE ? 0 : -2;
    case BCM_AWDL_PEER: {
        if (length < BCM_AWDL_PEER_HEADER_SIZE || !unicast(p + 16))
            return -2;
        unsigned bytes = read16(p + 22);
        if (bytes > BCM_AWDL_PEER_IE_MAX || bytes != length - BCM_AWDL_PEER_HEADER_SIZE)
            return -2;
        /* Validate the entire IE stream before any native peer mutation. */
        unsigned at = BCM_AWDL_PEER_HEADER_SIZE;
        while (at < length) {
            if (length - at < 2 || p[at + 1] > length - at - 2 ||
                (p[at] == 45 && p[at + 1] != 26) ||
                (p[at] == 191 && p[at + 1] != 12))
                return -2;
            at += 2 + p[at + 1];
        }
        return 0;
    }
    case BCM_AWDL_PLAN:
        return length == BCM_AWDL_PLAN_SIZE &&
            !read16(p + 60) && !read16(p + 62) ? 0 : -2;
    case BCM_AWDL_WINDOW:
        if (length != BCM_AWDL_WINDOW_SIZE)
            return -2;
        return (!read64(p + 16) && !read64(p + 24)) ||
            read64(p + 24) > read64(p + 16) ? 0 : -2;
    case BCM_AWDL_TX:
        return length >= BCM_AWDL_TX_HEADER_SIZE + 36 &&
            length <= BCM_AWDL_TX_HEADER_SIZE + BCM_AWDL_TX_FRAME_MAX &&
            read64(p + 16) && read16(p + 24) && read64(p + 28) &&
            read16(p + 26) == length - BCM_AWDL_TX_HEADER_SIZE ? 0 : -2;
    case BCM_AWDL_CANCEL_TX:
        return length == BCM_AWDL_CANCEL_TX_SIZE && read64(p + 16) ? 0 : -2;
    default:
        return -23;
    }
}

int awdl_command_validate(const struct nan_command_digest *digest, uint32_t action,
                          const uint8_t *input, size_t length,
                          uint8_t *output, size_t *output_length)
{
    int error = nan_command_validate(digest, action, input, length, output, output_length);
    if (error)
        return error;
    error = syntax(input, length);
    if (error)
        return error;
    unsigned op = read16(input + 2);
    void *nan = ((void *const *)digest)[1];
    struct awdl_session *s = awdl_session_get(nan);
    uint64_t generation = read64(input + 8);
    if (op == BCM_AWDL_START)
        return s ? -16 : 0;
    if (op == BCM_AWDL_QUERY && !generation)
        return 0;
    if (!s || s->generation != generation)
        return -30;
    if (s->stopping && op != BCM_AWDL_STOP && op != BCM_AWDL_QUERY &&
        op != BCM_AWDL_CANCEL_TX)
        return -16;
    return 0;
}

int awdl_command_query(const struct nan_command_digest *digest, const uint8_t *input,
                       size_t length, uint8_t *output, size_t *output_length)
{
    if (!output_length)
        return -2;
    size_t capacity = *output_length;
    *output_length = 0;
    int error = syntax(input, length);
    if (error || read16(input + 2) != BCM_AWDL_QUERY)
        return error ? error : -2;
    error = awdl_session_status(((void *const *)digest)[1], read64(input + 8), output, capacity);
    if (!error)
        *output_length = BCM_AWDL_STATUS_SIZE;
    return error;
}

int awdl_command_set(const struct nan_command_digest *digest, const uint8_t *p,
                     size_t length, uint8_t *output, size_t *output_length)
{
    (void)output;
    if (output_length)
        *output_length = 0;
    int error = syntax(p, length);
    if (error)
        return error;
    void *nan = ((void *const *)digest)[1];
    unsigned op = read16(p + 2);
    struct awdl_session *s = awdl_session_get(nan);
    uint64_t generation = read64(p + 8);
    if (op == BCM_AWDL_START) {
        uint32_t drain = 0;
        for (unsigned i = 0; i < 4; ++i)
            drain |= (uint32_t)p[28 + i] << (i * 8);
        return awdl_session_bind(nan, generation, p + 16, read16(p + 22),
                                  read16(p + 24), drain);
    }
    if (!s || s->generation != generation)
        return -30;
    if (s->stopping && op != BCM_AWDL_STOP && op != BCM_AWDL_CANCEL_TX)
        return -16;
    switch (op) {
    case BCM_AWDL_STOP:
        return awdl_session_release(s);
    case BCM_AWDL_PEER:
        if (read16(p + 22))
            return awdl_peer_set(s, p + 16, p + 24, read16(p + 22));
        for (unsigned i = 0; i < 6; ++i)
            if (s->peer[i] != p[16 + i])
                return -30;
        return awdl_peer_remove(s);
    case BCM_AWDL_PLAN:
        return awdl_schedule_install(s, p + BCM_AWDL_HEADER_SIZE,
                                      length - BCM_AWDL_HEADER_SIZE);
    case BCM_AWDL_WINDOW: {
        int error = awdl_timing_window(&s->timing, p + BCM_AWDL_HEADER_SIZE,
                                      length - BCM_AWDL_HEADER_SIZE);
        return error ? error : awdl_schedule_window(s);
    }
    case BCM_AWDL_TX:
        return awdl_tx_enqueue(s, read64(p + 16), read16(p + 24),
                                read64(p + 28), p + BCM_AWDL_TX_HEADER_SIZE, read16(p + 26));
    case BCM_AWDL_CANCEL_TX:
        return awdl_tx_cancel(s, read64(p + 16));
    default:
        return -23;
    }
}

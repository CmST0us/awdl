#ifndef BCM43752_AWDL_SESSION_H
#define BCM43752_AWDL_SESSION_H
#include "awdl_timing.h"

struct awdl_session {
    void *nan, *bss, *scb, *msch, *tx;
    uint64_t generation;
    struct awdl_timing timing;
    uint8_t local[6], peer[6];
    uint16_t bound_channel;
    uint16_t peer_ie_length;
    uint16_t data_sequence;
    uint8_t peer_ies[BCM_AWDL_PEER_IE_MAX];
    unsigned peer_closing;
    uint32_t queued, submitted, completed, received;
    int error;
    unsigned stopping;
    void *window_timer; /* Native HRT, cancelled before session release. */
    uint32_t aggregation_tick; /* Native watchdog seconds, not an extra aging clock. */
};
struct awdl_session *awdl_session_get(void *nan);
int awdl_session_bind(void *nan, uint64_t generation, const uint8_t local[6],
                      uint16_t primary, uint16_t secondary, uint32_t drain_us);
int awdl_session_release(struct awdl_session *);
int awdl_session_holds_bss(void *nan, void *bss);
int awdl_session_status(void *nan, uint64_t generation, uint8_t *out, unsigned size);
#endif

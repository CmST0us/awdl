/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BCM_AWDL_HOST_SESSION_H
#define BCM_AWDL_HOST_SESSION_H
#include <stddef.h>
#include <stdint.h>
#include "../hostap/netlink.h"
#include "../include/awdl_host_abi.h"

struct bcm_awdl_status {
    uint64_t generation, firmware_us;
    uint32_t flags, queued, submitted, completed, received;
    int32_t error;
    uint8_t local[6], peer[6];
    uint16_t channel;
};
struct bcm_awdl_session {
    struct bcm_link *link; /* Borrowed; existing NAN vendor transport. */
    uint64_t generation;
    /* Sampled mapping for host wakeups only. Protocol plans and frame times
     * stay in firmware time so refreshing this map cannot move live slots. */
    uint64_t firmware_origin, host_origin, uncertainty_us;
    int clock_ready;
};
struct bcm_awdl_event {
    uint16_t kind, flags, channel;
    uint64_t generation, firmware_us, cookie;
    int32_t result;
    const uint8_t *frame; /* Borrowed until the enclosing receive callback returns. */
    size_t length;
};
/* 1: matching event, 0: unrelated/stale generation, negative: malformed. */
int bcm_awdl_decode_event(const struct bcm_awdl_session *, const struct bcm_event *,
                           struct bcm_awdl_event *);
uint64_t bcm_awdl_monotonic_us(void);
uint16_t bcm_awdl_chanspec(unsigned channel);
int bcm_awdl_query(struct bcm_awdl_session *, struct bcm_awdl_status *);
int bcm_awdl_clock_sample(struct bcm_awdl_session *, struct bcm_awdl_status *);
int bcm_awdl_to_firmware(const struct bcm_awdl_session *, uint64_t host_us, uint64_t *);
int bcm_awdl_to_host(const struct bcm_awdl_session *, uint64_t firmware_us, uint64_t *);
int bcm_awdl_start(struct bcm_awdl_session *, const uint8_t local[6],
                   unsigned primary, unsigned secondary, uint32_t drain_us);
int bcm_awdl_stop(struct bcm_awdl_session *);
int bcm_awdl_peer(struct bcm_awdl_session *, const uint8_t mac[6], const void *ies, size_t);
int bcm_awdl_plan(struct bcm_awdl_session *, uint64_t firmware_epoch,
                  const uint8_t channels[16], unsigned preceding, unsigned following);
int bcm_awdl_window(struct bcm_awdl_session *, uint64_t firmware_open, uint64_t firmware_close);
int bcm_awdl_tx(struct bcm_awdl_session *, uint64_t cookie, unsigned channel,
                uint64_t origin, const void *frame, size_t length, int *rejected);
int bcm_awdl_cancel(struct bcm_awdl_session *, uint64_t cookie);
#endif

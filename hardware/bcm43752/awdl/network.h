/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BCM_AWDL_NETWORK_H
#define BCM_AWDL_NETWORK_H
#include "session.h"
struct bcm_awdl_network {
    struct bcm_link *link;
    uint8_t local[6];
    const char *congestion, *window, *reordering, *queue;
    unsigned drain_us;
};
int bcm_awdl_network_policy(struct bcm_awdl_network *);
int bcm_awdl_network_up(void);
int bcm_awdl_network_ready(void *, const uint8_t peer[6]);
#endif

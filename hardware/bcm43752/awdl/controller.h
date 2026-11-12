/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BCM_AWDL_CONTROLLER_H
#define BCM_AWDL_CONTROLLER_H
#include "session.h"
#include "../../../components/awdl/control.h"

struct bcm_awdl_controller {
    struct bcm_awdl_session *session;
    struct Control *control;
    uint64_t origin, epoch, next_cookie, pending_cookie;
    uint64_t last_now; /* Monotonic protocol time in the firmware domain. */
    uint64_t window_open, window_close;
    uint8_t local[6], peer[6], channels[16], preceding, following;
    unsigned primary, secondary, active, drain_us;
    int planned, error;
    uint8_t action[BCM_AWDL_TX_FRAME_MAX];
    size_t action_length;
    int (*netdev_ready)(void *, const uint8_t peer[6]);
    void *context;
};
/* Called after firmware START and NDI creation. Local PHY capabilities are
 * supplied by the real wiphy, not inferred from the chipset name. */
int bcm_awdl_controller_open(struct bcm_awdl_controller *, struct bcm_awdl_session *,
    const uint8_t local[6], unsigned primary, unsigned secondary,
    const char *target, const char *name, const uint8_t *he, uint32_t vht,
    unsigned drain_us, int (*netdev_ready)(void *, const uint8_t *), void *context);
void bcm_awdl_controller_close(struct bcm_awdl_controller *);
/* Absolute CLOCK_MONOTONIC microsecond deadline, zero on error. */
uint64_t bcm_awdl_controller_tick(struct bcm_awdl_controller *);
int bcm_awdl_controller_event(struct bcm_awdl_controller *, const struct bcm_event *);
int bcm_awdl_controller_select(struct bcm_awdl_controller *, const char *target);
#endif

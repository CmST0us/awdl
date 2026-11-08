/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BCM_NAN_SCHEDULE_H
#define BCM_NAN_SCHEDULE_H
#include "phy.h"
#include "command.h"
struct bcm_schedule { u8 grid[BCM_NAN_HOST_SCHEDULE_GRID_SIZE]; };
int bcm_schedule_local(const struct bcm_phy *, u8 map, const struct nan_schedule_config *,
                        struct bcm_schedule *);
int bcm_schedule_peer(const struct bcm_phy *, const struct bcm_schedule *, u8 sequence,
                       const struct nan_peer_schedule_config *, struct bcm_schedule *);
struct bcm_result bcm_schedule_install(struct bcm_link *, const u8 *peer,
                                        const struct bcm_schedule *);
#endif

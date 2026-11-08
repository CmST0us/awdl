/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BCM_NAN_STARTUP_H
#define BCM_NAN_STARTUP_H
#include "phy.h"
#include "command.h"

struct bcm_startup {
	/* Set before the first init mutation. A failed/ambiguous command keeps
	 * ownership until native deinit verifies that every resource is idle. */
	bool owned, prepared;
	u8 nmi[6], cluster[6];
	u16 discovery[2];
};
int bcm_startup_prepare(struct bcm_startup *, struct bcm_link *, const struct bcm_phy *,
                        const u8 *nmi, const struct nan_cluster_config *);
/* Caller first waits for actual STOP and deletes all NDI owners. */
int bcm_startup_release(struct bcm_startup *, struct bcm_link *);
#endif

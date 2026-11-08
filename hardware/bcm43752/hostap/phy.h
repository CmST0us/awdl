/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BCM_NAN_PHY_H
#define BCM_NAN_PHY_H

#include "utils/includes.h"
#include "utils/common.h"
#include "drivers/driver.h"
#include "netlink.h"

struct bcm_phy {
	struct hostapd_hw_modes band[2];
	bool present[2], ht[2], vht[2];
	u32 interface_types;
	unsigned vendor_commands;
	bool other_mode_commands;
	char country[3];
};
int bcm_phy_read(struct bcm_link *link, struct bcm_phy *phy);
void bcm_phy_clear(struct bcm_phy *phy);
struct hostapd_hw_modes *bcm_phy_copy(const struct bcm_phy *phy, u16 *count);
int bcm_phy_chanspec(const struct bcm_phy *phy, const struct nan_schedule_channel *channel,
                     u16 *chanspec);
int bcm_phy_nan_capability(const struct bcm_phy *, struct nan_capa *);
int bcm_chanspec_frequency(u16 chanspec);

#endif

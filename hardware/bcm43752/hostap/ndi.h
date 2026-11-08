/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BCM_NAN_NDI_H
#define BCM_NAN_NDI_H
#include "command.h"
#include <net/if.h>
struct bcm_ndi {
	/* attempted survives ambiguous command failures; flags come only from
	 * the DHD owner snapshot backed by actual interface/flow completions. */
	bool attempted;
	u8 mac[6], ifidx, bssidx;
	char name[IFNAMSIZ];
	u32 flags, ifindex;
	s32 native_result, host_result;
};
int bcm_ndi_prepare(struct bcm_ndi *, const u8 *mac, const u8 *nmi, const char *name);
int bcm_ndi_command(struct bcm_ndi *, struct bcm_link *, u16 operation);
bool bcm_ndi_created(const struct bcm_ndi *);
bool bcm_ndi_gone(const struct bcm_ndi *);
bool bcm_ndi_changed(const struct bcm_ndi *, const struct bcm_event *);
int bcm_ndi_up(const struct bcm_ndi *);
#endif

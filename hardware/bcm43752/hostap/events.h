/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BCM_NAN_EVENTS_H
#define BCM_NAN_EVENTS_H
#include "command.h"

enum bcm_event_kind { BCM_EVENT_NONE, BCM_EVENT_START, BCM_EVENT_STOP,
	BCM_EVENT_CLUSTER, BCM_EVENT_ROLE, BCM_EVENT_DW, BCM_EVENT_RX, BCM_EVENT_TX, BCM_EVENT_KEY_DRAINED };
struct bcm_notification {
	enum bcm_event_kind kind;
	u8 nmi[6], cluster[6], role;
	u16 chanspec, flags;
	u64 cookie;
	s32 result;
	const u8 *frame;
	size_t length;
};
/* Zero means well-formed (including an unrelated event); negative means a
 * malformed event from this transport. The frame is borrowed. */
int bcm_event_decode(const struct bcm_event *, struct bcm_notification *);
#endif

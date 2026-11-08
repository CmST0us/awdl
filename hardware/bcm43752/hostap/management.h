/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BCM_NAN_MANAGEMENT_H
#define BCM_NAN_MANAGEMENT_H
#include "events.h"
#include "drivers/driver.h"
struct bcm_management;
typedef void (*bcm_notify_fn)(void *, enum wpa_event_type, union wpa_event_data *);
typedef void (*bcm_failure_fn)(void *, int);
struct bcm_management *bcm_management_new(struct bcm_link *, const u8 *nmi,
	bcm_notify_fn, bcm_failure_fn, void *);
int bcm_management_free(struct bcm_management *);
void bcm_management_started(struct bcm_management *);
/* Only call after native STOP completion; a failed/deadline STOP retains TX. */
int bcm_management_stopped(struct bcm_management *);
int bcm_management_send(struct bcm_management *, const u8 *, size_t, u16 chanspec);
void bcm_management_event(struct bcm_management *, const struct bcm_notification *);
int bcm_management_cancel(struct bcm_management *, const u8 *peer);
bool bcm_management_busy(const struct bcm_management *, const u8 *peer);
#endif

/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BCM_NAN_KEYS_H
#define BCM_NAN_KEYS_H
#include "command.h"
#include "drivers/driver.h"
struct bcm_key {
	bool owned, ready;
	u16 command;
	u8 local[6], peer[6], id;
	u8 request[BCM_NAN_HOST_DATA_KEY_SIZE];
	size_t length;
};
/* NMI/NDI pairwise and group keys have separate identities. The retained
 * request allows identical retries without resetting native packet numbers. */
struct bcm_keys { unsigned drain_events; struct bcm_key entry[BCM_NAN_HOST_GROUP_KEY_LIMIT + BCM_NAN_HOST_STATION_LIMIT]; };
int bcm_key_set(struct bcm_keys *, struct bcm_link *, const u8 *local, bool data,
                const struct wpa_driver_set_key_params *);
int bcm_key_sequence(struct bcm_keys *, struct bcm_link *, const u8 *local,
                     const u8 *peer, int id, u8 sequence[8]);
int bcm_keys_remove(struct bcm_keys *, struct bcm_link *, const u8 *local, const u8 *peer);
bool bcm_keys_owned(const struct bcm_keys *, const u8 *local, const u8 *peer);
/* Only after actual native STOP has released its key owners. */
void bcm_keys_stopped(struct bcm_keys *);
#endif

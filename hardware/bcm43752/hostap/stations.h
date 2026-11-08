/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BCM_NAN_STATIONS_H
#define BCM_NAN_STATIONS_H
#include "command.h"
#include "drivers/driver.h"
struct bcm_station {
	bool owned, ready, data;
	u8 local[6], peer[6], nmi[6], ndp_id;
	u32 flags;
	u8 ies[BCM_NAN_HOST_DATA_IE_MAX];
	size_t ies_length;
};
struct bcm_stations { struct bcm_station entry[BCM_NAN_HOST_STATION_LIMIT]; };
struct bcm_station *bcm_station_find(struct bcm_stations *, const u8 *local, const u8 *peer);
int bcm_station_add(struct bcm_stations *, struct bcm_link *, const u8 *local,
                    bool data, const struct hostapd_sta_add_params *);
int bcm_station_remove(struct bcm_stations *, struct bcm_link *, const u8 *local, const u8 *peer);
int bcm_station_inactivity(struct bcm_stations *, struct bcm_link *, const u8 *local, const u8 *peer);
int bcm_station_flags(struct bcm_stations *, struct bcm_link *, const u8 *local,
                      const u8 *peer, unsigned set, unsigned mask);
bool bcm_station_data_for(const struct bcm_stations *, const u8 *nmi);
/* Native STOP must have completed before its actual owners are cleared. */
void bcm_stations_stopped(struct bcm_stations *);
#endif

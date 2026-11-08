/* SPDX-License-Identifier: BSD-3-Clause */
#include "startup.h"

static int get(struct bcm_link *link, u16 command, const void *input, size_t length,
                void *output, size_t expected)
{
	struct bcm_result result = bcm_get(link, command, input, length, output, expected);
	int error = bcm_result_errno(result);
	return error ? error : result.used == expected ? 0 : -EPROTO;
}
static int set(struct bcm_link *link, u16 command, const void *input, size_t length)
{
	return bcm_result_errno(bcm_set(link, command, input, length));
}
static int configuration(const struct bcm_phy *phy, const struct nan_cluster_config *c,
                          u16 discovery[2])
{
	if (!c || !c->master_pref || c->master_pref == 255 || c->dual_band > 1 ||
	    c->discovery_beacon_interval < 50 || c->discovery_beacon_interval > 200 ||
	    !c->scan_period || c->scan_period > 4294 || c->scan_dwell_time < 10 ||
	    c->scan_dwell_time > 150) return -EINVAL;
	if (c->extra_nan_attrs_len || c->vendor_elems_len ||
	    c->low_band_cfg.disable_scan || c->high_band_cfg.disable_scan ||
	    c->low_band_cfg.awake_dw_interval != 1 ||
	    c->high_band_cfg.awake_dw_interval != 1) return -EOPNOTSUPP;
	if (!is_zero_ether_addr(c->cluster_id) && os_memcmp(c->cluster_id, "\x50\x6f\x9a\x01", 4))
		return -EINVAL;
	if (c->low_band_cfg.frequency != 2437 ||
	    (c->high_band_cfg.frequency != 5745 && c->high_band_cfg.frequency != 5220))
		return -EOPNOTSUPP;
	const struct nan_band_config *bands[] = {&c->low_band_cfg, &c->high_band_cfg};
	for (unsigned i = 0; i < 2; ++i) {
		if (bands[i]->rssi_middle < -127 || bands[i]->rssi_close > 0 ||
		    bands[i]->rssi_middle >= bands[i]->rssi_close) return -EINVAL;
		discovery[i] = 0;
		if (i && !c->dual_band) continue;
		struct nan_schedule_channel channel = {.freq = bands[i]->frequency,
			.center_freq1 = bands[i]->frequency, .bandwidth = 20};
		int error = bcm_phy_chanspec(phy, &channel, &discovery[i]);
		if (error) return error;
	}
	return 0;
}
int bcm_startup_prepare(struct bcm_startup *s, struct bcm_link *link, const struct bcm_phy *phy,
                        const u8 *nmi, const struct nan_cluster_config *c)
{
	if (s->owned) return -EBUSY;
	if (!bcm_unicast(nmi)) return -EINVAL;
	u16 discovery[2];
	int error = configuration(phy, c, discovery);
	if (error) return error;
	u8 initialized = 0;
	error = get(link, 0x0101, NULL, 0, &initialized, 1);
	if (error) return error;
	/* A new host process cannot silently adopt a previous process's NAN,
	 * keys or pending frames. Recovery retains DHD ownership until reload. */
	if (initialized) return -EBUSY;
	u8 peers = BCM_NAN_HOST_STATION_LIMIT, one = 1, zero = 0;
	error = set(link, 0x0510, &peers, 1);
	if (error) return error;
	u8 cluster[6];
	os_memcpy(cluster, c->cluster_id, 6);
	if (is_zero_ether_addr(cluster)) {
		os_memcpy(cluster, "\x50\x6f\x9a\x01", 4);
		if (os_get_random(cluster + 4, 2)) return -EIO;
	}
	os_memset(s, 0, sizeof(*s)); s->owned = true;
	os_memcpy(s->nmi, nmi, 6); os_memcpy(s->cluster, cluster, 6);
	os_memcpy(s->discovery, discovery, sizeof(discovery));
	error = set(link, 0x0101, &one, 1);
	if (error) return error;
	u8 version[2], request[2] = {0}, control[4], control2[8], metrics[4];
	error = get(link, 0x0000, request, sizeof(request), version, sizeof(version));
	if (error) return error;
	if (WPA_GET_LE16(version) != 2) return -EPROTONOSUPPORT;
	error = get(link, 0x0115, NULL, 0, control, sizeof(control));
	if (error) return error;
	/* Preserve native/module-owned bits; disable autonomous replies,
	 * inactive-peer deletion and infrastructure coexistence. */
	u32 policy = WPA_GET_LE32(control);
	policy = (policy & ~(0x000001e0U | 0x00020000U | 0x00040000U)) | 0x00010000U;
	WPA_PUT_LE32(control, policy);
	error = set(link, 0x0115, control, sizeof(control));
	if (error) return error;
	error = get(link, 0x0118, NULL, 0, control2, sizeof(control2));
	if (error) return error;
	control2[0] |= 1; /* Host SDF transmission; keep SLOT_PRE_CLOSE and all other module bits. */
	error = set(link, 0x0118, control2, sizeof(control2));
	if (error) return error;
	u8 social[4] = {6, c->dual_band ? discovery[1] & 0xff : 0, 0, 0};
	error = set(link, 0x0401, social, sizeof(social));
	if (error) return error;
	u8 capability[12] = {1, !!c->dual_band, 1, !!c->dual_band, 0, 0, 0, 1, 0, 0, 0, 0};
	error = set(link, 0x0119, capability, sizeof(capability));
	if (error) return error;
	error = get(link, 0x0202, NULL, 0, metrics, sizeof(metrics));
	if (error) return error;
	metrics[1] = c->master_pref; /* byte 0 is the native random factor. */
	error = set(link, 0x0202, metrics, sizeof(metrics));
	if (!error) error = set(link, 0x0201, &zero, 1); /* Native automatic master election. */
	if (!error) error = set(link, 0x0204, &one, 1); /* Permit native cluster merging. */
	if (error) return error;
	u8 beacon[2]; WPA_PUT_LE16(beacon, c->discovery_beacon_interval);
	error = set(link, 0x010e, beacon, sizeof(beacon));
	if (error) return error;
	u8 rssi[4] = {(u8)c->low_band_cfg.rssi_close, (u8)c->low_band_cfg.rssi_middle,
		(u8)c->high_band_cfg.rssi_close, (u8)c->high_band_cfg.rssi_middle};
	error = set(link, 0x0206, rssi, sizeof(rssi));
	if (error) return error;
	for (unsigned band = 0; band < 2; ++band) {
		u8 scan[6] = {0};
		WPA_PUT_LE16(scan, c->scan_dwell_time); WPA_PUT_LE16(scan + 2, c->scan_period);
		scan[4] = band;
		error = set(link, 0x011a, scan, sizeof(scan));
		if (error) return error;
	}
	error = set(link, 0x010d, nmi, 6);
	if (!error) error = set(link, 0x010c, cluster, 6);
	if (error) return error;
	u8 mask[20] = {1, 0, 16, 0};
	const u8 reasons[] = {1, 3, 10, 11, BCM_NAN_HOST_REASON};
	for (unsigned i = 0; i < ARRAY_SIZE(reasons); ++i)
		mask[4 + (reasons[i] - 1) / 8] |= 1U << ((reasons[i] - 1) % 8);
	error = set(link, 0x0114, mask, sizeof(mask));
	if (!error) s->prepared = true;
	/* No START is fabricated here. The radio layer enables NAN only after
	 * this returns, then waits for the actual matching native START event. */
	return error;
}
int bcm_startup_release(struct bcm_startup *s, struct bcm_link *link)
{
	if (!s->owned) return 0;
	u8 zero = 0;
	int error = set(link, 0x0101, &zero, 1);
	if (!error) os_memset(s, 0, sizeof(*s));
	return error;
}

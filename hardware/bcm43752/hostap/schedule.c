/* SPDX-License-Identifier: BSD-3-Clause */
#include "schedule.h"
#include "common/nan_defs.h"
#include "utils/wpabuf.h"

static void initialize(struct bcm_schedule *s, u8 map, u8 sequence)
{
	os_memset(s, 0, sizeof(*s));
	WPA_PUT_LE16(s->grid, BCM_NAN_HOST_SCHEDULE_SLOTS);
	WPA_PUT_LE16(s->grid + 2, BCM_NAN_HOST_SCHEDULE_PERIOD_TU);
	s->grid[4] = BCM_NAN_HOST_SCHEDULE_SLOT_TU; s->grid[5] = map; s->grid[6] = sequence;
}
static const u8 *bitmap(const struct nan_schedule_channel *c)
{
	if (!c->time_bitmap || wpabuf_len(c->time_bitmap) != BCM_NAN_HOST_SCHEDULE_SLOTS / 8)
		return NULL;
	return wpabuf_head(c->time_bitmap);
}
static unsigned nss(const struct bcm_phy *phy, unsigned band)
{
	unsigned count = 0;
	while (count < 2 && phy->band[band].mcs_set[count]) ++count;
	return count;
}
static int append(struct bcm_schedule *s, unsigned i, u16 channel, u8 streams)
{
	if (i >= BCM_NAN_HOST_SCHEDULE_SLOTS || !streams || streams > 2) return -EOPNOTSUPP;
	u8 *slot = s->grid + 8 + 4 * i;
	if (slot[0] && (slot[1] != streams || WPA_GET_LE16(slot + 2) != channel)) return -EINVAL;
	slot[0] = 1; slot[1] = streams; WPA_PUT_LE16(slot + 2, channel);
	return 0;
}
int bcm_schedule_local(const struct bcm_phy *phy, u8 map, const struct nan_schedule_config *config,
                        struct bcm_schedule *output)
{
	if (!config || !map || map > 15 || !config->num_channels ||
	    config->num_channels > MAX_NUM_NAN_SCHEDULE_CHANNELS || config->deferred ||
	    !config->avail_attr || wpabuf_len(config->avail_attr) < 6) return -EOPNOTSUPP;
	const u8 *attribute = wpabuf_head(config->avail_attr);
	if (attribute[0] != NAN_ATTR_NAN_AVAILABILITY ||
	    WPA_GET_LE16(attribute + 1) != wpabuf_len(config->avail_attr) - 3 ||
	    (WPA_GET_LE16(attribute + 4) & 15) != map) return -EINVAL;
	struct bcm_schedule next;
	initialize(&next, map, attribute[3]);
	unsigned used = 0;
	for (unsigned i = 0; i < config->num_channels; ++i) {
		const struct nan_schedule_channel *c = &config->channels[i];
		u16 channel;
		const u8 *bits = bitmap(c);
		if (!bits) return -EINVAL;
		int error = bcm_phy_chanspec(phy, c, &channel);
		if (error) return error;
		if (!c->rx_nss || c->rx_nss > nss(phy, c->freq > 3000)) return -EOPNOTSUPP;
		for (unsigned slot = 0; slot < BCM_NAN_HOST_SCHEDULE_SLOTS; ++slot) {
			if (!(bits[slot / 8] & (1U << (slot % 8)))) continue;
			if (next.grid[8 + slot * 4]) return -EINVAL;
			error = append(&next, slot, channel, c->rx_nss);
			if (error) return error;
			++used;
		}
	}
	if (!used) return -ENODATA;
	*output = next; return 0;
}
static unsigned width(u16 channel)
{
	switch (channel & 0x3800) { case 0x1000: return 20; case 0x1800: return 40; case 0x2000: return 80; }
	return 0;
}
static bool contains(u16 wider, u16 narrower)
{
	if ((wider & 0xc000) != (narrower & 0xc000) ||
	    bcm_chanspec_frequency(wider) != bcm_chanspec_frequency(narrower)) return false;
	int w = width(wider), n = width(narrower);
	int wc = wider & 0xff, nc = narrower & 0xff;
	return w && n && w >= n && (nc * 5 - n / 2 >= wc * 5 - w / 2) &&
		(nc * 5 + n / 2 <= wc * 5 + w / 2);
}
int bcm_schedule_peer(const struct bcm_phy *phy, const struct bcm_schedule *local, u8 sequence,
                       const struct nan_peer_schedule_config *config, struct bcm_schedule *output)
{
	if (!config || !config->n_maps || config->n_maps > MAX_NUM_NAN_MAPS ||
	    WPA_GET_LE16(local->grid) != BCM_NAN_HOST_SCHEDULE_SLOTS) return -EINVAL;
	unsigned best_count = 0;
	struct bcm_schedule best;
	for (unsigned m = 0; m < config->n_maps; ++m) {
		const struct nan_schedule_map *map = &config->maps[m];
		if (map->map_id > 15 || map->sched.num_channels > MAX_NUM_NAN_SCHEDULE_CHANNELS ||
		    map->sched.deferred) return -EINVAL;
		struct bcm_schedule next;
		initialize(&next, map->map_id, sequence);
		for (unsigned c = 0; c < map->sched.num_channels; ++c) {
			const struct nan_schedule_channel *input = &map->sched.channels[c];
			u16 channel;
			const u8 *bits = bitmap(input);
			if (!bits) return -EINVAL;
			/* Select only the portion this single radio can actually use.
			 * Peer maps on other radios/bands do not create extra modes. */
			if (bcm_phy_chanspec(phy, input, &channel)) continue;
			u8 streams = input->rx_nss ? input->rx_nss : 1;
			if (streams > 2) streams = 2;
			for (unsigned slot = 0; slot < BCM_NAN_HOST_SCHEDULE_SLOTS; ++slot) {
				if (!(bits[slot / 8] & (1U << (slot % 8)))) continue;
				const u8 *own = local->grid + 8 + slot * 4;
				u16 local_channel = WPA_GET_LE16(own + 2), selected;
				if (!own[0]) continue;
				if (contains(channel, local_channel)) selected = local_channel;
				else if (contains(local_channel, channel)) selected = channel;
				else continue;
				int error = append(&next, slot, selected, streams);
				if (error) return error;
			}
		}
		unsigned count = 0;
		for (unsigned i = 0; i < BCM_NAN_HOST_SCHEDULE_SLOTS; ++i)
			count += !!next.grid[8 + i * 4];
		/* Keep one peer map's actual ID/sequence. Combining independent
		 * peer maps under an invented map ID would misrepresent its NDL. */
		if (count > best_count) { best = next; best_count = count; }
	}
	if (!best_count) return -ENODATA;
	*output = best; return 0;
}
struct bcm_result bcm_schedule_install(struct bcm_link *link, const u8 *peer,
                                        const struct bcm_schedule *schedule)
{
	if ((!peer && !schedule) || (peer && !bcm_unicast(peer)))
		return (struct bcm_result){.transport = -EINVAL};
	u8 wire[BCM_NAN_HOST_SCHEDULE_SIZE];
	bcm_header(wire, schedule ? BCM_NAN_HOST_ADD : BCM_NAN_HOST_REMOVE, sizeof(wire));
	if (peer) os_memcpy(wire + 8, peer, 6);
	if (schedule) {
		os_memcpy(wire + 24, schedule->grid, 8);
		for (unsigned i = 0; i < BCM_NAN_HOST_SCHEDULE_SLOTS; ++i) {
			const u8 *slot = schedule->grid + 8 + 4 * i;
			if (!slot[0]) continue;
			os_memcpy(wire + 24 + 8 + 2 * i, slot + 2, 2);
			if (slot[1] == 2)
				wire[24 + BCM_NAN_HOST_SCHEDULE_NSS_OFFSET + i / 8] |= 1U << (i % 8);
		}
	}
	return bcm_set(link, peer ? BCM_NAN_HOST_PEER_SCHEDULE_COMMAND :
		BCM_NAN_HOST_LOCAL_SCHEDULE_COMMAND, wire, sizeof(wire));
}

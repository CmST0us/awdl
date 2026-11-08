/* SPDX-License-Identifier: BSD-3-Clause */
#include "phy.h"
#include <linux/nl80211.h>
#include "common/nan_defs.h"
#include "../include/nan_vendor_abi.h"
#include "../include/nan_host_abi.h"

typedef int (*nested_fn)(void *, unsigned, const void *, size_t);
static int nested(const struct nlattr *container, nested_fn visit, void *context)
{
	if (!container) return 0;
	const u8 *p = bcm_nla_value(container);
	size_t left = bcm_nla_size(container);
	while (left) {
		const struct nlattr *a = (const void *)p;
		if (left < NLA_HDRLEN || a->nla_len < NLA_HDRLEN ||
		    (size_t)NLA_ALIGN(a->nla_len) > left) return -EBADMSG;
		int result = visit(context, a->nla_type & NLA_TYPE_MASK, bcm_nla_value(a), bcm_nla_size(a));
		if (result) return result;
		left -= NLA_ALIGN(a->nla_len); p += NLA_ALIGN(a->nla_len);
	}
	return 0;
}
static int channel_number(int frequency)
{
	if (frequency >= 2412 && frequency <= 2472 && (frequency - 2407) % 5 == 0)
		return (frequency - 2407) / 5;
	if (frequency == 2484) return 14;
	if (frequency >= 5005 && frequency <= 5895 && frequency % 5 == 0)
		return (frequency - 5000) / 5;
	return 0;
}
static int frequency(void *context, unsigned index, const void *data, size_t length)
{
	struct hostapd_hw_modes *mode = context;
	const struct nlattr *a[NL80211_FREQUENCY_ATTR_MAX + 1];
	(void)index;
	if (bcm_nla_parse(data, length, a, ARRAY_SIZE(a)) ||
	    bcm_nla_size(a[NL80211_FREQUENCY_ATTR_FREQ]) != 4) return -EBADMSG;
	int freq = bcm_nla_u32(a[NL80211_FREQUENCY_ATTR_FREQ]);
	int channel = channel_number(freq);
	if (!channel || (freq > 3000) != (mode->mode == HOSTAPD_MODE_IEEE80211A)) return 0;
	struct hostapd_channel_data *c = NULL;
	for (int i = 0; i < mode->num_channels; ++i)
		if (mode->channels[i].freq == freq) c = &mode->channels[i];
	if (!c) {
		if (mode->num_channels >= 128) return -EOVERFLOW;
		void *next = os_realloc_array(mode->channels, mode->num_channels + 1, sizeof(*c));
		if (!next) return -ENOMEM;
		mode->channels = next;
		c = &mode->channels[mode->num_channels++];
	}
	os_memset(c, 0, sizeof(*c));
	c->freq = freq; c->chan = channel;
	c->allowed_bw = HOSTAPD_CHAN_WIDTH_20 | HOSTAPD_CHAN_WIDTH_40P | HOSTAPD_CHAN_WIDTH_40M;
	if (freq > 3000) c->allowed_bw |= HOSTAPD_CHAN_WIDTH_80;
	if (a[NL80211_FREQUENCY_ATTR_DISABLED]) c->flag |= HOSTAPD_CHAN_DISABLED;
	if (a[NL80211_FREQUENCY_ATTR_NO_IR]) c->flag |= HOSTAPD_CHAN_NO_IR;
	if (a[NL80211_FREQUENCY_ATTR_RADAR]) c->flag |= HOSTAPD_CHAN_RADAR;
	if (a[NL80211_FREQUENCY_ATTR_INDOOR_ONLY]) c->flag |= HOSTAPD_CHAN_INDOOR_ONLY;
	if (a[NL80211_FREQUENCY_ATTR_NO_20MHZ]) c->allowed_bw &= ~HOSTAPD_CHAN_WIDTH_20;
	if (a[NL80211_FREQUENCY_ATTR_NO_HT40_PLUS]) c->allowed_bw &= ~HOSTAPD_CHAN_WIDTH_40P;
	if (a[NL80211_FREQUENCY_ATTR_NO_HT40_MINUS]) c->allowed_bw &= ~HOSTAPD_CHAN_WIDTH_40M;
	if (a[NL80211_FREQUENCY_ATTR_NO_80MHZ]) c->allowed_bw &= ~HOSTAPD_CHAN_WIDTH_80;
	if (c->allowed_bw & HOSTAPD_CHAN_WIDTH_40P) c->flag |= HOSTAPD_CHAN_HT40PLUS;
	if (c->allowed_bw & HOSTAPD_CHAN_WIDTH_40M) c->flag |= HOSTAPD_CHAN_HT40MINUS;
	if (c->allowed_bw & HOSTAPD_CHAN_WIDTH_80) c->flag |= HOSTAPD_CHAN_VHT_80MHZ_SUBCHANNEL;
	if (a[NL80211_FREQUENCY_ATTR_MAX_TX_POWER]) {
		if (bcm_nla_size(a[NL80211_FREQUENCY_ATTR_MAX_TX_POWER]) != 4) return -EBADMSG;
		c->max_tx_power = (int)bcm_nla_u32(a[NL80211_FREQUENCY_ATTR_MAX_TX_POWER]) / 100;
	}
	return 0;
}
static int rate(void *context, unsigned index, const void *data, size_t length)
{
	struct hostapd_hw_modes *mode = context;
	const struct nlattr *a[NL80211_BITRATE_ATTR_MAX + 1];
	(void)index;
	if (bcm_nla_parse(data, length, a, ARRAY_SIZE(a)) ||
	    bcm_nla_size(a[NL80211_BITRATE_ATTR_RATE]) != 4) return -EBADMSG;
	int value = bcm_nla_u32(a[NL80211_BITRATE_ATTR_RATE]);
	for (int i = 0; i < mode->num_rates; ++i) if (mode->rates[i] == value) return 0;
	if (mode->num_rates >= 32) return -EOVERFLOW;
	void *next = os_realloc_array(mode->rates, mode->num_rates + 1, sizeof(*mode->rates));
	if (!next) return -ENOMEM;
	mode->rates = next; mode->rates[mode->num_rates++] = value;
	return 0;
}
static int he(void *context, unsigned index, const void *data, size_t length)
{
	struct hostapd_hw_modes *mode = context;
	const struct nlattr *a[NL80211_BAND_IFTYPE_ATTR_MAX + 1], *types[NL80211_IFTYPE_MAX + 1];
	(void)index;
	if (bcm_nla_parse(data, length, a, ARRAY_SIZE(a)) || !a[NL80211_BAND_IFTYPE_ATTR_IFTYPES] ||
	    bcm_nla_parse(bcm_nla_value(a[NL80211_BAND_IFTYPE_ATTR_IFTYPES]),
	                  bcm_nla_size(a[NL80211_BAND_IFTYPE_ATTR_IFTYPES]), types, ARRAY_SIZE(types)))
		return -EBADMSG;
	/* DHD exports PHY capabilities under STATION, not a Linux NAN interface.
	 * Reading that capability does not configure or activate a station BSS. */
	if (!types[NL80211_IFTYPE_STATION] || !a[NL80211_BAND_IFTYPE_ATTR_HE_CAP_PHY]) return 0;
	struct he_capabilities next = {0};
	size_t mcs = bcm_nla_size(a[NL80211_BAND_IFTYPE_ATTR_HE_CAP_MCS_SET]);
	size_t ppe = bcm_nla_size(a[NL80211_BAND_IFTYPE_ATTR_HE_CAP_PPE]);
	if (bcm_nla_size(a[NL80211_BAND_IFTYPE_ATTR_HE_CAP_MAC]) != sizeof(next.mac_cap) ||
	    bcm_nla_size(a[NL80211_BAND_IFTYPE_ATTR_HE_CAP_PHY]) != sizeof(next.phy_cap) ||
	    (mcs != 4 && mcs != 8 && mcs != 12) || ppe > sizeof(next.ppet)) return -EBADMSG;
	next.he_supported = true;
	os_memcpy(next.mac_cap, bcm_nla_value(a[NL80211_BAND_IFTYPE_ATTR_HE_CAP_MAC]), sizeof(next.mac_cap));
	os_memcpy(next.phy_cap, bcm_nla_value(a[NL80211_BAND_IFTYPE_ATTR_HE_CAP_PHY]), sizeof(next.phy_cap));
	os_memset(next.mcs, 0xff, sizeof(next.mcs));
	os_memcpy(next.mcs, bcm_nla_value(a[NL80211_BAND_IFTYPE_ATTR_HE_CAP_MCS_SET]), mcs);
	if (ppe) os_memcpy(next.ppet, bcm_nla_value(a[NL80211_BAND_IFTYPE_ATTR_HE_CAP_PPE]), ppe);
	mode->he_capab[IEEE80211_MODE_INFRA] = next;
	mode->flags |= HOSTAPD_MODE_FLAG_HE_INFO_KNOWN;
	return 0;
}
static int band(void *context, unsigned index, const void *data, size_t length)
{
	struct bcm_phy *phy = context;
	const struct nlattr *a[NL80211_BAND_ATTR_MAX + 1];
	if (index != NL80211_BAND_2GHZ && index != NL80211_BAND_5GHZ) return 0;
	unsigned b = index == NL80211_BAND_5GHZ;
	struct hostapd_hw_modes *mode = &phy->band[b];
	if (bcm_nla_parse(data, length, a, ARRAY_SIZE(a))) return -EBADMSG;
	phy->present[b] = true;
	mode->mode = b ? HOSTAPD_MODE_IEEE80211A : HOSTAPD_MODE_IEEE80211G;
	if (a[NL80211_BAND_ATTR_HT_CAPA]) {
		if (bcm_nla_size(a[NL80211_BAND_ATTR_HT_CAPA]) != 2) return -EBADMSG;
		os_memcpy(&mode->ht_capab, bcm_nla_value(a[NL80211_BAND_ATTR_HT_CAPA]), 2);
		mode->flags |= HOSTAPD_MODE_FLAG_HT_INFO_KNOWN;
	}
	if (a[NL80211_BAND_ATTR_HT_MCS_SET]) {
		if (bcm_nla_size(a[NL80211_BAND_ATTR_HT_MCS_SET]) != sizeof(mode->mcs_set)) return -EBADMSG;
		os_memcpy(mode->mcs_set, bcm_nla_value(a[NL80211_BAND_ATTR_HT_MCS_SET]), sizeof(mode->mcs_set));
		phy->ht[b] = !!mode->mcs_set[0];
	}
	if (a[NL80211_BAND_ATTR_HT_AMPDU_FACTOR]) {
		if (bcm_nla_size(a[NL80211_BAND_ATTR_HT_AMPDU_FACTOR]) != 1) return -EBADMSG;
		mode->a_mpdu_params = (mode->a_mpdu_params & ~3) |
			(*(const u8 *)bcm_nla_value(a[NL80211_BAND_ATTR_HT_AMPDU_FACTOR]) & 3);
	}
	if (a[NL80211_BAND_ATTR_HT_AMPDU_DENSITY]) {
		if (bcm_nla_size(a[NL80211_BAND_ATTR_HT_AMPDU_DENSITY]) != 1) return -EBADMSG;
		mode->a_mpdu_params = (mode->a_mpdu_params & ~(7 << 2)) |
			((*(const u8 *)bcm_nla_value(a[NL80211_BAND_ATTR_HT_AMPDU_DENSITY]) & 7) << 2);
	}
	if (a[NL80211_BAND_ATTR_VHT_CAPA]) {
		if (bcm_nla_size(a[NL80211_BAND_ATTR_VHT_CAPA]) != 4) return -EBADMSG;
		mode->vht_capab = bcm_nla_u32(a[NL80211_BAND_ATTR_VHT_CAPA]);
		mode->flags |= HOSTAPD_MODE_FLAG_VHT_INFO_KNOWN;
	}
	if (a[NL80211_BAND_ATTR_VHT_MCS_SET]) {
		if (bcm_nla_size(a[NL80211_BAND_ATTR_VHT_MCS_SET]) != sizeof(mode->vht_mcs_set)) return -EBADMSG;
		os_memcpy(mode->vht_mcs_set, bcm_nla_value(a[NL80211_BAND_ATTR_VHT_MCS_SET]), sizeof(mode->vht_mcs_set));
		phy->vht[b] = (WPA_GET_LE16(mode->vht_mcs_set) & 3) != 3;
	}
	int result = nested(a[NL80211_BAND_ATTR_FREQS], frequency, mode);
	if (!result) result = nested(a[NL80211_BAND_ATTR_RATES], rate, mode);
	if (!result) result = nested(a[NL80211_BAND_ATTR_IFTYPE_DATA], he, mode);
	return result;
}
static int interface_type(void *context, unsigned type, const void *data, size_t length)
{
	struct bcm_phy *phy = context;
	(void)data;
	if (type >= 32 || length) return -EBADMSG;
	phy->interface_types |= 1U << type;
	return 0;
}
static int vendor_command(void *context, unsigned index, const void *data, size_t length)
{
	struct bcm_phy *phy = context;
	struct nl80211_vendor_cmd_info info;
	(void)index;
	if (length != sizeof(info)) return -EBADMSG;
	os_memcpy(&info, data, sizeof(info));
	if (info.vendor_id != BCM_NAN_VENDOR_OUI || info.subcmd != BCM_NAN_VENDOR_SUBCMD)
		return -EOPNOTSUPP;
	++phy->vendor_commands;
	return 0;
}
static int supported_command(void *context, unsigned index, const void *data, size_t length)
{
	struct bcm_phy *phy = context;
	u32 command;
	(void)index;
	if (length != sizeof(command)) return -EBADMSG;
	os_memcpy(&command, data, sizeof(command));
	switch (command) {
	case NL80211_CMD_NEW_INTERFACE: case NL80211_CMD_SET_INTERFACE:
	case NL80211_CMD_TRIGGER_SCAN: case NL80211_CMD_START_SCHED_SCAN:
	case NL80211_CMD_CONNECT: case NL80211_CMD_AUTHENTICATE: case NL80211_CMD_ASSOCIATE:
	case NL80211_CMD_START_AP: case NL80211_CMD_JOIN_IBSS: case NL80211_CMD_JOIN_MESH:
	case NL80211_CMD_START_P2P_DEVICE: case NL80211_CMD_REMAIN_ON_CHANNEL:
	case NL80211_CMD_FRAME: case NL80211_CMD_TDLS_MGMT: case NL80211_CMD_TDLS_OPER:
		phy->other_mode_commands = true;
	}
	return 0;
}
static int wiphy(void *context, const void *data, size_t length)
{
	struct bcm_phy *phy = context;
	const struct nlattr *a[NL80211_ATTR_MAX + 1];
	if (bcm_nla_parse(data, length, a, ARRAY_SIZE(a))) return -EBADMSG;
	if (a[NL80211_ATTR_REG_ALPHA2]) {
		if (bcm_nla_size(a[NL80211_ATTR_REG_ALPHA2]) != 3 ||
		    ((const u8 *)bcm_nla_value(a[NL80211_ATTR_REG_ALPHA2]))[2]) return -EBADMSG;
		os_memcpy(phy->country, bcm_nla_value(a[NL80211_ATTR_REG_ALPHA2]), 2);
	}
	int result = nested(a[NL80211_ATTR_SUPPORTED_IFTYPES], interface_type, phy);
	if (!result) result = nested(a[NL80211_ATTR_VENDOR_DATA], vendor_command, phy);
	if (!result) result = nested(a[NL80211_ATTR_SUPPORTED_COMMANDS], supported_command, phy);
	if (!result) result = nested(a[NL80211_ATTR_WIPHY_BANDS], band, phy);
	return result;
}
void bcm_phy_clear(struct bcm_phy *phy)
{
	for (unsigned i = 0; i < ARRAY_SIZE(phy->band); ++i) {
		os_free(phy->band[i].channels); os_free(phy->band[i].rates);
	}
	os_memset(phy, 0, sizeof(*phy));
}
int bcm_phy_read(struct bcm_link *link, struct bcm_phy *phy)
{
	struct bcm_phy next = {.country = "00"};
	int result = bcm_link_wiphy(link, wiphy, &next);
	if (!result && (next.interface_types != (1U << NL80211_IFTYPE_STATION) ||
	                next.vendor_commands != 1 || next.other_mode_commands)) {
		wpa_printf(MSG_ERROR, "BCM NAN: driver does not provide exclusive native NAN control");
		result = -EOPNOTSUPP;
	}
	if (!result) result = bcm_link_country(link, next.country);
	if (!result && (!next.present[0] || !next.ht[0] || !next.band[0].num_channels ||
	                !next.band[0].num_rates)) result = -EOPNOTSUPP;
	if (result) { bcm_phy_clear(&next); return result; }
	bcm_phy_clear(phy); *phy = next;
	return 0;
}
struct hostapd_hw_modes *bcm_phy_copy(const struct bcm_phy *phy, u16 *count)
{
	struct hostapd_hw_modes *modes = os_calloc(2, sizeof(*modes));
	if (!modes) return NULL;
	*count = 0;
	for (unsigned b = 0; b < 2; ++b) {
		if (!phy->present[b]) continue;
		struct hostapd_hw_modes *m = &modes[(*count)++];
		*m = phy->band[b];
		m->channels = os_memdup(phy->band[b].channels, m->num_channels * sizeof(*m->channels));
		m->rates = os_memdup(phy->band[b].rates, m->num_rates * sizeof(*m->rates));
		if (!m->channels || !m->rates) {
			for (unsigned i = 0; i < *count; ++i) { os_free(modes[i].channels); os_free(modes[i].rates); }
			os_free(modes); *count = 0; return NULL;
		}
	}
	return modes;
}
static const struct hostapd_channel_data *find_channel(const struct hostapd_hw_modes *mode, int frequency)
{
	for (int i = 0; i < mode->num_channels; ++i)
		if (mode->channels[i].freq == frequency) return &mode->channels[i];
	return NULL;
}
int bcm_phy_chanspec(const struct bcm_phy *phy, const struct nan_schedule_channel *channel, u16 *chanspec)
{
	unsigned b = channel->freq > 3000;
	const struct hostapd_hw_modes *mode = &phy->band[b];
	const struct hostapd_channel_data *primary = find_channel(mode, channel->freq);
	int center = channel->center_freq1 ? channel->center_freq1 : channel->freq;
	int number = channel_number(center), width = channel->bandwidth;
	u32 forbidden = HOSTAPD_CHAN_DISABLED | HOSTAPD_CHAN_NO_IR | HOSTAPD_CHAN_RADAR;
	if (!primary || !number || !phy->ht[b] || channel->center_freq2 ||
	    (width != 20 && width != 40 && width != 80) || (!b && width == 80) ||
	    (width > 20 && !(mode->ht_capab & HT_CAP_INFO_SUPP_CHANNEL_WIDTH_SET)) ||
	    (width == 80 && !phy->vht[b] && !mode->he_capab[IEEE80211_MODE_INFRA].he_supported))
		return -EOPNOTSUPP;
	if (width == 80 && center != 5210 && center != 5290 && center != 5530 &&
	    center != 5610 && center != 5690 && center != 5775 && center != 5855)
		return -EINVAL;
	int lowest = center - width / 2 + 10, side = (channel->freq - lowest) / 20;
	if (channel->freq < lowest || channel->freq >= lowest + width ||
	    (channel->freq - lowest) % 20) return -EINVAL;
	u32 required = width == 20 ? HOSTAPD_CHAN_WIDTH_20 : width == 40 ?
		(side ? HOSTAPD_CHAN_WIDTH_40M : HOSTAPD_CHAN_WIDTH_40P) : HOSTAPD_CHAN_WIDTH_80;
	if (!(primary->allowed_bw & required)) return -EOPNOTSUPP;
	for (int frequency = lowest; frequency < lowest + width; frequency += 20) {
		const struct hostapd_channel_data *c = find_channel(mode, frequency);
		if (!c || c->flag & forbidden || !(c->allowed_bw & HOSTAPD_CHAN_WIDTH_20) ||
		    (width == 80 && !(c->allowed_bw & HOSTAPD_CHAN_WIDTH_80)))
			return -EACCES;
	}
	*chanspec = (b ? 0xc000 : 0) | (width == 20 ? 0x1000 : width == 40 ? 0x1800 : 0x2000) |
		(side << 8) | number;
	return 0;
}
int bcm_chanspec_frequency(u16 chanspec)
{
	unsigned band = chanspec & 0xc000, width = chanspec & 0x3800, side = (chanspec >> 8) & 7;
	int center = chanspec & 0xff, channel;
	if ((band != 0 && band != 0xc000) || !center) return 0;
	if (width == 0x1000 && !side) channel = center;
	else if (width == 0x1800 && side < 2) channel = center - 2 + 4 * side;
	else if (width == 0x2000 && side < 4 && band) channel = center - 6 + 4 * side;
	else return 0;
	if (!band && channel == 14) return 2484;
	if (channel < 1 || (!band && channel > 14)) return 0;
	return (band ? 5000 : 2407) + channel * 5;
}

int bcm_phy_nan_capability(const struct bcm_phy *phy, struct nan_capa *n)
{
	/* The shared hostap contract has one PHY capability set. This backend
	 * uses the actual 5 GHz data PHY when available; channels remain checked
	 * individually against the wiphy and the firmware CLM. */
	unsigned band = phy->present[1] && phy->ht[1] ? 1 : 0;
	const struct hostapd_hw_modes *mode = &phy->band[band];
	if (!phy->ht[band]) return -ENODATA;
	unsigned rx = 0;
	while (rx < 4 && mode->mcs_set[rx]) ++rx;
	/* HT TX MCS must be defined; when TX/RX differ the encoded TX count
	 * is bits 2..3 plus one. Never infer 2x2 merely from the chip name. */
	if (!rx || rx > 2 || !(mode->mcs_set[12] & 1)) return -EOPNOTSUPP;
	unsigned tx = mode->mcs_set[12] & 2 ? ((mode->mcs_set[12] >> 2) & 3U) + 1U : rx;
	if (tx > 2) return -EOPNOTSUPP;
	os_memset(n, 0, sizeof(*n));
	n->drv_flags = WPA_DRIVER_FLAGS_NAN_SUPPORT_SYNC_CONFIG |
		WPA_DRIVER_FLAGS_NAN_SUPPORT_USERSPACE_DE | WPA_DRIVER_FLAGS_NAN_SUPPORT_NDP |
		WPA_DRIVER_FLAGS_NAN_SUPPORT_BEACON_PROT;
	if (phy->present[0] && phy->present[1]) n->drv_flags |= WPA_DRIVER_FLAGS_NAN_SUPPORT_DUAL_BAND;
	n->num_radios = 1; n->sched_chans = MAX_NUM_NAN_SCHEDULE_CHANNELS;
	n->slot_duration = BCM_NAN_HOST_SCHEDULE_SLOT_TU;
	n->schedule_period = BCM_NAN_HOST_SCHEDULE_PERIOD_TU;
	/* The native A2 device-capability setter stores 5000 microseconds. */
	n->max_channel_switch_time = 5000; n->num_antennas = (rx << 4) | tx;
	n->dev_capa = NAN_DEV_CAPA_NDPE_ATTR_SUPP;
	n->ht_capab = mode->ht_capab; n->ht_ampdu_params = mode->a_mpdu_params;
	os_memcpy(n->ht_mcs_set, mode->mcs_set, sizeof(n->ht_mcs_set));
	if (phy->vht[band]) {
		n->vht_valid = true;
		n->vht_capab = mode->vht_capab & ~(VHT_CAP_SUPP_CHAN_WIDTH_MASK | VHT_CAP_SHORT_GI_160 |
			VHT_CAP_MU_BEAMFORMER_CAPABLE | VHT_CAP_MU_BEAMFORMEE_CAPABLE | VHT_CAP_VHT_TXOP_PS);
		os_memcpy(n->vht_mcs_set, mode->vht_mcs_set, sizeof(n->vht_mcs_set));
		n->op_modes |= NAN_DEV_CAPA_OP_MODE_PHY_MODE_VHT;
	}
	const struct he_capabilities *he = &mode->he_capab[IEEE80211_MODE_INFRA];
	if (he->he_supported) {
		n->he_valid = true; n->he_capab = *he;
		/* Single NAN has no infrastructure TWT agreement or MU scheduler.
		 * Preserve actual SU/MCS/PPE bytes while excluding wider schedules. */
		n->he_capab.mac_cap[0] &= ~6U;
		n->he_capab.phy_cap[0] &= ~(HE_PHYCAP_CHANNEL_WIDTH_SET_160MHZ_IN_5G |
			HE_PHYCAP_CHANNEL_WIDTH_SET_80PLUS80MHZ_IN_5G);
		n->he_capab.phy_cap[HE_PHYCAP_MU_BEAMFORMER_CAPAB_IDX] &= ~HE_PHYCAP_MU_BEAMFORMER_CAPAB;
		os_memset(n->he_capab.mcs + 4, 0xff, sizeof(n->he_capab.mcs) - 4);
		n->op_modes |= NAN_DEV_CAPA_OP_MODE_PHY_MODE_HE;
	}
	return 0;
}

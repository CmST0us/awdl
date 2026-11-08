/* SPDX-License-Identifier: BSD-3-Clause */
#include "stations.h"
#include "common/ieee802_11_defs.h"
#include <limits.h>

struct bcm_station *bcm_station_find(struct bcm_stations *stations, const u8 *local, const u8 *peer)
{
	for (unsigned i = 0; i < ARRAY_SIZE(stations->entry); ++i) {
		struct bcm_station *s = &stations->entry[i];
		if (s->owned && !os_memcmp(s->local, local, 6) && !os_memcmp(s->peer, peer, 6)) return s;
	}
	return NULL;
}
bool bcm_station_data_for(const struct bcm_stations *stations, const u8 *nmi)
{
	for (unsigned i = 0; i < ARRAY_SIZE(stations->entry); ++i) {
		const struct bcm_station *s = &stations->entry[i];
		if (s->owned && s->data && (!nmi || !os_memcmp(s->nmi, nmi, 6))) return true;
	}
	return false;
}
static int capabilities(const struct hostapd_sta_add_params *p, u8 ies[BCM_NAN_HOST_DATA_IE_MAX],
                         size_t *length)
{
	*length = 0;
	if (!p->ht_capabilities) return p->vht_capabilities || p->he_capab ? -EINVAL : 0;
	/* Preserve the peer's actual negotiated PHY bytes. No local fallback
	 * or fabricated HT/VHT/HE information is used for the data station. */
	ies[(*length)++] = WLAN_EID_HT_CAP;
	ies[(*length)++] = sizeof(*p->ht_capabilities);
	os_memcpy(ies + *length, p->ht_capabilities, sizeof(*p->ht_capabilities));
	*length += sizeof(*p->ht_capabilities);
	if (p->vht_capabilities) {
		ies[(*length)++] = WLAN_EID_VHT_CAP;
		ies[(*length)++] = sizeof(*p->vht_capabilities);
		os_memcpy(ies + *length, p->vht_capabilities, sizeof(*p->vht_capabilities));
		*length += sizeof(*p->vht_capabilities);
	}
	if (p->he_capab) {
		if (p->he_capab_len < 21 || p->he_capab_len > 254 ||
		    *length + 3 + p->he_capab_len > BCM_NAN_HOST_DATA_IE_MAX) return -EINVAL;
		ies[(*length)++] = WLAN_EID_EXTENSION;
		ies[(*length)++] = p->he_capab_len + 1;
		ies[(*length)++] = WLAN_EID_EXT_HE_CAPABILITIES;
		os_memcpy(ies + *length, p->he_capab, p->he_capab_len); *length += p->he_capab_len;
	} else if (p->he_capab_len) return -EINVAL;
	return 0;
}
static size_t request(u8 *wire, const struct bcm_station *s, unsigned op)
{
	size_t length = s->data ? BCM_NAN_HOST_DATA_STATION_SIZE : BCM_NAN_HOST_STATION_SIZE;
	if (s->data && op == BCM_NAN_HOST_ADD) length += s->ies_length;
	bcm_header(wire, op, length);
	if (op == BCM_NAN_HOST_ADD)
		WPA_PUT_LE16(wire + 6, s->flags & WPA_STA_MFP ? BCM_NAN_HOST_MFP : 0);
	if (s->data) {
		os_memcpy(wire + 8, s->nmi, 6); os_memcpy(wire + 14, s->local, 6);
		os_memcpy(wire + 20, s->peer, 6); wire[26] = s->ndp_id;
		if (op == BCM_NAN_HOST_ADD) {
			WPA_PUT_LE16(wire + 28, s->ies_length);
			os_memcpy(wire + 32, s->ies, s->ies_length);
		}
	} else os_memcpy(wire + 8, s->peer, 6);
	return length;
}
static struct bcm_result command(struct bcm_link *link, const struct bcm_station *s, unsigned op)
{
	u8 wire[BCM_NAN_HOST_DATA_STATION_SIZE + BCM_NAN_HOST_DATA_IE_MAX];
	size_t length = request(wire, s, op);
	return bcm_set(link, s->data ? BCM_NAN_HOST_DATA_STATION_COMMAND :
		BCM_NAN_HOST_STATION_COMMAND, wire, length);
}
int bcm_station_inactivity(struct bcm_stations *stations, struct bcm_link *link,
                            const u8 *local, const u8 *peer)
{
	struct bcm_station *s = bcm_station_find(stations, local, peer);
	if (!s || !s->ready || !s->data) return -ENOENT;
	u8 wire[BCM_NAN_HOST_DATA_STATION_SIZE], output[4];
	size_t length = request(wire, s, BCM_NAN_HOST_GET_INACTIVITY);
	struct bcm_result result = bcm_get(link, BCM_NAN_HOST_DATA_STATION_COMMAND,
		wire, length, output, sizeof(output));
	int error = bcm_result_errno(result);
	if (error) return error;
	if (result.used != sizeof(output)) return -EPROTO;
	u32 seconds = WPA_GET_LE32(output);
	return seconds > INT_MAX ? INT_MAX : (int)seconds;
}
int bcm_station_add(struct bcm_stations *stations, struct bcm_link *link, const u8 *local,
                    bool data, const struct hostapd_sta_add_params *p)
{
	if (!p || !bcm_unicast(local) || !bcm_unicast(p->addr) ||
	    !os_memcmp(local, p->addr, 6)) return -EINVAL;
	struct bcm_station *s = bcm_station_find(stations, local, p->addr);
	struct bcm_station next = {.owned = true, .data = data};
	os_memcpy(next.local, local, 6); os_memcpy(next.peer, p->addr, 6);
	if (data) {
		u32 required = WPA_STA_AUTHENTICATED | WPA_STA_ASSOCIATED | WPA_STA_MFP;
		if (p->set || s || !bcm_unicast(p->nmi_addr) || !p->nan_ndp_id ||
		    (p->flags & required) != required || p->flags & WPA_STA_AUTHORIZED ||
		    !os_memcmp(local, p->nmi_addr, 6))
			return -EOPNOTSUPP;
		/* A peer's NMI and NDI may be equal; local BSS plus peer MAC
		 * distinguishes its control and data station owners. */
		struct bcm_station *nmi = NULL;
		for (unsigned i = 0; i < ARRAY_SIZE(stations->entry); ++i) {
			struct bcm_station *peer = &stations->entry[i];
			if (peer->owned && peer->ready && !peer->data &&
			    !os_memcmp(peer->peer, p->nmi_addr, 6)) nmi = peer;
		}
		if (!nmi || !nmi->ies_length || bcm_station_data_for(stations, p->nmi_addr)) return -ENODATA;
		next.flags = required; next.ndp_id = p->nan_ndp_id;
		os_memcpy(next.nmi, p->nmi_addr, 6);
		next.ies_length = nmi->ies_length; os_memcpy(next.ies, nmi->ies, nmi->ies_length);
	} else {
		if (p->nmi_addr || p->nan_ndp_id || p->flags & ~(WPA_STA_AUTHENTICATED |
		    WPA_STA_ASSOCIATED | WPA_STA_AUTHORIZED | WPA_STA_MFP)) return -EOPNOTSUPP;
		if (s) {
			if (!s->ready) return -EBUSY;
			next = *s;
		} else if (p->set) return -ENOENT;
		next.flags |= p->flags;
		u8 ies[BCM_NAN_HOST_DATA_IE_MAX]; size_t length;
		int error = capabilities(p, ies, &length);
		if (error) return error;
		if (length) {
			if (bcm_station_data_for(stations, p->addr) &&
			    (length != next.ies_length || os_memcmp(ies, next.ies, length))) return -EBUSY;
			os_memcpy(next.ies, ies, length); next.ies_length = length;
		}
	}
	if (!s) {
		for (unsigned i = 0; i < ARRAY_SIZE(stations->entry); ++i)
			if (!stations->entry[i].owned) { s = &stations->entry[i]; break; }
		if (!s) return -ENOSPC;
	}
	/* Preserve the exact attempted identities even if the firmware can
	 * only report a partial allocation or a transport reply is lost. */
	*s = next; s->ready = false;
	struct bcm_result result = command(link, s, BCM_NAN_HOST_ADD);
	int error = bcm_result_errno(result); s->ready = !error; return error;
}
int bcm_station_remove(struct bcm_stations *stations, struct bcm_link *link,
                       const u8 *local, const u8 *peer)
{
	struct bcm_station *s = bcm_station_find(stations, local, peer);
	if (!s) return 0;
	if (!s->data && bcm_station_data_for(stations, s->peer)) return -EBUSY;
	s->ready = false;
	struct bcm_result result = command(link, s, BCM_NAN_HOST_REMOVE);
	int error = bcm_result_errno(result);
	if (!error) forced_memzero(s, sizeof(*s));
	return error;
}
int bcm_station_flags(struct bcm_stations *stations, struct bcm_link *link, const u8 *local,
                      const u8 *peer, unsigned set, unsigned mask)
{
	struct bcm_station *s = bcm_station_find(stations, local, peer);
	if (!s || !s->ready) return -ENOENT;
	unsigned next = (s->flags & mask) | set;
	if ((next ^ s->flags) & ~(WPA_STA_AUTHORIZED | WPA_STA_MFP)) return -EOPNOTSUPP;
	if (s->data) {
		if (!(next & WPA_STA_MFP)) return -EOPNOTSUPP;
		if (!((next ^ s->flags) & WPA_STA_AUTHORIZED)) return 0;
		unsigned op = next & WPA_STA_AUTHORIZED ? BCM_NAN_HOST_AUTHORIZE : BCM_NAN_HOST_DEAUTHORIZE;
		struct bcm_result result = command(link, s, op);
		int error = bcm_result_errno(result);
		if (!error) s->flags = next;
		else s->ready = false;
		return error;
	}
	if (next == s->flags) return 0;
	struct bcm_station request = *s; request.flags = next;
	struct bcm_result result = command(link, &request, BCM_NAN_HOST_ADD);
	int error = bcm_result_errno(result);
	if (!error) s->flags = next;
	else s->ready = false; /* Mutation may have happened before the error. */
	return error;
}
void bcm_stations_stopped(struct bcm_stations *stations) { forced_memzero(stations, sizeof(*stations)); }

// SPDX-License-Identifier: GPL-2.0-only
/* AWDL data framing on a firmware-owned native NDI. */
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <asm/unaligned.h>
#include <bcmutils.h>
#include <bcmendian.h>
#include <wl_cfg80211.h>
#include "wl_nan_host.h"
#include "awdl_frame.h"

bool wl_nan_host_awdl_active(struct bcm_cfg80211 *cfg, int ifidx)
{
	struct wl_nan_ndi_state *state;
	unsigned long flags;
	unsigned int i;
	bool active = false;
	if (!cfg || ifidx < 0)
		return false;
	state = &cfg->nan_host_ndi;
	if (!state->initialized)
		return false;
	spin_lock_irqsave(&state->lock, flags);
	for (i = 0; i < ARRAY_SIZE(state->owner); ++i) {
		struct wl_nan_ndi_owner *o = &state->owner[i];
		if (o->ifidx == ifidx && o->awdl_generation &&
		    (o->flags & BCM_NAN_NDI_REGISTERED) && !(o->flags & BCM_NAN_NDI_GONE))
			active = true;
	}
	spin_unlock_irqrestore(&state->lock, flags);
	return active;
}

/* Serialize with NDI commands under cfg->if_sync. Publish intent before the
 * START IOVAR: firmware can return an error after acquiring real resources.
 * Retain ownership until successful STOP or an authoritative empty QUERY. */
int wl_nan_host_awdl_claim(struct bcm_cfg80211 *cfg, const u8 *p)
{
	struct wl_nan_ndi_state *state = &cfg->nan_host_ndi;
	unsigned long flags;
	unsigned int i;
	u64 generation = get_unaligned_le64(p + 8);
	int error = -ENODEV;
	if (!state->initialized || state->stopping)
		return -ENODEV;
	spin_lock_irqsave(&state->lock, flags);
	for (i = 0; i < ARRAY_SIZE(state->owner); ++i) {
		if (state->owner[i].awdl_generation) {
			error = -EBUSY;
			goto out;
		}
	}
	for (i = 0; i < ARRAY_SIZE(state->owner); ++i) {
		struct wl_nan_ndi_owner *o = &state->owner[i];
		if (!memcmp(o->mac, p + 16, ETH_ALEN) &&
		    (o->flags & BCM_NAN_NDI_REGISTERED) &&
		    !(o->flags & (BCM_NAN_NDI_GONE | BCM_NAN_NDI_QUIESCING |
		                  BCM_NAN_NDI_DELETE_SENT | BCM_NAN_NDI_CONFLICT))) {
			o->awdl_generation = generation;
			error = 0;
			break;
		}
	}
out:
	spin_unlock_irqrestore(&state->lock, flags);
	return error;
}

void wl_nan_host_awdl_released(struct bcm_cfg80211 *cfg, u64 generation)
{
	struct wl_nan_ndi_state *state = &cfg->nan_host_ndi;
	unsigned long flags;
	unsigned int i;
	if (!state->initialized)
		return;
	spin_lock_irqsave(&state->lock, flags);
	for (i = 0; i < ARRAY_SIZE(state->owner); ++i)
		if (!generation || state->owner[i].awdl_generation == generation)
			state->owner[i].awdl_generation = 0;
	spin_unlock_irqrestore(&state->lock, flags);
}

/* The A2 normal RX path keeps an unrecognized LLC in an 802.3 payload.
 * Decode before protocol classification/GRO. No private TAP or monitor path. */
int wl_nan_host_awdl_rx(struct bcm_cfg80211 *cfg, int ifidx, struct sk_buff *skb)
{
	u8 addresses[12], protocol[2];
	u16 length;
	if (!wl_nan_host_awdl_active(cfg, ifidx))
		return 0;
	if (!pskb_may_pull(skb, ETH_HLEN))
		return -EINVAL;
	length = get_unaligned_be16(skb->data + 12);
	/* DHD firmware event Ethernet frames are not AWDL MSDUs. */
	if (length == ETHER_TYPE_BRCM)
		return 0;
	if (length >= 1536 || length < BCM_AWDL_MSDU_HEADER_SIZE ||
	    length > skb->len - ETH_HLEN ||
	    !pskb_may_pull(skb, ETH_HLEN + BCM_AWDL_MSDU_HEADER_SIZE))
		return -EPROTO;
	if (!bcm_awdl_msdu_decode(skb->data + ETH_HLEN, length, protocol) ||
	    (((unsigned int)protocol[0] << 8) | protocol[1]) < 1536)
		return -EPROTO;
	if (skb_cow_head(skb, 0) || pskb_trim(skb, ETH_HLEN + length))
		return -ENOMEM;
	memcpy(addresses, skb->data, sizeof(addresses));
	skb_pull(skb, BCM_AWDL_MSDU_HEADER_SIZE);
	memcpy(skb->data, addresses, sizeof(addresses));
	memcpy(skb->data + 12, protocol, sizeof(protocol));
	/* Firmware offsets describe the encapsulated packet, not this one. */
	skb->ip_summed = CHECKSUM_NONE;
	skb->csum = 0;
	skb_clear_hash(skb);
	return 0;
}

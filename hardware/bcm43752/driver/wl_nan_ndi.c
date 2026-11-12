// SPDX-License-Identifier: GPL-2.0-only
/* Event-owned native NAN data interfaces for the pinned A2 firmware. */
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include <asm/unaligned.h>
#include <bcmutils.h>
#include <bcmendian.h>
#include <wl_cfg80211.h>
#include <dhd_linux.h>
#include <wldev_common.h>
#include "wl_nan_host.h"

static void snapshot(struct wl_nan_ndi_state *state,
                     struct wl_nan_ndi_owner *owner, struct wl_nan_ndi_owner *copy)
{
	unsigned long flags;
	spin_lock_irqsave(&state->lock, flags);
	*copy = *owner;
	spin_unlock_irqrestore(&state->lock, flags);
}

static void encode(const struct wl_nan_ndi_owner *owner, u8 *output)
{
	memset(output, 0, BCM_NAN_NDI_STATUS_SIZE);
	put_unaligned_le32(owner->flags, output);
	put_unaligned_le32(owner->ifindex, output + 4);
	output[8] = owner->ifidx;
	output[9] = owner->bssidx;
	memcpy(output + 10, owner->mac, ETH_ALEN);
	memcpy(output + 16, owner->name, IFNAMSIZ);
	put_unaligned_le32(owner->ioctl_result, output + 32);
	put_unaligned_le32(owner->host_result, output + 36);
}

/* Called only by the completion worker, with RTNL then if_sync held. Vendor
 * commands hold wiphy then if_sync and never wait for this worker or RTNL. */
static void finish(struct bcm_cfg80211 *cfg, struct wl_nan_ndi_owner *owner)
{
	struct wl_nan_ndi_state *state = &cfg->nan_host_ndi;
	struct wl_nan_ndi_owner copy;
	unsigned long flags;
	int result;
	bool removed;
	u8 payload[BCM_NAN_NDI_STATUS_SIZE];

	ASSERT_RTNL();
again:
	snapshot(state, owner, &copy);
	if (copy.flags & (BCM_NAN_NDI_CONFLICT | BCM_NAN_NDI_GONE))
		return;
	removed = !!(copy.flags & BCM_NAN_NDI_DEL_SEEN);
	if (!removed && (!(copy.flags & BCM_NAN_NDI_ADD_SEEN) ||
	    (copy.flags & (BCM_NAN_NDI_REGISTERED | BCM_NAN_NDI_DELETE_SENT |
	                   BCM_NAN_NDI_ORPHAN | BCM_NAN_NDI_QUIESCING))))
		return;
	if (removed)
		result = dhd_nan_ndi_remove(cfg->pub, copy.ifidx, copy.bssidx, copy.mac);
	else
		result = dhd_nan_ndi_register(cfg->pub, copy.ifidx, copy.bssidx,
			copy.mac, copy.name, copy.firmware_name);
	spin_lock_irqsave(&state->lock, flags);
	owner->host_result = result < 0 ? result : 0;
	if (result >= 0) {
		if (removed) {
			owner->flags |= BCM_NAN_NDI_GONE;
			owner->flags &= ~BCM_NAN_NDI_REGISTERED;
			owner->ifindex = 0;
		} else {
			owner->flags |= BCM_NAN_NDI_REGISTERED;
			owner->ifindex = result;
			if (!(owner->flags & BCM_NAN_NDI_DEL_SEEN))
				owner->host_result = dhd_nan_ndi_resume(cfg->pub,
					owner->ifidx, owner->bssidx, owner->mac);
		}
	}
	/* IF_DEL can arrive while register_netdevice runs. Finish that actual
 * completion immediately, never publish it as a live NDI. */
	encode(owner, payload);
	copy = *owner;
	spin_unlock_irqrestore(&state->lock, flags);
	wl_nan_host_send_event(cfg, BCM_NAN_NDI_HOST_EVENT, removed ? 2 : 1,
		copy.host_result < 0 ? -copy.host_result : 0,
		copy.ifidx, copy.bssidx, payload, sizeof(payload), GFP_KERNEL);
	removed = !removed && result >= 0 && (copy.flags & BCM_NAN_NDI_DEL_SEEN);
	if (removed)
		goto again;
}

static void complete_work(struct work_struct *work)
{
	struct wl_nan_ndi_state *state = container_of(work, struct wl_nan_ndi_state, work);
	struct bcm_cfg80211 *cfg = container_of(state, struct bcm_cfg80211, nan_host_ndi);
	int i;
	rtnl_lock();
	mutex_lock(&cfg->if_sync);
	/* A primary-interface shutdown owns DHD's generic teardown under RTNL.
	 * Retain our records until reload; never register an old ADD afterward. */
	if (!READ_ONCE(state->stopping) && cfg->pub->up &&
	    !cfg->pub->stop_in_progress && cfg->pub->busstate == DHD_BUS_DATA)
		for (i = 0; i < ARRAY_SIZE(state->owner); ++i)
			finish(cfg, &state->owner[i]);
	mutex_unlock(&cfg->if_sync);
	rtnl_unlock();
}

void wl_nan_host_ndi_init(struct bcm_cfg80211 *cfg)
{
	struct wl_nan_ndi_state *state = &cfg->nan_host_ndi;
	spin_lock_init(&state->lock);
	spin_lock_init(&state->work_lock);
	INIT_WORK(&state->work, complete_work);
	state->initialized = true;
}

/* Call before DHD tears down any interface, outside RTNL and if_sync. The
 * stopping flag prevents DPC from requeueing work after cancellation. */
void wl_nan_host_ndi_shutdown(struct bcm_cfg80211 *cfg)
{
	struct wl_nan_ndi_state *state;
	unsigned long flags;
	if (!cfg || !cfg->nan_host_ndi.initialized)
		return;
	state = &cfg->nan_host_ndi;
	spin_lock_irqsave(&state->work_lock, flags);
	state->stopping = true;
	spin_unlock_irqrestore(&state->work_lock, flags);
	/* Drain an IF event that passed its stopping check before teardown. */
	spin_lock_irqsave(&state->lock, flags);
	spin_unlock_irqrestore(&state->lock, flags);
	cancel_work_sync(&state->work);
}

void wl_nan_host_ndi_wake(struct bcm_cfg80211 *cfg)
{
	struct wl_nan_ndi_state *state;
	unsigned long flags;
	int i;
	if (!cfg || !cfg->nan_host_ndi.initialized)
		return;
	state = &cfg->nan_host_ndi;
	spin_lock_irqsave(&state->work_lock, flags);
	if (!state->stopping) {
		for (i = 0; i < ARRAY_SIZE(state->owner); ++i) {
			u32 value = READ_ONCE(state->owner[i].flags);
			if (!(value & BCM_NAN_NDI_OWNED) ||
			    (value & (BCM_NAN_NDI_GONE | BCM_NAN_NDI_CONFLICT)))
				continue;
			if ((value & BCM_NAN_NDI_DEL_SEEN) ||
			    ((value & BCM_NAN_NDI_ADD_SEEN) && !(value & BCM_NAN_NDI_REGISTERED))) {
				schedule_work(&state->work);
				break;
			}
		}
	}
	spin_unlock_irqrestore(&state->work_lock, flags);
}

static void create(struct bcm_cfg80211 *cfg, struct wl_nan_ndi_owner *owner)
{
	u8 request[28] = { 0 }, response[WLC_IOCTL_SMLEN] = { 0 };
	struct wl_nan_ndi_state *state = &cfg->nan_host_ndi;
	unsigned long flags;
	u8 ifidx, bssidx;
	int result;

	/* Fixed firmware ABI v3, not a probing request which might itself create
 * an interface. Firmware NAN iftype=3 maps to native BSS type5/subtype11. */
	put_unaligned_le16(3, request);
	put_unaligned_le16(sizeof(request), request + 2);
	put_unaligned_le16(sizeof(request), request + 4);
	request[6] = WL_INTERFACE_TYPE_NAN;
	put_unaligned_le32(WL_INTERFACE_MAC_USE, request + 8);
	memcpy(request + 12, owner->mac, ETH_ALEN);
	result = wldev_iovar_getbuf(bcmcfg_to_prmry_ndev(cfg), "interface_create",
		request, sizeof(request), response, sizeof(response), &cfg->ioctl_buf_sync);
	spin_lock_irqsave(&state->lock, flags);
	owner->ioctl_result = result;
	if (!result) {
		ifidx = response[11];
		bssidx = response[10];
		if (get_unaligned_le16(response) != 2 ||
		    get_unaligned_le16(response + 2) < 28 ||
		    memcmp(response + 4, owner->mac, ETH_ALEN) ||
		    !ifidx || ifidx >= DHD_MAX_IFS || !bssidx ||
		    ((owner->flags & BCM_NAN_NDI_INDEX_KNOWN) &&
		     (owner->ifidx != ifidx || owner->bssidx != bssidx))) {
			WL_ERR(("NAN interface_create reply %*ph; requested %pM, event %u/%u flags=%x\n",
				28, response, owner->mac, owner->ifidx, owner->bssidx, owner->flags));
			owner->flags |= BCM_NAN_NDI_CONFLICT;
			owner->host_result = -EPROTO;
		} else {
			owner->flags |= BCM_NAN_NDI_INDEX_KNOWN;
			owner->ifidx = ifidx;
			owner->bssidx = bssidx;
		}
	}
	spin_unlock_irqrestore(&state->lock, flags);
}

static void remove_native(struct bcm_cfg80211 *cfg, struct wl_nan_ndi_owner *owner)
{
	u8 response[WLC_IOCTL_SMLEN] = { 0 };
	struct wl_nan_ndi_state *state = &cfg->nan_host_ndi;
	unsigned long flags;
	u8 bssidx;
	int result;

	spin_lock_irqsave(&state->lock, flags);
	if (!(owner->flags & BCM_NAN_NDI_INDEX_KNOWN) ||
	    (owner->flags & (BCM_NAN_NDI_CONFLICT | BCM_NAN_NDI_DEL_SEEN)) ||
	    ((owner->flags & BCM_NAN_NDI_DELETE_SENT) && !owner->ioctl_result)) {
		spin_unlock_irqrestore(&state->lock, flags);
		return;
	}
	owner->flags |= BCM_NAN_NDI_DELETE_SENT;
	bssidx = owner->bssidx;
	spin_unlock_irqrestore(&state->lock, flags);
	/* Address the native BSS even when Linux registration failed. TX/rings
	 * have completed; the host netdev remains until matching IF_DEL. */
	result = wldev_iovar_setbuf_bsscfg(bcmcfg_to_prmry_ndev(cfg), "interface_remove",
		NULL, 0, response, sizeof(response), bssidx, &cfg->ioctl_buf_sync);
	spin_lock_irqsave(&state->lock, flags);
	owner->ioctl_result = result;
	spin_unlock_irqrestore(&state->lock, flags);
}

static bool quiesce(struct bcm_cfg80211 *cfg, struct wl_nan_ndi_owner *owner)
{
	struct wl_nan_ndi_state *state = &cfg->nan_host_ndi;
	struct wl_nan_ndi_owner copy;
	unsigned long flags;
	int result, firmware_error;

	spin_lock_irqsave(&state->lock, flags);
	owner->flags |= BCM_NAN_NDI_QUIESCING;
	copy = *owner;
	spin_unlock_irqrestore(&state->lock, flags);
	if (!(copy.flags & BCM_NAN_NDI_INDEX_KNOWN) ||
	    (copy.flags & BCM_NAN_NDI_CONFLICT))
		return false;
	result = dhd_nan_ndi_quiesce(cfg->pub, copy.ifidx, copy.bssidx, copy.mac, &firmware_error);
	spin_lock_irqsave(&state->lock, flags);
	owner->host_result = result;
	owner->ioctl_result = firmware_error;
	if (!result)
		owner->flags |= BCM_NAN_NDI_QUIESCED;
	spin_unlock_irqrestore(&state->lock, flags);
	return !result;
}

static void resume(struct bcm_cfg80211 *cfg, struct wl_nan_ndi_owner *owner)
{
	struct wl_nan_ndi_state *state = &cfg->nan_host_ndi;
	unsigned long flags;
	u32 required = BCM_NAN_NDI_OWNED | BCM_NAN_NDI_INDEX_KNOWN |
		BCM_NAN_NDI_ADD_SEEN | BCM_NAN_NDI_REGISTERED;
	u32 forbidden = BCM_NAN_NDI_DELETE_SENT | BCM_NAN_NDI_DEL_SEEN |
		BCM_NAN_NDI_GONE | BCM_NAN_NDI_CONFLICT | BCM_NAN_NDI_ORPHAN;
	int result = -EBUSY;
	/* The owner lock also serializes IF_DEL. The helper never waits or sends
	 * an IOVAR; it validates the native netdev and real TX/ring ownership. */
	spin_lock_irqsave(&state->lock, flags);
	if ((owner->flags & required) == required && !(owner->flags & forbidden) &&
	    (!(owner->flags & BCM_NAN_NDI_QUIESCING) || (owner->flags & BCM_NAN_NDI_QUIESCED)))
		result = dhd_nan_ndi_resume(cfg->pub, owner->ifidx, owner->bssidx, owner->mac);
	owner->host_result = result;
	if (!result) {
		owner->flags &= ~(BCM_NAN_NDI_QUIESCING | BCM_NAN_NDI_QUIESCED);
		owner->ioctl_result = 0;
	}
	spin_unlock_irqrestore(&state->lock, flags);
}

int wl_nan_host_ndi_command(struct bcm_cfg80211 *cfg, u16 command,
                            const u8 *input, u8 *status)
{
	struct wl_nan_ndi_state *state = &cfg->nan_host_ndi;
	struct wl_nan_ndi_owner *owner = NULL, *available = NULL, copy;
	const char *name = (const char *)input + 8;
	unsigned long flags;
	bool fresh = false;
	int i, result = 0;

	if (command < BCM_NAN_NDI_CREATE || command > BCM_NAN_NDI_RESUME ||
	    !is_valid_ether_addr(input) || input[6] || input[7] ||
	    !memchr(name, 0, IFNAMSIZ) || strchr(name, '%') ||
	    (name[0] && !dev_valid_name(name)) ||
	    (command == BCM_NAN_NDI_CREATE && !name[0]))
		return -EINVAL;
	mutex_lock(&cfg->if_sync);
	spin_lock_irqsave(&state->lock, flags);
	if (READ_ONCE(state->stopping)) {
		result = -ESHUTDOWN;
		goto unlock;
	}
	for (i = 0; i < ARRAY_SIZE(state->owner); ++i) {
		struct wl_nan_ndi_owner *entry = &state->owner[i];
		if ((entry->flags & BCM_NAN_NDI_OWNED) && ether_addr_equal(entry->mac, input))
			owner = entry;
		if (!(entry->flags & BCM_NAN_NDI_OWNED) || (entry->flags & BCM_NAN_NDI_GONE))
			available = entry;
		else if (command == BCM_NAN_NDI_CREATE && !strcmp(entry->name, name) &&
		         !ether_addr_equal(entry->mac, input))
			result = -EEXIST;
	}
	if (result)
		goto unlock;
	if (command == BCM_NAN_NDI_CREATE && (!owner || (owner->flags & BCM_NAN_NDI_GONE))) {
		owner = owner ? owner : available;
		if (!owner) {
			result = -ENOSPC;
			goto unlock;
		}
		memset(owner, 0, sizeof(*owner));
		owner->flags = BCM_NAN_NDI_OWNED;
		memcpy(owner->mac, input, ETH_ALEN);
		strscpy(owner->name, name, sizeof(owner->name));
		fresh = true;
	} else if (!owner) {
		result = -ENOENT;
	} else if (name[0] && strcmp(name, owner->name)) {
		result = -EEXIST;
	} else if (command == BCM_NAN_NDI_DELETE && owner->awdl_generation) {
		/* STOP must drain the firmware peer/schedule before NDI deletion.
		 * A failed START also retains its claim until an empty QUERY. */
		result = -EBUSY;
	}
unlock:
	spin_unlock_irqrestore(&state->lock, flags);
	if (!result) {
		if (fresh)
			create(cfg, owner);
		else if (command == BCM_NAN_NDI_DELETE || command == BCM_NAN_NDI_QUIESCE) {
			snapshot(state, owner, &copy);
			if (!(copy.flags & BCM_NAN_NDI_GONE) && quiesce(cfg, owner) &&
			    command == BCM_NAN_NDI_DELETE)
				remove_native(cfg, owner);
		}
		else if (command == BCM_NAN_NDI_RESUME)
			resume(cfg, owner);
		snapshot(state, owner, &copy);
		encode(&copy, status);
	}
	mutex_unlock(&cfg->if_sync);
	if (!result && command != BCM_NAN_NDI_QUERY)
		wl_nan_host_ndi_wake(cfg);
	return result;
}

bool wl_nan_host_ndi_if_event(struct bcm_cfg80211 *cfg,
                              const wl_event_data_if_t *event,
                              const char *name, const u8 *mac)
{
	struct wl_nan_ndi_state *state;
	struct wl_nan_ndi_owner *owner = NULL, *available = NULL;
	u8 payload[BCM_NAN_NDI_IF_EVENT_SIZE] = { 0 };
	unsigned long flags;
	u32 seen;
	int i, result = 0;

	if (!cfg || !wl_nan_host_supported(cfg) || event->role != WLC_E_IF_ROLE_NAN)
		return false;
	state = &cfg->nan_host_ndi;
	if (!event->ifidx || event->ifidx >= DHD_MAX_IFS || !event->bssidx ||
	    !is_valid_ether_addr(mac) ||
	    (event->opcode != WLC_E_IF_ADD && event->opcode != WLC_E_IF_DEL)) {
		result = -EPROTO;
		goto notify;
	}
	seen = event->opcode == WLC_E_IF_ADD ? BCM_NAN_NDI_ADD_SEEN : BCM_NAN_NDI_DEL_SEEN;
	spin_lock_irqsave(&state->lock, flags);
	if (READ_ONCE(state->stopping)) {
		spin_unlock_irqrestore(&state->lock, flags);
		return true;
	}
	for (i = 0; i < ARRAY_SIZE(state->owner); ++i) {
		struct wl_nan_ndi_owner *entry = &state->owner[i];
		if ((entry->flags & BCM_NAN_NDI_OWNED) && ether_addr_equal(entry->mac, mac))
			owner = entry;
		if (!(entry->flags & BCM_NAN_NDI_OWNED) || (entry->flags & BCM_NAN_NDI_GONE))
			available = entry;
	}
	if (!owner) {
		/* Keep unexpected real firmware ownership recoverable. Do not let the
		 * generic IF_ADD path create a station-mode netdev for a NAN event. */
		owner = available;
		if (!owner) {
			result = -ENOSPC;
			goto unlock;
		}
		memset(owner, 0, sizeof(*owner));
		owner->flags = BCM_NAN_NDI_OWNED | BCM_NAN_NDI_ORPHAN;
		memcpy(owner->mac, mac, ETH_ALEN);
	}
	if (((owner->flags & BCM_NAN_NDI_INDEX_KNOWN) &&
	     (owner->ifidx != event->ifidx || owner->bssidx != event->bssidx)) ||
	    ((owner->flags & BCM_NAN_NDI_DEL_SEEN) && event->opcode == WLC_E_IF_ADD)) {
		result = -EPROTO;
	} else if (!(owner->flags & seen)) {
		result = dhd_nan_ndi_flow_event(cfg->pub, event, mac);
		if (!result) {
			owner->ifidx = event->ifidx;
			owner->bssidx = event->bssidx;
			memcpy(owner->firmware_name, name, IFNAMSIZ);
			owner->firmware_name[IFNAMSIZ - 1] = 0;
			owner->flags |= BCM_NAN_NDI_INDEX_KNOWN | seen;
		}
	}
	if (result) {
		owner->flags |= BCM_NAN_NDI_CONFLICT;
		owner->host_result = result;
	}
unlock:
	spin_unlock_irqrestore(&state->lock, flags);
notify:
	memcpy(payload, event, 5);
	memcpy(payload + 6, mac, ETH_ALEN);
	memcpy(payload + 12, name, IFNAMSIZ);
	wl_nan_host_send_event(cfg, WLC_E_IF, event->opcode, -result,
		event->ifidx, event->bssidx, payload, sizeof(payload), GFP_ATOMIC);
	wl_nan_host_ndi_wake(cfg);
	return true;
}

void wl_nan_host_ndi_flow_completion(struct bcm_cfg80211 *cfg, u8 ifidx,
                                    u16 flowid, u32 status, u32 operation)
{
	struct wl_nan_ndi_state *state;
	u8 payload[8], bssidx = 0;
	unsigned long flags;
	int i;
	if (!cfg || !wl_nan_host_supported(cfg))
		return;
	state = &cfg->nan_host_ndi;
	spin_lock_irqsave(&state->lock, flags);
	for (i = 0; i < ARRAY_SIZE(state->owner); ++i) {
		struct wl_nan_ndi_owner *owner = &state->owner[i];
		if ((owner->flags & BCM_NAN_NDI_INDEX_KNOWN) && owner->ifidx == ifidx &&
		    !(owner->flags & BCM_NAN_NDI_GONE)) {
			memcpy(payload, owner->mac, ETH_ALEN);
			bssidx = owner->bssidx;
			if (status)
				owner->ioctl_result = status;
			break;
		}
	}
	spin_unlock_irqrestore(&state->lock, flags);
	if (!bssidx)
		return;
	put_unaligned_le16(flowid, payload + 6);
	wl_nan_host_send_event(cfg, BCM_NAN_NDI_FLOW_EVENT, operation, status,
		ifidx, bssidx, payload, sizeof(payload), GFP_ATOMIC);
	wl_nan_host_ndi_wake(cfg);
}

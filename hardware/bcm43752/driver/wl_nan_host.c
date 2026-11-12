// SPDX-License-Identifier: GPL-2.0-only
/* Single-command native NAN transport for the pinned BCM43752 A2 image.
 * Firmware owns native radio resources; common hostap owns negotiations. */
#include <linux/slab.h>
#include <net/netlink.h>
#include <asm/unaligned.h>
#include <bcmutils.h>
#include <bcmendian.h>
#include <bcmdevs.h>
#include <wl_cfg80211.h>
#include <dngl_stats.h>
#include <dhd.h>
#include <wldev_common.h>
#include "wl_nan_host.h"
#include "nan_host_abi.h"
#include "awdl_host_abi.h"

/* Reject malformed AWDL messages before they reach the firmware parser.
 * Native resource/channel validation remains in firmware. In particular,
 * passing this check does not advertise AWDL support on an older image. */
static int awdl_request(const u8 *p, size_t length, u16 operation, u16 limit)
{
	u16 op, size;
	if (length < BCM_AWDL_HEADER_SIZE ||
	    get_unaligned_le16(p) != BCM_AWDL_ABI_VERSION ||
	    get_unaligned_le16(p + 4) != length || get_unaligned_le16(p + 6))
		return -EINVAL;
	op = get_unaligned_le16(p + 2);
	if (op == BCM_AWDL_QUERY)
		return operation == BCM_NAN_VENDOR_GET &&
			limit == BCM_AWDL_STATUS_SIZE && length == BCM_AWDL_HEADER_SIZE ? 0 : -EINVAL;
	if (operation != BCM_NAN_VENDOR_SET || limit || !get_unaligned_le64(p + 8))
		return -EINVAL;
	switch (op) {
	case BCM_AWDL_START:
		size = BCM_AWDL_START_SIZE;
		break;
	case BCM_AWDL_STOP:
		size = BCM_AWDL_HEADER_SIZE;
		break;
	case BCM_AWDL_PLAN:
		size = BCM_AWDL_PLAN_SIZE;
		break;
	case BCM_AWDL_WINDOW:
		size = BCM_AWDL_WINDOW_SIZE;
		break;
	case BCM_AWDL_CANCEL_TX:
		if (length != BCM_AWDL_CANCEL_TX_SIZE || !get_unaligned_le64(p + 16))
			return -EINVAL;
		return 0;
	case BCM_AWDL_PEER:
		if (length < BCM_AWDL_PEER_HEADER_SIZE ||
		    get_unaligned_le16(p + 22) > BCM_AWDL_PEER_IE_MAX ||
		    get_unaligned_le16(p + 22) != length - BCM_AWDL_PEER_HEADER_SIZE)
			return -EINVAL;
		return 0;
	case BCM_AWDL_TX:
		if (length < BCM_AWDL_TX_HEADER_SIZE + 36 ||
		    length > BCM_AWDL_TX_HEADER_SIZE + BCM_AWDL_TX_FRAME_MAX ||
		    !get_unaligned_le64(p + 16) || !get_unaligned_le16(p + 24) ||
		    !get_unaligned_le64(p + 28) ||
		    get_unaligned_le16(p + 26) != length - BCM_AWDL_TX_HEADER_SIZE)
			return -EINVAL;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
	return length == size ? 0 : -EINVAL;
}

static int awdl_event(const u8 *p, size_t length)
{
	u16 kind, flags;
	if (length < BCM_AWDL_EVENT_HEADER_SIZE ||
	    get_unaligned_le16(p) != BCM_AWDL_ABI_VERSION ||
	    get_unaligned_le16(p + 4) != length ||
	    get_unaligned_le16(p + 38) != length - BCM_AWDL_EVENT_HEADER_SIZE ||
	    !get_unaligned_le64(p + 8))
		return -EPROTO;
	kind = get_unaligned_le16(p + 2);
	flags = get_unaligned_le16(p + 6);
	if (kind < BCM_AWDL_RX || kind > BCM_AWDL_SENDER ||
	    (flags & ~BCM_AWDL_TX_ACK) || (flags && kind != BCM_AWDL_TX_DONE))
		return -EPROTO;
	if (kind == BCM_AWDL_RX) {
		if (length < BCM_AWDL_EVENT_HEADER_SIZE + 24 ||
		    length > BCM_AWDL_EVENT_HEADER_SIZE + BCM_AWDL_TX_FRAME_MAX ||
		    get_unaligned_le64(p + 24) || get_unaligned_le32(p + 32) ||
		    !get_unaligned_le16(p + 36))
			return -EPROTO;
	} else if (kind == BCM_AWDL_SENDER) {
		if (length != BCM_AWDL_EVENT_HEADER_SIZE + ETH_ALEN ||
		    get_unaligned_le32(p + 32) || get_unaligned_le16(p + 36) ||
		    !is_valid_ether_addr(p + BCM_AWDL_EVENT_HEADER_SIZE))
			return -EPROTO;
	} else if (length != BCM_AWDL_EVENT_HEADER_SIZE) {
		return -EPROTO;
	}
	if ((kind == BCM_AWDL_TX_DONE && !get_unaligned_le64(p + 24)) ||
	    (kind != BCM_AWDL_TX_DONE && get_unaligned_le64(p + 24)))
		return -EPROTO;
	if ((kind == BCM_AWDL_CHANNEL_OPEN || kind == BCM_AWDL_CHANNEL_CLOSED || kind == BCM_AWDL_NAN_DW) &&
	    (!get_unaligned_le16(p + 36) || get_unaligned_le32(p + 32)))
		return -EPROTO;
	if (kind == BCM_AWDL_STOPPED && get_unaligned_le32(p + 32))
		return -EPROTO;
	return 0;
}

bool wl_nan_host_device(dhd_pub_t *dhd)
{
	return dhd && dhd_bus_chip_id(dhd) == BCM43752_CHIP_ID &&
		dhd_bus_chiprev_id(dhd) == 2;
}

bool wl_nan_host_supported(struct bcm_cfg80211 *cfg)
{
	return cfg && wl_nan_host_device(cfg->pub);
}

int wl_nan_host_legacy_command(struct wiphy *wiphy, struct wireless_dev *wdev,
                               const void *data, int length)
{
	/* Android's autonomous discovery/NDP commands cannot share protocol
	 * ownership with the hostap backend and the patched host-only firmware. */
	return -EOPNOTSUPP;
}

int wl_nan_host_command(struct wiphy *wiphy, struct wireless_dev *wdev,
                        const void *data, int length)
{
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	const u8 *input = data;
	struct sk_buff *skb;
	u8 *request, *buffer, *reply;
	u16 command, operation, limit, result_length = 0;
	size_t input_length, buffer_length;
	int result, error;
	bool awdl_locked = false;

	if (!wdev || !wdev->netdev || !wl_nan_host_supported(cfg))
		return -EOPNOTSUPP;
	if (wdev->netdev != bcmcfg_to_prmry_ndev(cfg))
		return -EINVAL;
	if (!input || length < BCM_NAN_VENDOR_REQUEST_SIZE ||
	    length > BCM_NAN_VENDOR_REQUEST_SIZE + BCM_NAN_VENDOR_MAX_DATA)
		return -EINVAL;
	command = get_unaligned_le16(input + 2);
	operation = get_unaligned_le16(input + 4);
	limit = get_unaligned_le16(input + 6);
	if (get_unaligned_le16(input) != BCM_NAN_VENDOR_VERSION ||
	    (operation != BCM_NAN_VENDOR_GET && operation != BCM_NAN_VENDOR_SET &&
	     operation != BCM_NAN_VENDOR_NDI) ||
	    limit > BCM_NAN_VENDOR_MAX_DATA ||
	    (operation == BCM_NAN_VENDOR_SET && limit))
		return -EINVAL;
	input_length = length - BCM_NAN_VENDOR_REQUEST_SIZE;
	if (command == BCM_AWDL_COMMAND && operation != BCM_NAN_VENDOR_NDI) {
		error = awdl_request(input + BCM_NAN_VENDOR_REQUEST_SIZE,
			input_length, operation, limit);
		if (error)
			return error;
	}
	if (operation == BCM_NAN_VENDOR_NDI &&
	    (input_length != BCM_NAN_NDI_REQUEST_SIZE || limit != BCM_NAN_NDI_STATUS_SIZE))
		return -EINVAL;
	buffer_length = max_t(size_t, 10 + input_length, 6 + limit);
	request = kzalloc(6 + input_length, GFP_KERNEL);
	buffer = kzalloc(buffer_length, GFP_KERNEL);
	reply = kzalloc(BCM_NAN_VENDOR_REPLY_SIZE + limit, GFP_KERNEL);
	if (!request || !buffer || !reply) {
		error = -ENOMEM;
		goto out;
	}
	if (operation == BCM_NAN_VENDOR_NDI) {
		error = wl_nan_host_ndi_command(cfg, command, input + BCM_NAN_VENDOR_REQUEST_SIZE,
			reply + BCM_NAN_VENDOR_REPLY_SIZE);
		if (error)
			goto out;
		result = 0;
		result_length = BCM_NAN_NDI_STATUS_SIZE;
		goto respond;
	}
	put_unaligned_le16(2, request);
	put_unaligned_le16(input_length, request + 2);
	put_unaligned_le16(command, request + 4);
	memcpy(request + 6, input + BCM_NAN_VENDOR_REQUEST_SIZE, input_length);
	if (command == BCM_AWDL_COMMAND) {
		mutex_lock(&cfg->if_sync);
		awdl_locked = true;
		if (operation == BCM_NAN_VENDOR_SET &&
		    get_unaligned_le16(request + 8) == BCM_AWDL_START) {
			error = wl_nan_host_awdl_claim(cfg, request + 6);
			if (error)
				goto out;
		}
	}
	if (operation == BCM_NAN_VENDOR_SET)
		result = wldev_iovar_setbuf(wdev->netdev, "nan", request,
			6 + input_length, buffer, buffer_length, &cfg->ioctl_buf_sync);
	else
		result = wldev_iovar_getbuf(wdev->netdev, "nan", request,
			6 + input_length, buffer, buffer_length, &cfg->ioctl_buf_sync);
	if (!result && command == BCM_AWDL_COMMAND && operation == BCM_NAN_VENDOR_SET &&
	    get_unaligned_le16(request + 8) == BCM_AWDL_STOP)
		wl_nan_host_awdl_released(cfg, get_unaligned_le64(request + 14));
	if (!result && operation == BCM_NAN_VENDOR_GET) {
		result_length = get_unaligned_le16(buffer + 2);
		if (get_unaligned_le16(buffer) != 2 ||
		    get_unaligned_le16(buffer + 4) != command || result_length > limit ||
		    result_length > buffer_length - 6) {
			pr_err("BCM NAN: GET %04x malformed reply version=%u command=%04x length=%u capacity=%u\n",
				command, get_unaligned_le16(buffer),
				get_unaligned_le16(buffer + 4), result_length, limit);
			error = -EPROTO;
			goto out;
		}
		memcpy(reply + BCM_NAN_VENDOR_REPLY_SIZE, buffer + 6, result_length);
		if (command == BCM_AWDL_COMMAND) {
			const u8 *status = buffer + 6;
			u64 requested = get_unaligned_le64(input + BCM_NAN_VENDOR_REQUEST_SIZE + 8);
			if (result_length != BCM_AWDL_STATUS_SIZE ||
			    get_unaligned_le16(status) != BCM_AWDL_ABI_VERSION ||
			    get_unaligned_le16(status + 2) != BCM_AWDL_QUERY ||
			    get_unaligned_le16(status + 4) != BCM_AWDL_STATUS_SIZE ||
			    get_unaligned_le16(status + 6) || get_unaligned_le16(status + 54) ||
			    (requested && requested != get_unaligned_le64(status + 8))) {
				error = -EPROTO;
				goto out;
			}
			if (!get_unaligned_le64(status + 8) && !get_unaligned_le32(status + 24))
				wl_nan_host_awdl_released(cfg, 0);
		}
	}
respond:
	put_unaligned_le16(BCM_NAN_VENDOR_VERSION, reply);
	put_unaligned_le16(command, reply + 2);
	put_unaligned_le16(result_length, reply + 4);
	put_unaligned_le32(result, reply + 8);
	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy,
		BCM_NAN_VENDOR_REPLY_SIZE + result_length);
	if (!skb) {
		error = -ENOMEM;
		goto out;
	}
	/* cfg80211 already owns the enclosing VENDOR_DATA attribute. Keep its
	 * payload exact: nla_put_nohdr would count alignment bytes as output. */
	skb_put_data(skb, reply, BCM_NAN_VENDOR_REPLY_SIZE + result_length);
	error = cfg80211_vendor_cmd_reply(skb);
out:
	if (awdl_locked)
		mutex_unlock(&cfg->if_sync);
	/* A SET input may contain PASN key material. Never echo it in a reply,
	 * log it, or leave it in this transport's reusable heap buffers. */
	if (request)
		memzero_explicit(request, 6 + input_length);
	if (buffer)
		memzero_explicit(buffer, buffer_length);
	if (reply)
		memzero_explicit(reply, BCM_NAN_VENDOR_REPLY_SIZE + limit);
	kfree(request);
	kfree(buffer);
	kfree(reply);
	return error;
}

s32 wl_nan_host_event(struct bcm_cfg80211 *cfg, bcm_struct_cfgdev *cfgdev,
                      const wl_event_msg_t *event, void *data)
{
	u32 type, reason, length;

	(void)cfgdev;
	if (!event || !wl_nan_host_supported(cfg))
		return -EOPNOTSUPP;
	type = ntoh32(event->event_type);
	reason = ntoh32(event->reason);
	length = ntoh32(event->datalen);
	if ((type != WLC_E_NAN_CRITICAL && type != WLC_E_NAN_NON_CRITICAL) ||
	    length > BCM_NAN_VENDOR_MAX_EVENT || (length && !data))
		return -EINVAL;
	if (reason == BCM_AWDL_EVENT_REASON &&
	    (type != WLC_E_NAN_CRITICAL || awdl_event(data, length)))
		return -EPROTO;
	if (reason == BCM_NAN_HOST_REASON) {
		const u8 *frame = data;
		u16 kind;
		if (type != BCM_NAN_HOST_EVENT || length < BCM_NAN_HOST_HEADER_SIZE ||
		    length > BCM_NAN_HOST_MAX_MESSAGE ||
		    get_unaligned_le16(frame) != BCM_NAN_HOST_VERSION ||
		    get_unaligned_le16(frame + 4) != length ||
		    get_unaligned_le16(frame + 6) != length - BCM_NAN_HOST_HEADER_SIZE)
			return -EPROTO;
		kind = get_unaligned_le16(frame + 2);
		if ((kind != BCM_NAN_HOST_RX && kind != BCM_NAN_HOST_TX_STATUS &&
		     kind != BCM_NAN_HOST_DW_START && kind != BCM_NAN_HOST_KEY_DRAINED) ||
		    (kind != BCM_NAN_HOST_RX && length != BCM_NAN_HOST_HEADER_SIZE) ||
		    (kind == BCM_NAN_HOST_RX && length < BCM_NAN_HOST_HEADER_SIZE + 24))
			return -EPROTO;
		if (kind == BCM_NAN_HOST_KEY_DRAINED &&
		    (!get_unaligned_le64(frame + 8) || get_unaligned_le64(frame + 8) > 65536 ||
		     !get_unaligned_le16(frame + 16) || get_unaligned_le16(frame + 18) ||
		     get_unaligned_le32(frame + 20)))
			return -EPROTO;
		if (kind == BCM_NAN_HOST_DW_START &&
		    (get_unaligned_le64(frame + 8) || !get_unaligned_le16(frame + 16) ||
		     get_unaligned_le16(frame + 18) || get_unaligned_le32(frame + 20)))
			return -EPROTO;
	}
	return wl_nan_host_send_event(cfg, type, reason, ntoh32(event->status),
		event->ifidx, event->bsscfgidx, data, length, GFP_KERNEL);
}

int wl_nan_host_send_event(struct bcm_cfg80211 *cfg, u32 type, u32 reason,
                           u32 status, u8 ifidx, u8 bssidx,
                           const void *data, size_t length, gfp_t allocation)
{
	struct wiphy *wiphy = cfg->wdev->wiphy;
	struct sk_buff *skb;
	u8 *wire;
	unsigned int index;

	/* cfg80211 takes a table index, not the vendor subcommand value. Keep
	 * existing vendor event indexes unchanged and find our appended entry. */
	for (index = 0; index < wiphy->n_vendor_events; ++index)
		if (wiphy->vendor_events[index].vendor_id == BCM_NAN_VENDOR_OUI &&
		    wiphy->vendor_events[index].subcmd == BCM_NAN_VENDOR_SUBCMD)
			break;
	if (index == wiphy->n_vendor_events)
		return -ENOENT;
	skb = cfg80211_vendor_event_alloc(wiphy, cfg->wdev,
		BCM_NAN_VENDOR_EVENT_SIZE + length, index, allocation);
	if (!skb)
		return -ENOMEM;
	wire = skb_put(skb, BCM_NAN_VENDOR_EVENT_SIZE + length);
	memset(wire, 0, BCM_NAN_VENDOR_EVENT_SIZE);
	put_unaligned_le16(BCM_NAN_VENDOR_VERSION, wire);
	put_unaligned_le16(BCM_NAN_VENDOR_EVENT_SIZE + length, wire + 2);
	put_unaligned_le32(type, wire + 4);
	put_unaligned_le32(reason, wire + 8);
	put_unaligned_le32(status, wire + 12);
	wire[16] = ifidx;
	wire[17] = bssidx;
	if (length)
		memcpy(wire + BCM_NAN_VENDOR_EVENT_SIZE, data, length);
	cfg80211_vendor_event(skb, allocation);
	return 0;
}

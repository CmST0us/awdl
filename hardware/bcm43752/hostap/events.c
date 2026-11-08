/* SPDX-License-Identifier: BSD-3-Clause */
#include "events.h"

static int host_event(const struct bcm_event *event, struct bcm_notification *n)
{
	const u8 *p = event->data;
	if (event->status || event->length < BCM_NAN_HOST_HEADER_SIZE ||
	    WPA_GET_LE16(p) != BCM_NAN_HOST_VERSION || WPA_GET_LE16(p + 4) != event->length ||
	    WPA_GET_LE16(p + 6) != event->length - BCM_NAN_HOST_HEADER_SIZE) return -EBADMSG;
	n->length = WPA_GET_LE16(p + 6); n->cookie = WPA_GET_LE64(p + 8);
	n->chanspec = WPA_GET_LE16(p + 16); n->flags = WPA_GET_LE16(p + 18);
	n->result = (s32)WPA_GET_LE32(p + 20);
	if (!n->chanspec) return -EBADMSG;
	switch (WPA_GET_LE16(p + 2)) {
	case BCM_NAN_HOST_RX:
		if (n->cookie || n->result || n->flags & ~BCM_NAN_HOST_RX_DECRYPTED ||
		    n->length < 24 || n->length > BCM_NAN_HOST_MAX_FRAME) return -EBADMSG;
		n->kind = BCM_EVENT_RX; n->frame = p + BCM_NAN_HOST_HEADER_SIZE;
		if (n->frame[0] & 0x0c || n->frame[1] & 3) return -EBADMSG;
		/* Protected unicast must have completed native integrity/replay
		 * validation. Group BIP clears Protected but also reports verified. */
		if ((n->frame[1] & 0x40) && !(n->flags & BCM_NAN_HOST_RX_DECRYPTED)) return -EBADMSG;
		return 0;
	case BCM_NAN_HOST_TX_STATUS:
		if (!n->cookie || n->length || n->flags & ~BCM_NAN_HOST_ACK ||
		    (n->result < 0 && n->flags)) return -EBADMSG;
		n->kind = BCM_EVENT_TX; return 0;
	case BCM_NAN_HOST_KEY_DRAINED:
		if (!n->cookie || n->cookie > 65536 || n->length || n->flags || n->result)
			return -EBADMSG;
		n->kind = BCM_EVENT_KEY_DRAINED; return 0;
	case BCM_NAN_HOST_DW_START:
		if (n->cookie || n->length || n->flags || n->result) return -EBADMSG;
		n->kind = BCM_EVENT_DW; return 0;
	default: return -EPROTONOSUPPORT;
	}
}

int bcm_event_decode(const struct bcm_event *event, struct bcm_notification *n)
{
	os_memset(n, 0, sizeof(*n));
	if (event->type != 158 && event->type != 159) return 0;
	if (event->reason == BCM_NAN_HOST_REASON) return host_event(event, n);
	switch (event->reason) {
	case 1: n->kind = BCM_EVENT_START; break;
	case 3: n->kind = BCM_EVENT_ROLE; break;
	case 10: n->kind = BCM_EVENT_CLUSTER; break;
	case 11: n->kind = BCM_EVENT_STOP; break;
	default: return 0; /* Original DW events lack a channel; kind 5 replaces them. */
	}
	if (event->status) return -EIO;
	/* Actual A2 0x2bd6c serializes wl_nan_conf_status under XTLV 0x0105.
	 * Its 60-byte fixed part ends in a length for optional nested XTLVs. */
	const u8 *p = event->data, *status = NULL;
	size_t left = event->length, bytes = 0;
	while (left) {
		if (left < 4) return -EBADMSG;
		size_t length = WPA_GET_LE16(p + 2), aligned = (length + 7) & ~(size_t)3;
		if (aligned > left) return -EBADMSG;
		if (WPA_GET_LE16(p) == 0x0105) {
			if (status) return -EBADMSG;
			status = p + 4; bytes = length;
		}
		p += aligned; left -= aligned;
	}
	if (!status || bytes < 60 || WPA_GET_LE16(status + 58) != bytes - 60 ||
	    !bcm_unicast(status) || status[54] ||
	    status[6] != (n->kind != BCM_EVENT_STOP)) return -EBADMSG;
	os_memcpy(n->nmi, status, 6); os_memcpy(n->cluster, status + 8, 6); n->role = status[7];
	if (n->kind != BCM_EVENT_STOP &&
	    (os_memcmp(n->cluster, "\x50\x6f\x9a\x01", 4) || n->role > 4)) return -EBADMSG;
	return 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#include "ndi.h"
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

int bcm_ndi_prepare(struct bcm_ndi *ndi, const u8 *mac, const u8 *nmi, const char *name)
{
	if (!bcm_unicast(mac) || !bcm_unicast(nmi) || !os_memcmp(mac, nmi, 6) ||
	    !name || !name[0] || os_strlen(name) >= IFNAMSIZ || os_strchr(name, '%') ||
	    os_strchr(name, '/') || os_strchr(name, ' ') || os_strchr(name, '\t') ||
	    !os_strcmp(name, ".") || !os_strcmp(name, "..")) return -EINVAL;
	if (ndi->attempted && !bcm_ndi_gone(ndi)) return -EBUSY;
	os_memset(ndi, 0, sizeof(*ndi));
	os_memcpy(ndi->mac, mac, 6); os_strlcpy(ndi->name, name, sizeof(ndi->name)); return 0;
}
bool bcm_ndi_created(const struct bcm_ndi *ndi)
{
	u32 required = BCM_NAN_NDI_OWNED | BCM_NAN_NDI_INDEX_KNOWN |
		BCM_NAN_NDI_ADD_SEEN | BCM_NAN_NDI_REGISTERED;
	u32 forbidden = BCM_NAN_NDI_DELETE_SENT | BCM_NAN_NDI_DEL_SEEN | BCM_NAN_NDI_GONE |
		BCM_NAN_NDI_CONFLICT | BCM_NAN_NDI_ORPHAN | BCM_NAN_NDI_QUIESCING | BCM_NAN_NDI_QUIESCED;
	return ndi->attempted && (ndi->flags & required) == required &&
		!(ndi->flags & forbidden) && ndi->ifindex && ndi->ifidx && ndi->bssidx &&
		!ndi->native_result && !ndi->host_result;
}
bool bcm_ndi_gone(const struct bcm_ndi *ndi)
{
	u32 required = BCM_NAN_NDI_OWNED | BCM_NAN_NDI_DEL_SEEN | BCM_NAN_NDI_GONE;
	return ndi->attempted && (ndi->flags & required) == required &&
		!(ndi->flags & (BCM_NAN_NDI_REGISTERED | BCM_NAN_NDI_CONFLICT)) && !ndi->ifindex;
}
int bcm_ndi_command(struct bcm_ndi *ndi, struct bcm_link *link, u16 operation)
{
	if (!bcm_unicast(ndi->mac) || !ndi->name[0] || operation < BCM_NAN_NDI_CREATE ||
	    operation > BCM_NAN_NDI_RESUME || (!ndi->attempted && operation != BCM_NAN_NDI_CREATE))
		return -EINVAL;
	u8 input[BCM_NAN_NDI_REQUEST_SIZE] = {0}, output[BCM_NAN_NDI_STATUS_SIZE];
	os_memcpy(input, ndi->mac, 6); os_memcpy(input + 8, ndi->name, IFNAMSIZ);
	ndi->attempted = true;
	struct bcm_result result = bcm_command(link, operation, BCM_NAN_VENDOR_NDI,
		input, sizeof(input), output, sizeof(output));
	int error = bcm_result_errno(result);
	if (error) return error;
	if (result.used != sizeof(output) || os_memcmp(output + 10, ndi->mac, 6) ||
	    !memchr(output + 16, 0, IFNAMSIZ) ||
	    os_strcmp((const char *)output + 16, ndi->name)) return -EPROTO;
	u32 flags = WPA_GET_LE32(output), index = WPA_GET_LE32(output + 4);
	u32 known = BCM_NAN_NDI_OWNED | BCM_NAN_NDI_INDEX_KNOWN | BCM_NAN_NDI_ADD_SEEN |
		BCM_NAN_NDI_REGISTERED | BCM_NAN_NDI_DELETE_SENT | BCM_NAN_NDI_DEL_SEEN |
		BCM_NAN_NDI_GONE | BCM_NAN_NDI_CONFLICT | BCM_NAN_NDI_ORPHAN |
		BCM_NAN_NDI_QUIESCING | BCM_NAN_NDI_QUIESCED;
	if (!(flags & BCM_NAN_NDI_OWNED) || flags & ~known ||
	    ((flags & BCM_NAN_NDI_INDEX_KNOWN) && (!output[8] || !output[9])) ||
	    ((flags & BCM_NAN_NDI_REGISTERED) && (!index || !(flags & BCM_NAN_NDI_ADD_SEEN))) ||
	    ((flags & BCM_NAN_NDI_GONE) && (!(flags & BCM_NAN_NDI_DEL_SEEN) || index ||
	                                  (flags & BCM_NAN_NDI_REGISTERED)))) return -EPROTO;
	if ((ndi->flags & BCM_NAN_NDI_INDEX_KNOWN) && (flags & BCM_NAN_NDI_INDEX_KNOWN) &&
	    (ndi->ifidx != output[8] || ndi->bssidx != output[9])) return -ESTALE;
	ndi->flags = flags; ndi->ifindex = index; ndi->ifidx = output[8]; ndi->bssidx = output[9];
	ndi->native_result = (s32)WPA_GET_LE32(output + 32);
	ndi->host_result = (s32)WPA_GET_LE32(output + 36);
	if (ndi->native_result || ndi->host_result)
		wpa_printf(MSG_INFO, "BCM NAN: NDI %s flags=%x native=%d host=%d",
			ndi->name, ndi->flags, ndi->native_result, ndi->host_result);
	if (flags & (BCM_NAN_NDI_CONFLICT | BCM_NAN_NDI_ORPHAN)) return -EUCLEAN;
	/* Successful status retrieval is separate from CREATE/GONE/QUIESCED.
	 * The caller waits for actual matching events before querying again. */
	return 0;
}
bool bcm_ndi_changed(const struct bcm_ndi *ndi, const struct bcm_event *event)
{
	if (!ndi->attempted) return false;
	if (event->type == BCM_NAN_NDI_HOST_EVENT) {
		return event->length == BCM_NAN_NDI_STATUS_SIZE &&
			(event->reason == 1 || event->reason == 2) &&
			!os_memcmp(event->data + 10, ndi->mac, 6) &&
			event->data[8] == event->ifidx && event->data[9] == event->bssidx &&
			memchr(event->data + 16, 0, IFNAMSIZ) &&
			!os_strcmp((const char *) event->data + 16, ndi->name) &&
			(!(ndi->flags & BCM_NAN_NDI_INDEX_KNOWN) ||
			 (ndi->ifidx == event->ifidx && ndi->bssidx == event->bssidx));
	}
	if (event->type == BCM_NAN_NDI_FLOW_EVENT) {
		return event->length == 8 && !os_memcmp(event->data, ndi->mac, 6) &&
			(ndi->flags & BCM_NAN_NDI_INDEX_KNOWN) && ndi->ifidx == event->ifidx &&
			ndi->bssidx == event->bssidx;
	}
	/* WLC_E_IF=54, payload is the driver's checked native event snapshot. */
	if (event->type != 54 || event->length != BCM_NAN_NDI_IF_EVENT_SIZE ||
	    event->data[5] || os_memcmp(event->data + 6, ndi->mac, 6)) return false;
	return !(ndi->flags & BCM_NAN_NDI_INDEX_KNOWN) ||
		(ndi->ifidx == event->ifidx && ndi->bssidx == event->bssidx);
}
int bcm_ndi_up(const struct bcm_ndi *ndi)
{
	if (!bcm_ndi_created(ndi)) return -ENETDOWN;
	if (if_nametoindex(ndi->name) != ndi->ifindex) return -ESTALE;
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -errno;
	struct ifreq request = {0};
	os_strlcpy(request.ifr_name, ndi->name, sizeof(request.ifr_name));
	int error = 0;
	if (ioctl(fd, SIOCGIFHWADDR, &request)) error = -errno;
	else if (os_memcmp(request.ifr_hwaddr.sa_data, ndi->mac, 6)) error = -ESTALE;
	else if (ioctl(fd, SIOCGIFFLAGS, &request)) error = -errno;
	else if (!(request.ifr_flags & IFF_UP)) {
		request.ifr_flags |= IFF_UP;
		if (ioctl(fd, SIOCSIFFLAGS, &request)) error = -errno;
	}
	close(fd); return error;
}

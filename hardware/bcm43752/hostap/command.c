/* SPDX-License-Identifier: BSD-3-Clause */
#include "command.h"

struct bcm_result bcm_command(struct bcm_link *link, u16 command, u16 operation,
                              const void *input, size_t length, void *output, size_t capacity)
{
	struct bcm_result result = {0};
	result.transport = bcm_link_command(link, command, operation, input, length,
		output, capacity, &result.used, &result.native);
	if (result.transport || result.native)
		wpa_printf(MSG_INFO, "BCM NAN: command %04x op=%u transport=%d native=%d",
			command, operation, result.transport, result.native);
	return result;
}
int bcm_result_errno(struct bcm_result result)
{
	if (result.transport) return result.transport;
	/* Broadcom BCME values are not Linux errno values. Keep the original
	 * number in the diagnostic above, and map only known meanings here. */
	switch (result.native) {
	case 0: return 0;
	case -2: return -EINVAL;
	case -14: return -EMSGSIZE;
	case -16: return -EBUSY;
	case -22: return -ENOSPC;
	case -23: return -EOPNOTSUPP;
	case -24: return -EMSGSIZE;
	case -25: return -ENETDOWN;
	case -27: return -ENOMEM;
	case -30: return -ENOENT;
	default: return -EIO;
	}
}
struct bcm_result bcm_set(struct bcm_link *link, u16 command, const void *input, size_t length)
{
	return bcm_command(link, command, BCM_NAN_VENDOR_SET, input, length, NULL, 0);
}
struct bcm_result bcm_get(struct bcm_link *link, u16 command, const void *input, size_t length,
                          void *output, size_t capacity)
{
	return bcm_command(link, command, BCM_NAN_VENDOR_GET, input, length, output, capacity);
}
void bcm_header(u8 *wire, u16 operation, u16 length)
{
	os_memset(wire, 0, length);
	WPA_PUT_LE16(wire, BCM_NAN_HOST_VERSION);
	WPA_PUT_LE16(wire + 2, operation); WPA_PUT_LE16(wire + 4, length);
}
bool bcm_unicast(const u8 *address)
{
	return address && !is_zero_ether_addr(address) && !is_multicast_ether_addr(address);
}

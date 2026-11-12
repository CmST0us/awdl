/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BCM_NAN_NETLINK_H
#define BCM_NAN_NETLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <linux/netlink.h>

struct bcm_link;
struct bcm_event {
	uint32_t type, reason, status;
	uint8_t ifidx, bssidx;
	const uint8_t *data;
	size_t length;
};
typedef void (*bcm_event_fn)(void *, const struct bcm_event *);
/* Attribute buffers and event data are borrowed only for the callback. */
typedef int (*bcm_wiphy_fn)(void *, const void *, size_t);

struct bcm_link *bcm_link_open(const char *interface, bcm_event_fn event, void *context);
void bcm_link_close(struct bcm_link *link);
int bcm_link_fd(const struct bcm_link *link);
int bcm_link_receive(struct bcm_link *link);
int bcm_link_wiphy(struct bcm_link *link, bcm_wiphy_fn callback, void *context);
int bcm_link_country(struct bcm_link *link, char country[3]);
unsigned bcm_link_ifindex(const struct bcm_link *link);
unsigned bcm_link_phy(const struct bcm_link *link);
int bcm_link_clear_tcp_metrics(struct bcm_link *, const uint8_t source[16],
                                const uint8_t destination[16]);
const uint8_t *bcm_link_address(const struct bcm_link *link);
int bcm_nla_parse(const void *input, size_t length, const struct nlattr **entry, size_t count);
const void *bcm_nla_value(const struct nlattr *attribute);
size_t bcm_nla_size(const struct nlattr *attribute);
uint32_t bcm_nla_u32(const struct nlattr *attribute);

/* Returns a Linux transport error, separately from the native signed result.
 * Success requires both the actual vendor reply and netlink acknowledgement.
 * A deadline never proves that a mutation was rejected or rolled back. */
int bcm_link_command(struct bcm_link *link, uint16_t command, uint16_t operation,
                     const void *input, size_t length, void *output, size_t capacity,
                     size_t *used, int32_t *native_result);

#endif

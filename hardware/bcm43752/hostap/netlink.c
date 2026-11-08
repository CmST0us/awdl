/* SPDX-License-Identifier: BSD-3-Clause */
#include "netlink.h"
#include "../include/nan_vendor_abi.h"
#include <errno.h>
#include <linux/genetlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* One socket serializes command replies; the second is an event source for
 * hostap's eloop. Native QUIESCE may wait twice for a five-second completion
 * deadline, so transport failure has a separate twenty-second bound. */
#define COMMAND_TIMEOUT_MS 20000
#define MESSAGE_SIZE 8192
#define RECEIVE_SIZE 65536
struct bcm_link {
	int command, events, family;
	uint32_t sequence, ifindex, wiphy;
	uint8_t address[6];
	bcm_event_fn event;
	void *context;
};
struct message { uint8_t bytes[MESSAGE_SIZE]; size_t length; };
typedef int (*reply_fn)(void *, const struct genlmsghdr *, const void *, size_t);

static uint16_t le16(const void *input)
{
	const uint8_t *p = input;
	return p[0] | (uint16_t)p[1] << 8;
}
static uint32_t le32(const void *input)
{
	const uint8_t *p = input;
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put16(void *output, uint16_t value)
{
	uint8_t *p = output;
	p[0] = value; p[1] = value >> 8;
}
static void erase(void *data, size_t length)
{
	volatile uint8_t *p = data;
	while (length--) *p++ = 0;
}
const void *bcm_nla_value(const struct nlattr *a) { return (const uint8_t *)a + NLA_HDRLEN; }
size_t bcm_nla_size(const struct nlattr *a) { return a ? a->nla_len - NLA_HDRLEN : 0; }
uint32_t bcm_nla_u32(const struct nlattr *a)
{
	uint32_t result = 0;
	if (bcm_nla_size(a) == sizeof(result)) memcpy(&result, bcm_nla_value(a), sizeof(result));
	return result;
}
int bcm_nla_parse(const void *input, size_t length, const struct nlattr **entry, size_t count)
{
	const uint8_t *p = input;
	memset(entry, 0, count * sizeof(*entry));
	while (length) {
		const struct nlattr *a = (const void *)p;
		if (length < NLA_HDRLEN || a->nla_len < NLA_HDRLEN ||
		    a->nla_len > length)
			return -EBADMSG;
		unsigned type = a->nla_type & NLA_TYPE_MASK;
		if (type < count) {
			if (entry[type]) return -EBADMSG;
			entry[type] = a;
		}
		/* cfg80211's raw VENDOR_DATA may end exactly at nlmsg_len,
		 * without padding after the last attribute. Padding is not data. */
		if (a->nla_len == length) return 0;
		size_t step = NLA_ALIGN(a->nla_len);
		if (step > length) return -EBADMSG;
		p += step; length -= step;
	}
	return 0;
}
static int put(struct message *message, uint16_t type, const void *input, size_t length)
{
	if (length > UINT16_MAX - NLA_HDRLEN) return -EMSGSIZE;
	struct nlattr attr = {.nla_len = NLA_HDRLEN + length, .nla_type = type};
	size_t aligned = NLA_ALIGN(attr.nla_len);
	if (message->length + aligned > sizeof(message->bytes)) return -EMSGSIZE;
	uint8_t *p = message->bytes + message->length;
	memset(p, 0, aligned); memcpy(p, &attr, NLA_HDRLEN);
	if (length) memcpy(p + NLA_HDRLEN, input, length);
	message->length += aligned;
	return 0;
}
static void begin(struct bcm_link *link, struct message *message, int family,
                   uint8_t command, bool dump)
{
	if (!++link->sequence) ++link->sequence;
	struct nlmsghdr n = {.nlmsg_type = family, .nlmsg_seq = link->sequence,
		.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | (dump ? NLM_F_DUMP : 0)};
	struct genlmsghdr g = {.cmd = command, .version = 1};
	memset(message, 0, sizeof(*message));
	memcpy(message->bytes, &n, NLMSG_HDRLEN);
	memcpy(message->bytes + NLMSG_HDRLEN, &g, GENL_HDRLEN);
	message->length = NLMSG_HDRLEN + GENL_HDRLEN;
}
static int64_t milliseconds(void)
{
	struct timespec time;
	if (clock_gettime(CLOCK_MONOTONIC, &time)) return -1;
	return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}
static int receive(int socket, void *buffer, size_t capacity, int flags)
{
	struct sockaddr_nl source = {0};
	struct iovec vector = {.iov_base = buffer, .iov_len = capacity};
	struct msghdr message = {.msg_name = &source, .msg_namelen = sizeof(source),
		.msg_iov = &vector, .msg_iovlen = 1};
	ssize_t result = recvmsg(socket, &message, flags);
	if (result < 0) return -errno;
	if (source.nl_family != AF_NETLINK || source.nl_pid) return -EPROTO;
	if (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) return -EMSGSIZE;
	return result;
}
static int exchange(struct bcm_link *link, struct message *message, reply_fn reply, void *context)
{
	struct nlmsghdr *request = (void *)message->bytes;
	struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
	struct pollfd pollfd = {.fd = link->command, .events = POLLIN};
	uint8_t buffer[RECEIVE_SIZE];
	bool acknowledged = false, received = false, dump = request->nlmsg_flags & NLM_F_DUMP;
	int64_t now = milliseconds(), deadline = now + COMMAND_TIMEOUT_MS;
	int result = now < 0 ? -errno : 0;
	if (result) goto out;
	request->nlmsg_len = message->length;
	ssize_t sent = sendto(link->command, message->bytes, message->length, 0,
		(void *)&kernel, sizeof(kernel));
	if (sent != (ssize_t)message->length) {
		result = sent < 0 ? -errno : -EIO; goto out;
	}
	for (;;) {
		now = milliseconds();
		if (now < 0) { result = -errno; break; }
		if (now >= deadline) { result = -ETIMEDOUT; break; }
		result = poll(&pollfd, 1, deadline - now);
		if (result < 0 && errno == EINTR) continue;
		if (result <= 0) { result = result ? -errno : -ETIMEDOUT; break; }
		if (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)) { result = -EIO; break; }
		int length = receive(link->command, buffer, sizeof(buffer), 0);
		if (length == -EINTR) continue;
		if (length <= 0) { result = length ? length : -EIO; break; }
		struct nlmsghdr *n;
		for (n = (void *)buffer; NLMSG_OK(n, length); n = NLMSG_NEXT(n, length)) {
			if (n->nlmsg_seq != link->sequence) continue;
			if (n->nlmsg_flags & NLM_F_DUMP_INTR) { result = -EINTR; goto out; }
			if (n->nlmsg_type == NLMSG_ERROR) {
				if (n->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
					result = -EBADMSG; goto out;
				}
				result = ((struct nlmsgerr *)NLMSG_DATA(n))->error;
				if (result > 0) result = -EBADMSG;
				if (result) goto out;
				acknowledged = true;
			} else if (n->nlmsg_type == NLMSG_DONE) {
				if (!dump) { result = -EPROTO; goto out; }
				result = 0;
				if (n->nlmsg_len >= NLMSG_LENGTH(sizeof(result)))
					memcpy(&result, NLMSG_DATA(n), sizeof(result));
				if (!result && !received) result = -ENODATA;
				goto out;
			} else if (n->nlmsg_type == NLMSG_OVERRUN) {
				result = -ENOBUFS; goto out;
			} else {
				if (n->nlmsg_type != request->nlmsg_type ||
				    n->nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN) {
					result = -EPROTO; goto out;
				}
				const struct genlmsghdr *g = NLMSG_DATA(n);
				result = reply(context, g, (const uint8_t *)g + GENL_HDRLEN,
					n->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN);
				if (result) goto out;
				received = true;
			}
		}
		/* NLMSG_NEXT subtracts aligned size even for an unpadded last
		 * message; a negative remainder is only its absent tail padding. */
		if (length > 0) { result = -EBADMSG; break; }
		if (!dump && acknowledged && received) { result = 0; break; }
	}
out:
	erase(buffer, sizeof(buffer));
	erase(message, sizeof(*message));
	return result;
}
static int socket_open(void)
{
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
	struct sockaddr_nl address = {.nl_family = AF_NETLINK};
	int capacity = 262144;
	if (fd < 0) return -1;
	if (bind(fd, (void *)&address, sizeof(address)) ||
	    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &capacity, sizeof(capacity))) {
		int error = errno; close(fd); errno = error; return -1;
	}
	return fd;
}
static int family_reply(void *context, const struct genlmsghdr *g, const void *data, size_t length)
{
	struct bcm_link *link = context;
	const struct nlattr *a[CTRL_ATTR_MAX + 1];
	if (g->cmd != CTRL_CMD_NEWFAMILY || bcm_nla_parse(data, length, a, CTRL_ATTR_MAX + 1) ||
	    bcm_nla_size(a[CTRL_ATTR_FAMILY_ID]) != 2 || !a[CTRL_ATTR_MCAST_GROUPS]) return -EBADMSG;
	uint16_t family;
	memcpy(&family, bcm_nla_value(a[CTRL_ATTR_FAMILY_ID]), sizeof(family)); link->family = family;
	const uint8_t *p = bcm_nla_value(a[CTRL_ATTR_MCAST_GROUPS]);
	size_t left = bcm_nla_size(a[CTRL_ATTR_MCAST_GROUPS]);
	while (left) {
		const struct nlattr *group = (const void *)p, *v[CTRL_ATTR_MCAST_GRP_MAX + 1];
		if (left < NLA_HDRLEN || group->nla_len < NLA_HDRLEN ||
		    (size_t)NLA_ALIGN(group->nla_len) > left ||
		    bcm_nla_parse(bcm_nla_value(group), bcm_nla_size(group), v, CTRL_ATTR_MCAST_GRP_MAX + 1)) return -EBADMSG;
		if (bcm_nla_size(v[CTRL_ATTR_MCAST_GRP_NAME]) == sizeof("vendor") &&
		    !memcmp(bcm_nla_value(v[CTRL_ATTR_MCAST_GRP_NAME]), "vendor", sizeof("vendor"))) {
			if (bcm_nla_size(v[CTRL_ATTR_MCAST_GRP_ID]) != 4) return -EBADMSG;
			uint32_t id = bcm_nla_u32(v[CTRL_ATTR_MCAST_GRP_ID]);
			return setsockopt(link->events, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP,
				&id, sizeof(id)) ? -errno : 0;
		}
		left -= NLA_ALIGN(group->nla_len); p += NLA_ALIGN(group->nla_len);
	}
	return -ENOENT;
}
static int interface_reply(void *context, const struct genlmsghdr *g, const void *data, size_t length)
{
	struct bcm_link *link = context;
	const struct nlattr *a[NL80211_ATTR_MAX + 1];
	if (g->cmd != NL80211_CMD_NEW_INTERFACE ||
	    bcm_nla_parse(data, length, a, NL80211_ATTR_MAX + 1) ||
	    bcm_nla_size(a[NL80211_ATTR_IFINDEX]) != 4 || bcm_nla_u32(a[NL80211_ATTR_IFINDEX]) != link->ifindex ||
	    bcm_nla_size(a[NL80211_ATTR_WIPHY]) != 4 || bcm_nla_size(a[NL80211_ATTR_MAC]) != 6) return -EBADMSG;
	link->wiphy = bcm_nla_u32(a[NL80211_ATTR_WIPHY]);
	memcpy(link->address, bcm_nla_value(a[NL80211_ATTR_MAC]), sizeof(link->address));
	return 0;
}
struct bcm_link *bcm_link_open(const char *interface, bcm_event_fn event, void *context)
{
	struct bcm_link *link = calloc(1, sizeof(*link));
	struct message message;
	int result = 0;
	if (!link) return NULL;
	link->command = link->events = -1;
	link->ifindex = if_nametoindex(interface);
	if (!link->ifindex) { result = errno ? -errno : -ENODEV; goto fail; }
	link->event = event; link->context = context;
	link->command = socket_open();
	if (link->command < 0) { result = -errno; goto fail; }
	link->events = socket_open();
	if (link->events < 0) { result = -errno; goto fail; }
	begin(link, &message, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, false);
	put(&message, CTRL_ATTR_FAMILY_NAME, "nl80211", sizeof("nl80211"));
	result = exchange(link, &message, family_reply, link);
	if (result) goto fail;
	begin(link, &message, link->family, NL80211_CMD_GET_INTERFACE, false);
	put(&message, NL80211_ATTR_IFINDEX, &link->ifindex, sizeof(link->ifindex));
	result = exchange(link, &message, interface_reply, link);
	if (!result) return link;
fail:
	bcm_link_close(link); errno = -result; return NULL;
}
void bcm_link_close(struct bcm_link *link)
{
	if (!link) return;
	if (link->command >= 0) close(link->command);
	if (link->events >= 0) close(link->events);
	erase(link, sizeof(*link)); free(link);
}
int bcm_link_fd(const struct bcm_link *link) { return link->events; }
unsigned bcm_link_ifindex(const struct bcm_link *link) { return link->ifindex; }
unsigned bcm_link_phy(const struct bcm_link *link) { return link->wiphy; }
const uint8_t *bcm_link_address(const struct bcm_link *link) { return link->address; }

struct vendor_reply {
	uint16_t command;
	void *output;
	size_t capacity, used;
	int32_t result;
	bool received;
};
static int vendor_reply(void *context, const struct genlmsghdr *g, const void *data, size_t length)
{
	struct vendor_reply *reply = context;
	const struct nlattr *a[NL80211_ATTR_MAX + 1];
	if (reply->received || g->cmd != NL80211_CMD_VENDOR ||
	    bcm_nla_parse(data, length, a, NL80211_ATTR_MAX + 1) ||
	    bcm_nla_size(a[NL80211_ATTR_VENDOR_DATA]) < BCM_NAN_VENDOR_REPLY_SIZE) return -EBADMSG;
	const uint8_t *wire = bcm_nla_value(a[NL80211_ATTR_VENDOR_DATA]);
	size_t used = le16(wire + 4);
	if (le16(wire) != BCM_NAN_VENDOR_VERSION || le16(wire + 2) != reply->command ||
	    le16(wire + 6) || used > reply->capacity ||
	    bcm_nla_size(a[NL80211_ATTR_VENDOR_DATA]) != BCM_NAN_VENDOR_REPLY_SIZE + used) return -EPROTO;
	reply->result = (int32_t)le32(wire + 8);
	if (reply->result && used) return -EPROTO;
	if (used) memcpy(reply->output, wire + BCM_NAN_VENDOR_REPLY_SIZE, used);
	reply->used = used; reply->received = true;
	return 0;
}
int bcm_link_command(struct bcm_link *link, uint16_t command, uint16_t operation,
                     const void *input, size_t length, void *output, size_t capacity,
                     size_t *used, int32_t *native_result)
{
	struct message message;
	uint8_t wire[BCM_NAN_VENDOR_REQUEST_SIZE + BCM_NAN_VENDOR_MAX_DATA];
	struct vendor_reply reply = {.command = command,
		.output = output, .capacity = capacity};
	uint32_t oui = BCM_NAN_VENDOR_OUI, subcommand = BCM_NAN_VENDOR_SUBCMD;
	if (!used || !native_result) return -EINVAL;
	*used = 0; *native_result = 0;
	if (length > BCM_NAN_VENDOR_MAX_DATA || capacity > BCM_NAN_VENDOR_MAX_DATA ||
	    (length && !input) || (capacity && !output) ||
	    operation < BCM_NAN_VENDOR_GET || operation > BCM_NAN_VENDOR_NDI ||
	    (operation == BCM_NAN_VENDOR_SET && capacity)) return -EINVAL;
	put16(wire, BCM_NAN_VENDOR_VERSION); put16(wire + 2, command);
	put16(wire + 4, operation); put16(wire + 6, capacity);
	if (length) memcpy(wire + BCM_NAN_VENDOR_REQUEST_SIZE, input, length);
	begin(link, &message, link->family, NL80211_CMD_VENDOR, false);
	int result = put(&message, NL80211_ATTR_IFINDEX, &link->ifindex, sizeof(link->ifindex));
	if (!result) result = put(&message, NL80211_ATTR_VENDOR_ID, &oui, sizeof(oui));
	if (!result) result = put(&message, NL80211_ATTR_VENDOR_SUBCMD, &subcommand, sizeof(subcommand));
	if (!result) result = put(&message, NL80211_ATTR_VENDOR_DATA, wire,
		BCM_NAN_VENDOR_REQUEST_SIZE + length);
	if (!result) result = exchange(link, &message, vendor_reply, &reply);
	if (!result) { *used = reply.used; *native_result = reply.result; }
	erase(&message, sizeof(message)); erase(wire, sizeof(wire));
	return result;
}

int bcm_link_receive(struct bcm_link *link)
{
	uint8_t buffer[RECEIVE_SIZE];
	int length = receive(link->events, buffer, sizeof(buffer), MSG_DONTWAIT);
	if (length == -EAGAIN || length == -EINTR) return 0;
	if (length <= 0) return length ? length : -EIO;
	struct nlmsghdr *n;
	for (n = (void *)buffer; NLMSG_OK(n, length); n = NLMSG_NEXT(n, length)) {
		const struct nlattr *a[NL80211_ATTR_MAX + 1];
		if (n->nlmsg_type == NLMSG_OVERRUN) return -ENOBUFS;
		if (n->nlmsg_type != link->family || n->nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN) continue;
		const struct genlmsghdr *g = NLMSG_DATA(n);
		if (g->cmd != NL80211_CMD_VENDOR) continue;
		if (bcm_nla_parse((const uint8_t *)g + GENL_HDRLEN,
		    n->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN, a, NL80211_ATTR_MAX + 1)) return -EBADMSG;
		if (bcm_nla_u32(a[NL80211_ATTR_VENDOR_ID]) != BCM_NAN_VENDOR_OUI ||
		    bcm_nla_u32(a[NL80211_ATTR_VENDOR_SUBCMD]) != BCM_NAN_VENDOR_SUBCMD) continue;
		if (bcm_nla_size(a[NL80211_ATTR_WIPHY]) != 4 || bcm_nla_u32(a[NL80211_ATTR_WIPHY]) != link->wiphy)
			continue;
		if (a[NL80211_ATTR_IFINDEX] &&
		    (bcm_nla_size(a[NL80211_ATTR_IFINDEX]) != 4 || bcm_nla_u32(a[NL80211_ATTR_IFINDEX]) != link->ifindex))
			continue;
		if (bcm_nla_size(a[NL80211_ATTR_VENDOR_DATA]) < BCM_NAN_VENDOR_EVENT_SIZE) return -EBADMSG;
		const uint8_t *wire = bcm_nla_value(a[NL80211_ATTR_VENDOR_DATA]);
		size_t total = bcm_nla_size(a[NL80211_ATTR_VENDOR_DATA]);
		if (le16(wire) != BCM_NAN_VENDOR_VERSION || le16(wire + 2) != total ||
		    total > BCM_NAN_VENDOR_EVENT_SIZE + BCM_NAN_VENDOR_MAX_EVENT || le16(wire + 18))
			return -EPROTO;
		struct bcm_event event = {.type = le32(wire + 4), .reason = le32(wire + 8),
			.status = le32(wire + 12), .ifidx = wire[16], .bssidx = wire[17],
			.data = wire + BCM_NAN_VENDOR_EVENT_SIZE, .length = total - BCM_NAN_VENDOR_EVENT_SIZE};
		if (link->event) link->event(link->context, &event);
	}
	return length > 0 ? -EBADMSG : 0;
}
struct wiphy_reply { struct bcm_link *link; bcm_wiphy_fn callback; void *context; bool found; };
static int wiphy_reply(void *context, const struct genlmsghdr *g, const void *data, size_t length)
{
	struct wiphy_reply *reply = context;
	const struct nlattr *a[NL80211_ATTR_MAX + 1];
	if (g->cmd != NL80211_CMD_NEW_WIPHY || bcm_nla_parse(data, length, a, NL80211_ATTR_MAX + 1) ||
	    bcm_nla_size(a[NL80211_ATTR_WIPHY]) != 4) return -EBADMSG;
	if (bcm_nla_u32(a[NL80211_ATTR_WIPHY]) != reply->link->wiphy) return 0;
	reply->found = true;
	return reply->callback(reply->context, data, length);
}
int bcm_link_wiphy(struct bcm_link *link, bcm_wiphy_fn callback, void *context)
{
	struct message message;
	struct wiphy_reply reply = {.link = link, .callback = callback, .context = context};
	if (!callback) return -EINVAL;
	begin(link, &message, link->family, NL80211_CMD_GET_WIPHY, true);
	int result = put(&message, NL80211_ATTR_WIPHY, &link->wiphy, sizeof(link->wiphy));
	if (!result) result = put(&message, NL80211_ATTR_SPLIT_WIPHY_DUMP, NULL, 0);
	if (!result) result = exchange(link, &message, wiphy_reply, &reply);
	return result ? result : reply.found ? 0 : -ENODEV;
}
static int country_reply(void *context, const struct genlmsghdr *g, const void *data, size_t length)
{
	const struct nlattr *a[NL80211_ATTR_MAX + 1];
	if (g->cmd != NL80211_CMD_GET_REG || bcm_nla_parse(data, length, a, NL80211_ATTR_MAX + 1) ||
	    bcm_nla_size(a[NL80211_ATTR_REG_ALPHA2]) != 3) return -EBADMSG;
	const uint8_t *country = bcm_nla_value(a[NL80211_ATTR_REG_ALPHA2]);
	if (country[2]) return -EBADMSG;
	memcpy(context, country, 3); return 0;
}
int bcm_link_country(struct bcm_link *link, char country[3])
{
	if (!country) return -EINVAL;
	struct message message;
	begin(link, &message, link->family, NL80211_CMD_GET_REG, false);
	int result = put(&message, NL80211_ATTR_WIPHY, &link->wiphy, sizeof(link->wiphy));
	if (!result) result = exchange(link, &message, country_reply, country);
	return result;
}

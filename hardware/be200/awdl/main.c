#define _POSIX_C_SOURCE 200809L
#include "nl80211_copy.h"
#include <arpa/inet.h>
#include <errno.h>
#include <linux/genetlink.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../../components/awdl/control.h"

/* The command socket owns awdl0. The kernel owns its NMI peer until netdev
 * stop. */
struct netlink {
  int fd, family;
  uint32_t seq;
};
struct message {
  unsigned char bytes[8192];
  size_t len;
};
struct run {
  struct netlink cmd, events;
  struct Control *node;
  uint32_t phy, ndi;
  uint64_t nmi, start;
  unsigned char local[6], peer[6];
  unsigned tx, rx, completions;
  int error;
};
static volatile sig_atomic_t stopped;
static void stop_signal(int signal) {
  (void)signal;
  stopped = 1;
}
static uint64_t micros(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static int open_netlink(struct netlink *nl) {
  struct sockaddr_nl address = {.nl_family = AF_NETLINK};
  nl->fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
  if (nl->fd < 0 || bind(nl->fd, (void *)&address, sizeof(address)))
    return -errno;
  return 0;
}
static void begin(struct netlink *nl, struct message *m, int family, int cmd) {
  struct nlmsghdr h = {.nlmsg_len = NLMSG_HDRLEN + GENL_HDRLEN,
                       .nlmsg_type = family,
                       .nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK,
                       .nlmsg_seq = ++nl->seq};
  struct genlmsghdr g = {.cmd = cmd, .version = 1};
  memcpy(m->bytes, &h, sizeof(h));
  memcpy(m->bytes + NLMSG_HDRLEN, &g, sizeof(g));
  m->len = h.nlmsg_len;
}
static void put(struct message *m, int type, const void *data, size_t len) {
  struct nlattr a = {.nla_len = NLA_HDRLEN + len, .nla_type = type};
  if (m->len + NLA_ALIGN(a.nla_len) > sizeof(m->bytes))
    abort();
  memcpy(m->bytes + m->len, &a, NLA_HDRLEN);
  if (len)
    memcpy(m->bytes + m->len + NLA_HDRLEN, data, len);
  memset(m->bytes + m->len + a.nla_len, 0, NLA_ALIGN(a.nla_len) - a.nla_len);
  m->len += NLA_ALIGN(a.nla_len);
}
static void put32(struct message *m, int type, uint32_t v) {
  put(m, type, &v, sizeof(v));
}
static void put16(struct message *m, int type, uint16_t v) {
  put(m, type, &v, sizeof(v));
}
static void put8(struct message *m, int type, uint8_t v) {
  put(m, type, &v, sizeof(v));
}
static size_t nest(struct message *m, int type) {
  size_t at = m->len;
  put(m, type | NLA_F_NESTED, NULL, 0);
  return at;
}
static void endnest(struct message *m, size_t at) {
  ((struct nlattr *)(m->bytes + at))->nla_len = m->len - at;
}
static const void *value(const struct nlattr *a) {
  return (const unsigned char *)a + NLA_HDRLEN;
}
static size_t length(const struct nlattr *a) {
  return a ? a->nla_len - NLA_HDRLEN : 0;
}
static uint32_t number(const struct nlattr *a) {
  uint32_t v = 0;
  if (length(a) == sizeof(v))
    memcpy(&v, value(a), sizeof(v));
  return v;
}
static void parse(const void *bytes, size_t len, const struct nlattr **attrs,
                  size_t count) {
  memset(attrs, 0, count * sizeof(*attrs));
  const unsigned char *pos = bytes;
  while (len >= NLA_HDRLEN) {
    const struct nlattr *a = (const void *)pos;
    if (a->nla_len < NLA_HDRLEN || (size_t)NLA_ALIGN(a->nla_len) > len)
      break;
    unsigned type = a->nla_type & NLA_TYPE_MASK;
    if (type < count)
      attrs[type] = a;
    size_t step = NLA_ALIGN(a->nla_len);
    pos += step;
    len -= step;
  }
}
static void attrs(struct nlmsghdr *h, const struct nlattr **a, size_t count) {
  parse((unsigned char *)NLMSG_DATA(h) + GENL_HDRLEN,
        h->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN, a, count);
}
static int send_message(struct netlink *nl, struct message *m) {
  struct sockaddr_nl to = {.nl_family = AF_NETLINK};
  ((struct nlmsghdr *)m->bytes)->nlmsg_len = m->len;
  return sendto(nl->fd, m->bytes, m->len, 0, (void *)&to, sizeof(to)) < 0
             ? -errno
             : 0;
}
static int request(struct netlink *nl, struct message *m,
                   int (*reply)(struct nlmsghdr *, void *), void *context) {
  int rc = send_message(nl, m);
  if (rc)
    return rc;
  uint64_t expires = micros() + 3000000;
  for (;;) {
    uint64_t now = micros();
    if (now >= expires)
      return -ETIMEDOUT;
    struct pollfd f = {.fd = nl->fd, .events = POLLIN};
    rc = poll(&f, 1, (expires - now + 999) / 1000);
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      return -errno;
    }
    if (!rc)
      return -ETIMEDOUT;
    unsigned char buf[32768];
    ssize_t len = recv(nl->fd, buf, sizeof(buf), 0);
    if (len < 0)
      return -errno;
    for (struct nlmsghdr *h = (void *)buf; NLMSG_OK(h, len);
         h = NLMSG_NEXT(h, len)) {
      if (h->nlmsg_seq != nl->seq)
        continue;
      if (h->nlmsg_type == NLMSG_ERROR)
        return ((struct nlmsgerr *)NLMSG_DATA(h))->error;
      if (h->nlmsg_type == NLMSG_DONE)
        return *(int *)NLMSG_DATA(h);
      if (reply && (rc = reply(h, context)))
        return rc;
    }
  }
}
static int family_reply(struct nlmsghdr *h, void *context) {
  struct netlink *nl = context;
  const struct nlattr *a[CTRL_ATTR_MAX + 1];
  attrs(h, a, CTRL_ATTR_MAX + 1);
  uint16_t id = 0;
  if (length(a[CTRL_ATTR_FAMILY_ID]) == 2)
    memcpy(&id, value(a[CTRL_ATTR_FAMILY_ID]), 2);
  nl->family = id;
  if (a[CTRL_ATTR_MCAST_GROUPS]) {
    const unsigned char *pos = value(a[CTRL_ATTR_MCAST_GROUPS]);
    size_t len = length(a[CTRL_ATTR_MCAST_GROUPS]);
    while (len >= NLA_HDRLEN) {
      const struct nlattr *group = (void *)pos;
      if (group->nla_len < NLA_HDRLEN ||
          (size_t)NLA_ALIGN(group->nla_len) > len)
        break;
      const struct nlattr *g[CTRL_ATTR_MCAST_GRP_MAX + 1];
      parse(value(group), length(group), g, CTRL_ATTR_MCAST_GRP_MAX + 1);
      if (g[CTRL_ATTR_MCAST_GRP_NAME] &&
          !strcmp(value(g[CTRL_ATTR_MCAST_GRP_NAME]), "mlme")) {
        uint32_t membership = number(g[CTRL_ATTR_MCAST_GRP_ID]);
        if (setsockopt(nl->fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &membership,
                       sizeof(membership)))
          return -errno;
      }
      size_t step = NLA_ALIGN(group->nla_len);
      pos += step;
      len -= step;
    }
  }
  return id ? 0 : -ENOENT;
}
static int interface_reply(struct nlmsghdr *h, void *context) {
  struct run *r = context;
  const struct nlattr *a[NL80211_ATTR_MAX + 1];
  attrs(h, a, NL80211_ATTR_MAX + 1);
  if (number(a[NL80211_ATTR_WIPHY]) != r->phy)
    return 0;
  if (number(a[NL80211_ATTR_IFTYPE]) == NL80211_IFTYPE_NAN &&
      length(a[NL80211_ATTR_WDEV]) == 8) {
    if (r->nmi)
      return -EEXIST;
    memcpy(&r->nmi, value(a[NL80211_ATTR_WDEV]), 8);
    if (length(a[NL80211_ATTR_MAC]) != 6)
      return -EINVAL;
    memcpy(r->local, value(a[NL80211_ATTR_MAC]), 6);
  }
  if (a[NL80211_ATTR_IFNAME] &&
      !strcmp(value(a[NL80211_ATTR_IFNAME]), "awdl0")) {
    if (number(a[NL80211_ATTR_IFTYPE]) != NL80211_IFTYPE_NAN_DATA)
      return -EINVAL;
    r->ndi = number(a[NL80211_ATTR_IFINDEX]);
  }
  return 0;
}
static int vendor(struct run *r, const void *data, size_t len) {
  struct message m;
  begin(&r->cmd, &m, r->cmd.family, NL80211_CMD_VENDOR);
  put32(&m, NL80211_ATTR_IFINDEX, r->ndi);
  put32(&m, NL80211_ATTR_VENDOR_ID, 0x001735);
  put32(&m, NL80211_ATTR_VENDOR_SUBCMD, 0x4157444c);
  put(&m, NL80211_ATTR_VENDOR_DATA, data, len);
  return request(&r->cmd, &m, NULL, NULL);
}
static int ip(char *const argv[]) {
  pid_t child = fork();
  if (child < 0)
    return -errno;
  if (!child) {
    execvp("ip", argv);
    _exit(127);
  }
  int status;
  while (waitpid(child, &status, 0) < 0)
    if (errno != EINTR)
      return -errno;
  return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -EIO;
}
static void linklocal(const unsigned char *mac, char *out) {
  unsigned char addr[16] = {0xfe, 0x80};
  memcpy(addr + 8, mac, 3);
  addr[8] ^= 2;
  addr[11] = 0xff;
  addr[12] = 0xfe;
  memcpy(addr + 13, mac + 3, 3);
  inet_ntop(AF_INET6, addr, out, INET6_ADDRSTRLEN);
}
static int create_data(struct run *r) {
  struct message m;
  begin(&r->cmd, &m, r->cmd.family, NL80211_CMD_NEW_INTERFACE);
  put32(&m, NL80211_ATTR_WIPHY, r->phy);
  put32(&m, NL80211_ATTR_IFTYPE, NL80211_IFTYPE_NAN_DATA);
  put(&m, NL80211_ATTR_IFNAME, "awdl0", 6);
  put(&m, NL80211_ATTR_MAC, r->local, 6);
  put(&m, NL80211_ATTR_SOCKET_OWNER, NULL, 0);
  int rc = request(&r->cmd, &m, NULL, NULL);
  if (rc)
    return rc;
  r->ndi = if_nametoindex("awdl0");
  if (!r->ndi)
    return -ENODEV;
  unsigned char mode = 1;
  rc = vendor(r, &mode, 1);
  if (rc)
    return rc;
  rc =
      ip((char *[]){"ip", "link", "set", "awdl0", "addrgenmode", "none", NULL});
  if (rc)
    return rc;
  return ip(
      (char *[]){"ip", "link", "set", "awdl0", "mtu", "1280", "up", NULL});
}
static int peer_caps(struct run *r, const unsigned char *caps) {
  struct message m;
  unsigned char ht[26] = {0};
  // AWDL HT TLV has a two-byte prefix before the IEEE HT capability.
  memcpy(ht, caps + 14, 6);
  begin(&r->cmd, &m, r->cmd.family, NL80211_CMD_SET_STATION);
  put(&m, NL80211_ATTR_WDEV, &r->nmi, 8);
  put(&m, NL80211_ATTR_MAC, r->peer, 6);
  put(&m, NL80211_ATTR_HT_CAPABILITY, ht, sizeof(ht));
  put(&m, NL80211_ATTR_VHT_CAPABILITY, caps, 12);
  return request(&r->cmd, &m, NULL, NULL);
}
static int add_peer(struct run *r, const unsigned char *data) {
  unsigned char add[7] = {2};
  memcpy(add + 1, data, 6);
  int rc = vendor(r, add, sizeof(add));
  if (rc)
    return rc;
  memcpy(r->peer, data, 6);
  rc = peer_caps(r, data + 6);
  if (rc)
    return rc;
  struct message m;
  begin(&r->cmd, &m, r->cmd.family, NL80211_CMD_NAN_SET_PEER_SCHED);
  put(&m, NL80211_ATTR_WDEV, &r->nmi, 8);
  put(&m, NL80211_ATTR_MAC, data, 6);
  put16(&m, NL80211_ATTR_NAN_COMMITTED_DW, 0);
  put16(&m, NL80211_ATTR_NAN_MAX_CHAN_SWITCH_TIME, 4000);
  put8(&m, NL80211_ATTR_NAN_SEQ_ID, 1);
  size_t ch = nest(&m, NL80211_ATTR_NAN_CHANNEL);
  put32(&m, NL80211_ATTR_WIPHY_FREQ, 5745);
  put32(&m, NL80211_ATTR_CENTER_FREQ1, 5775);
  put32(&m, NL80211_ATTR_CENTER_FREQ2, 0);
  put32(&m, NL80211_ATTR_CHANNEL_WIDTH, NL80211_CHAN_WIDTH_80);
  /* Operating class 128, center 155 (bit 5), primary 149 (bit 0). */
  unsigned char entry[6] = {128, 32, 0, 1, 0, 0};
  put(&m, NL80211_ATTR_NAN_CHANNEL_ENTRY, entry, sizeof(entry));
  put8(&m, NL80211_ATTR_NAN_RX_NSS, 2);
  endnest(&m, ch);
  size_t maps = nest(&m, NL80211_ATTR_NAN_PEER_MAPS), map = nest(&m, 1);
  put8(&m, NL80211_NAN_PEER_MAP_ATTR_MAP_ID, 1);
  unsigned char slots[32] = {0};
  put(&m, NL80211_NAN_PEER_MAP_ATTR_TIME_SLOTS, slots, sizeof(slots));
  endnest(&m, map);
  endnest(&m, maps);
  rc = request(&r->cmd, &m, NULL, NULL);
  if (rc)
    return rc;
  begin(&r->cmd, &m, r->cmd.family, NL80211_CMD_NEW_STATION);
  put32(&m, NL80211_ATTR_IFINDEX, r->ndi);
  put(&m, NL80211_ATTR_MAC, data, 6);
  put(&m, NL80211_ATTR_NAN_NMI_MAC, data, 6);
  uint32_t flags = (1u << NL80211_STA_FLAG_AUTHENTICATED) |
                   (1u << NL80211_STA_FLAG_ASSOCIATED) |
                   (1u << NL80211_STA_FLAG_AUTHORIZED);
  struct nl80211_sta_flag_update f = {.mask = flags, .set = flags};
  put(&m, NL80211_ATTR_STA_FLAGS2, &f, sizeof(f));
  rc = request(&r->cmd, &m, NULL, NULL);
  if (rc)
    return rc;
  char local[INET6_ADDRSTRLEN + 4], peer[INET6_ADDRSTRLEN], mac[18];
  linklocal(r->local, local);
  strcat(local, "/64");
  linklocal(data, peer);
  snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", data[0], data[1],
           data[2], data[3], data[4], data[5]);
  rc = ip((char *[]){"ip", "-6", "addr", "replace", local, "dev", "awdl0",
                     "nodad", NULL});
  if (rc)
    return rc;
  rc = ip((char *[]){"ip", "-6", "neigh", "replace", peer, "lladdr", mac, "nud",
                     "permanent", "dev", "awdl0", NULL});
  if (rc)
    return rc;
  printf("{\"event\":\"netdev-ready\",\"interface\":\"awdl0\",\"local\":\"%s\","
         "\"peer\":\"%s\",\"mac\":\"%s\"}\n",
         local, peer, mac);
  return 0;
}
static int delete_data(struct run *r) {
  if (!r->ndi)
    return 0;
  struct message m;
  begin(&r->cmd, &m, r->cmd.family, NL80211_CMD_DEL_INTERFACE);
  put32(&m, NL80211_ATTR_IFINDEX, r->ndi);
  int rc = request(&r->cmd, &m, NULL, NULL);
  if (!rc || rc == -ENODEV) {
    r->ndi = 0;
    return 0;
  }
  return rc;
}
static int emit(void *context, uint32_t kind, const unsigned char *data,
                size_t len) {
  struct run *r = context;
  int rc = 0;
  if (kind == 0) {
    struct message m;
    begin(&r->events, &m, r->events.family, NL80211_CMD_FRAME);
    put(&m, NL80211_ATTR_WDEV, &r->nmi, 8);
    put32(&m, NL80211_ATTR_WIPHY_FREQ, 5745);
    put(&m, NL80211_ATTR_TX_NO_CCK_RATE, NULL, 0);
    put(&m, NL80211_ATTR_FRAME, data, len);
    rc = send_message(&r->events, &m);
    if (!rc)
      r->tx++;
  } else if (kind == 1) {
    rc = len == 34 ? add_peer(r, data) : -EINVAL;
  } else if (kind == 4) {
    rc = -ENOLINK;
  } else if (kind == 5) {
    if (len != 16)
      rc = -EINVAL;
    else {
      uint64_t relative[2];
      unsigned char window[17] = {3};
      memcpy(relative, data, 16);
      for (int i = 0; i < 2; i++) {
        uint64_t absolute =
            relative[i] ? (r->start + relative[i] - 1) * 1000 : 0;
        for (int b = 0; b < 8; b++)
          window[1 + 8 * i + b] = absolute >> (8 * b);
      }
      rc = vendor(r, window, sizeof(window));
    }
  } else if (kind == 7)
    rc = len == 28 ? peer_caps(r, data) : -EINVAL;
  if (rc) {
    fprintf(stderr, "AWDL event %u: %s (%d)\n", kind, strerror(-rc), rc);
    r->error = rc;
    stopped = 1;
  }
  return rc;
}
static int receive(struct run *r) {
  unsigned char buf[32768];
  ssize_t len = recv(r->events.fd, buf, sizeof(buf), 0);
  if (len < 0)
    return -errno;
  for (struct nlmsghdr *h = (void *)buf; NLMSG_OK(h, len);
       h = NLMSG_NEXT(h, len)) {
    if (h->nlmsg_type == NLMSG_ERROR) {
      int rc = ((struct nlmsgerr *)NLMSG_DATA(h))->error;
      if (rc)
        return rc;
    } else if (h->nlmsg_type == r->events.family) {
      struct genlmsghdr *g = NLMSG_DATA(h);
      const struct nlattr *a[NL80211_ATTR_MAX + 1];
      attrs(h, a, NL80211_ATTR_MAX + 1);
      uint64_t wdev = 0;
      if (length(a[NL80211_ATTR_WDEV]) == 8)
        memcpy(&wdev, value(a[NL80211_ATTR_WDEV]), 8);
      if (wdev != r->nmi)
        continue;
      if (g->cmd == NL80211_CMD_FRAME && a[NL80211_ATTR_FRAME]) {
        r->rx++;
        control_rx(r->node, value(a[NL80211_ATTR_FRAME]),
                   length(a[NL80211_ATTR_FRAME]), micros() - r->start + 1);
      } else if (g->cmd == NL80211_CMD_FRAME_TX_STATUS &&
                 length(a[NL80211_ATTR_FRAME]) >= 29 &&
                 !memcmp((const unsigned char *)value(a[NL80211_ATTR_FRAME]) +
                             24,
                         (const unsigned char[]){127, 0, 0x17, 0xf2, 8}, 5))
        r->completions++;
    }
  }
  return 0;
}
int main(int argc, char **argv) {
  if ((argc != 3 && argc != 5) || strncmp(argv[1], "phy", 3))
    return 2;
  int stopping = !strcmp(argv[2], "stop");
  if (!stopping && (argc != 5 || strcmp(argv[2], "149")))
    return 2;
  char *end;
  unsigned long phy = strtoul(argv[1] + 3, &end, 10);
  if (*end || end == argv[1] + 3)
    return 2;
  struct run r = {.phy = phy, .cmd.fd = -1, .events.fd = -1};
  setvbuf(stdout, NULL, _IOLBF, 0);
  signal(SIGTERM, stop_signal);
  signal(SIGINT, stop_signal);
  int rc = open_netlink(&r.events);
  if (rc)
    goto close;
  struct message m;
  begin(&r.events, &m, GENL_ID_CTRL, CTRL_CMD_GETFAMILY);
  put(&m, CTRL_ATTR_FAMILY_NAME, "nl80211", 8);
  rc = request(&r.events, &m, family_reply, &r.events);
  if (rc)
    goto close;
  r.cmd.family = r.events.family;
  rc = open_netlink(&r.cmd);
  if (rc)
    goto close;
  begin(&r.cmd, &m, r.cmd.family, NL80211_CMD_GET_INTERFACE);
  ((struct nlmsghdr *)m.bytes)->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  rc = request(&r.cmd, &m, interface_reply, &r);
  if (rc)
    goto close;
  if (stopping) {
    rc = delete_data(&r);
    goto close;
  }
  if (r.ndi) {
    rc = -EEXIST;
    goto close;
  }
  if (!r.nmi) {
    rc = -ENODEV;
    goto close;
  }
  begin(&r.events, &m, r.events.family, NL80211_CMD_REGISTER_FRAME);
  put(&m, NL80211_ATTR_WDEV, &r.nmi, 8);
  put16(&m, NL80211_ATTR_FRAME_TYPE, 0xd0);
  unsigned char pattern[] = {127, 0, 0x17, 0xf2, 8};
  put(&m, NL80211_ATTR_FRAME_MATCH, pattern, sizeof(pattern));
  put(&m, NL80211_ATTR_RECEIVE_MULTICAST, NULL, 0);
  rc = request(&r.events, &m, NULL, NULL);
  if (rc)
    goto close;
  rc = create_data(&r);
  if (rc)
    goto cleanup;
  r.start = micros();
  r.node = control_new(r.local, 149, 0, argv[3], argv[4], NULL, emit, &r);
  printf("{\"event\":\"started\"}\n");
  while (!stopped) {
    uint64_t next = r.start + control_tick(r.node, micros() - r.start + 1) - 1;
    if (stopped)
      break;
    uint64_t now = micros();
    int wait = next > now ? (next - now + 999) / 1000 : 0;
    struct pollfd f[2] = {{.fd = r.events.fd, .events = POLLIN},
                          {.fd = STDIN_FILENO, .events = POLLIN}};
    int got = poll(f, 2, wait);
    if (got < 0) {
      if (errno == EINTR)
        continue;
      rc = -errno;
      break;
    }
    if (f[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      rc = -EIO;
      break;
    }
    if (f[1].revents & (POLLIN | POLLHUP)) {
      char input[128];
      if (!fgets(input, sizeof(input), stdin))
        break;
      input[strcspn(input, "\r\n")] = 0;
      if (!strcmp(input, "STOP"))
        break;
      if (!strncmp(input, "SELECT ", 7) &&
          (rc = control_select(r.node, input + 7)))
        break;
    }
    if ((f[0].revents & POLLIN) && (rc = receive(&r)))
      break;
  }
  if (r.error)
    rc = r.error;
cleanup:;
  int cleanup = delete_data(&r);
  printf("{\"event\":\"stopped\",\"rc\":%d,\"cleanup\":%d,\"control_rx\":%u,"
         "\"control_tx\":%u,\"tx_completed\":%u}\n",
         rc, cleanup, r.rx, r.tx, r.completions);
  if (r.node)
    control_free(r.node);
  if (cleanup)
    rc = cleanup;
close:
  if (rc)
    fprintf(stderr, "BE200 AWDL: %s (%d)\n", strerror(-rc), rc);
  if (r.cmd.fd >= 0)
    close(r.cmd.fd);
  if (r.events.fd >= 0)
    close(r.events.fd);
  return rc ? 1 : 0;
}

#include "backend.h"
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <linux/genetlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
struct wa_backend {
  int cmd, events, family, ifindex;
  uint32_t seq;
  wa_event_fn callback;
  void *ctx;
};
struct message {
  unsigned char bytes[8192];
  size_t len;
};
static int put(struct message *m, unsigned type, const void *data, size_t len) {
  struct nlattr a = {.nla_len = NLA_HDRLEN + len, .nla_type = type};
  size_t n = NLA_ALIGN(a.nla_len);
  if (m->len + n > sizeof(m->bytes))
    return -EMSGSIZE;
  memcpy(m->bytes + m->len, &a, NLA_HDRLEN);
  memcpy(m->bytes + m->len + NLA_HDRLEN, data, len);
  memset(m->bytes + m->len + a.nla_len, 0, n - a.nla_len);
  m->len += n;
  return 0;
}
static void begin(struct message *m, int family, int command, uint32_t seq) {
  struct nlmsghdr n = {.nlmsg_len = NLMSG_HDRLEN + GENL_HDRLEN,
                       .nlmsg_type = family,
                       .nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK,
                       .nlmsg_seq = seq};
  struct genlmsghdr g = {.cmd = command, .version = 1};
  memset(m, 0, sizeof(*m));
  memcpy(m->bytes, &n, NLMSG_HDRLEN);
  memcpy(m->bytes + NLMSG_HDRLEN, &g, GENL_HDRLEN);
  m->len = n.nlmsg_len;
}
static int attrs(const void *p, size_t len, const struct nlattr **out,
                 size_t count) {
  memset(out, 0, count * sizeof(*out));
  while (len) {
    const struct nlattr *a = p;
    if (len < NLA_HDRLEN || a->nla_len < NLA_HDRLEN || a->nla_len > len ||
        (size_t)NLA_ALIGN(a->nla_len) > len)
      return -EBADMSG;
    unsigned type = a->nla_type & NLA_TYPE_MASK;
    if (type < count)
      out[type] = a;
    len -= NLA_ALIGN(a->nla_len);
    p = (const char *)p + NLA_ALIGN(a->nla_len);
  }
  return 0;
}
static const void *value(const struct nlattr *a) {
  return (const char *)a + NLA_HDRLEN;
}
static size_t size(const struct nlattr *a) {
  return a ? a->nla_len - NLA_HDRLEN : 0;
}
static uint32_t u32(const struct nlattr *a) {
  uint32_t n = 0;
  if (size(a) == 4)
    memcpy(&n, value(a), 4);
  return n;
}
static int socket_open(void) {
  int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
  struct sockaddr_nl addr = {.nl_family = AF_NETLINK};
  if (fd >= 0 && bind(fd, (void *)&addr, sizeof(addr))) {
    close(fd);
    return -1;
  }
  return fd;
}
static int exchange(struct wa_backend *b, struct message *m,
                    struct message *reply) {
  struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
  struct timespec start, now;
  struct pollfd p = {.fd = b->cmd, .events = POLLIN};
  ((struct nlmsghdr *)m->bytes)->nlmsg_len = m->len;
  if (sendto(b->cmd, m->bytes, m->len, 0, (void *)&kernel, sizeof(kernel)) < 0)
    return -errno;
  clock_gettime(CLOCK_MONOTONIC, &start);
  for (;;) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    int remaining = 5000 - (now.tv_sec - start.tv_sec) * 1000 -
                    (now.tv_nsec - start.tv_nsec) / 1000000;
    if (remaining <= 0)
      return -ETIMEDOUT;
    int ret = poll(&p, 1, remaining);
    if (ret < 0 && errno == EINTR)
      continue;
    if (ret <= 0)
      return ret ? -errno : -ETIMEDOUT;
    unsigned char buf[16384];
    ssize_t got = recv(b->cmd, buf, sizeof(buf), 0);
    if (got < 0)
      return -errno;
    for (struct nlmsghdr *n = (void *)buf; NLMSG_OK(n, got);
         n = NLMSG_NEXT(n, got)) {
      if (n->nlmsg_seq != b->seq)
        continue;
      if (n->nlmsg_type == NLMSG_ERROR) {
        if (n->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr)))
          return -EBADMSG;
        return ((struct nlmsgerr *)NLMSG_DATA(n))->error;
      }
      if (reply && n->nlmsg_len <= sizeof(reply->bytes)) {
        memcpy(reply->bytes, n, n->nlmsg_len);
        reply->len = n->nlmsg_len;
      }
    }
  }
}
struct wa_backend *wa_backend_open(const char *interface, wa_event_fn callback,
                                   void *ctx) {
  struct wa_backend *b = calloc(1, sizeof(*b));
  struct message m, reply = {0};
  const struct nlattr *a[CTRL_ATTR_MAX + 1];
  if (!b)
    return NULL;
  b->cmd = b->events = -1;
  b->callback = callback;
  b->ctx = ctx;
  b->ifindex = if_nametoindex(interface);
  if (!b->ifindex)
    goto fail;
  b->cmd = socket_open();
  b->events = socket_open();
  if (b->cmd < 0 || b->events < 0)
    goto fail;
  begin(&m, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, ++b->seq);
  put(&m, CTRL_ATTR_FAMILY_NAME, "nl80211", 8);
  int ret = exchange(b, &m, &reply);
  if (ret) {
    errno = -ret;
    goto fail;
  }
  if (reply.len < NLMSG_HDRLEN + GENL_HDRLEN ||
      attrs(reply.bytes + NLMSG_HDRLEN + GENL_HDRLEN,
            reply.len - NLMSG_HDRLEN - GENL_HDRLEN, a, CTRL_ATTR_MAX + 1))
    goto fail;
  if (size(a[CTRL_ATTR_FAMILY_ID]) != 2 || !a[CTRL_ATTR_MCAST_GROUPS])
    goto fail;
  uint16_t family;
  memcpy(&family, value(a[CTRL_ATTR_FAMILY_ID]), 2);
  b->family = family;
  const unsigned char *pos = value(a[CTRL_ATTR_MCAST_GROUPS]);
  size_t left = size(a[CTRL_ATTR_MCAST_GROUPS]);
  int joined = 0;
  while (left >= NLA_HDRLEN) {
    const struct nlattr *g = (void *)pos, *v[CTRL_ATTR_MCAST_GRP_MAX + 1];
    if (g->nla_len < NLA_HDRLEN || (size_t)NLA_ALIGN(g->nla_len) > left)
      goto fail;
    if (attrs(value(g), size(g), v, CTRL_ATTR_MCAST_GRP_MAX + 1))
      goto fail;
    if (size(v[CTRL_ATTR_MCAST_GRP_NAME]) == 7 &&
        !memcmp(value(v[CTRL_ATTR_MCAST_GRP_NAME]), "vendor", 7)) {
      int group = u32(v[CTRL_ATTR_MCAST_GRP_ID]);
      if (setsockopt(b->events, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &group,
                     sizeof(group)))
        goto fail;
      joined = 1;
    }
    left -= NLA_ALIGN(g->nla_len);
    pos += NLA_ALIGN(g->nla_len);
  }
  if (!joined) {
    errno = ENOENT;
    goto fail;
  }
  return b;
fail:
  wa_backend_close(b);
  return NULL;
}
void wa_backend_close(struct wa_backend *b) {
  if (!b)
    return;
  if (b->cmd >= 0)
    close(b->cmd);
  if (b->events >= 0)
    close(b->events);
  free(b);
}
int wa_backend_fd(struct wa_backend *b) { return b->events; }
static void vendor_begin(struct wa_backend *b, struct message *m,
                         struct message *v, uint32_t op) {
  uint32_t version = WA_NAN_ABI_VERSION, oui = WA_NAN_VENDOR_ID,
           sub = WA_NAN_VENDOR_SUBCMD, index = b->ifindex;
  begin(m, b->family, NL80211_CMD_VENDOR, ++b->seq);
  put(m, NL80211_ATTR_IFINDEX, &index, 4);
  put(m, NL80211_ATTR_VENDOR_ID, &oui, 4);
  put(m, NL80211_ATTR_VENDOR_SUBCMD, &sub, 4);
  memset(v, 0, sizeof(*v));
  put(v, WA_NAN_ATTR_VERSION, &version, 4);
  put(v, WA_NAN_ATTR_OP, &op, 4);
}
int wa_backend_command(struct wa_backend *b, uint32_t op, uint64_t cookie,
                       uint32_t freq, const void *frame, size_t len) {
  struct message m, v;
  if (len > WA_NAN_FRAME_MAX)
    return -EMSGSIZE;
  vendor_begin(b, &m, &v, op);
  if (cookie)
    put(&v, WA_NAN_ATTR_COOKIE, &cookie, 8);
  if (freq)
    put(&v, WA_NAN_ATTR_FREQ, &freq, 4);
  if (len)
    put(&v, WA_NAN_ATTR_FRAME, frame, len);
  put(&m, NL80211_ATTR_VENDOR_DATA | NLA_F_NESTED, v.bytes, v.len);
  return exchange(b, &m, NULL);
}
/* Encode peer and explicitly owned CAM key operations. */
static int security_request(struct wa_backend *b, struct message *m,
                            uint32_t op, const uint8_t *peer, uint32_t cipher,
                            uint32_t index, const uint8_t *key, size_t len,
                            const uint32_t *owner, uint64_t key_cookie) {
  struct message v;
  int local = owner && *owner == WA_NAN_KEY_OWNER_LOCAL_NMI_IGTK;
  if (owner && *owner != WA_NAN_KEY_OWNER_PAIRWISE && !local)
    return -EOPNOTSUPP;
  if (local ? peer != NULL :
      (!peer || (peer[0] & 1) || !memcmp(peer, "\0\0\0\0\0\0", 6)))
    return -EINVAL;
  int is_key = op == WA_NAN_OP_KEY_ADD || op == WA_NAN_OP_KEY_DEL;
  if (!is_key && op != WA_NAN_OP_PEER_ADD && op != WA_NAN_OP_PEER_DEL)
    return -EOPNOTSUPP;
  if (owner && !is_key)
    return -EOPNOTSUPP;
  if (is_key && (local ? (cipher != 0x000fac06 || (index != 4 && index != 5))
                       : (cipher != 0x000fac04 || index != 0)))
    return -EOPNOTSUPP;
  if ((op == WA_NAN_OP_KEY_ADD && (!key || len != 16)) ||
      (op != WA_NAN_OP_KEY_ADD && (key || len)))
    return -EINVAL;
  vendor_begin(b, m, &v, op);
  if (key_cookie)
    put(&v, WA_NAN_ATTR_KEY_COOKIE, &key_cookie, sizeof(key_cookie));
  if (owner)
    put(&v, WA_NAN_ATTR_KEY_OWNER, owner, sizeof(*owner));
  if (!local)
    put(&v, WA_NAN_ATTR_PEER_ADDR, peer, 6);
  if (is_key) {
    put(&v, WA_NAN_ATTR_KEY_CIPHER, &cipher, sizeof(cipher));
    put(&v, WA_NAN_ATTR_KEY_INDEX, &index, sizeof(index));
    if (op == WA_NAN_OP_KEY_ADD)
      put(&v, WA_NAN_ATTR_KEY_DATA, key, len);
  }
  put(m, NL80211_ATTR_VENDOR_DATA | NLA_F_NESTED, v.bytes, v.len);
  /* Both netlink envelopes contain a temporary TK copy. */
  volatile unsigned char *wipe = v.bytes;
  for (size_t i = 0; i < sizeof(v.bytes); i++)
    wipe[i] = 0;
  return 0;
}
static int security_command(struct wa_backend *b, uint32_t op,
                            const uint8_t *peer, uint32_t cipher,
                            uint32_t index, const uint8_t *key, size_t len,
                            const uint32_t *owner, uint64_t key_cookie) {
  struct message m;
  int r = security_request(b, &m, op, peer, cipher, index, key, len, owner, key_cookie);
  if (r)
    return r;
  r = exchange(b, &m, NULL);
  volatile unsigned char *wipe = m.bytes;
  for (size_t i = 0; i < sizeof(m.bytes); i++)
    wipe[i] = 0;
  return r;
}
int wa_backend_peer(struct wa_backend *b, uint32_t op, const uint8_t *peer) {
  if (op != WA_NAN_OP_PEER_ADD && op != WA_NAN_OP_PEER_DEL)
    return -EOPNOTSUPP;
  return security_command(b, op, peer, 0, 0, NULL, 0, NULL, 0);
}
int wa_backend_key(struct wa_backend *b, uint32_t op, const uint8_t *peer,
                   uint32_t cipher, uint32_t index, const uint8_t *key,
                   size_t len) {
  if (op != WA_NAN_OP_KEY_ADD && op != WA_NAN_OP_KEY_DEL)
    return -EOPNOTSUPP;
  return security_command(b, op, peer, cipher, index, key, len, NULL, 0);
}
int wa_backend_owned_key(struct wa_backend *b, uint32_t op, uint32_t owner,
                         const uint8_t *peer, uint32_t cipher, uint32_t index,
                         const uint8_t *key, size_t len) {
  if (op != WA_NAN_OP_KEY_ADD && op != WA_NAN_OP_KEY_DEL)
    return -EOPNOTSUPP;
  return security_command(b, op, peer, cipher, index, key, len, &owner, 0);
}
int wa_backend_pairwise_key(struct wa_backend *b, uint32_t op,
                            const uint8_t *peer, uint64_t key_cookie,
                            const uint8_t *key, size_t len) {
  if (op != WA_NAN_OP_KEY_ADD && op != WA_NAN_OP_KEY_DEL)
    return -EOPNOTSUPP;
  if (!key_cookie) return -EINVAL;
  return security_command(b, op, peer, 0x000fac04, 0, key, len, NULL, key_cookie);
}
int wa_backend_bigtk(struct wa_backend *b, uint32_t index,
                     const uint8_t pn[6], const uint8_t key[16]) {
  struct message m, v;
  uint32_t owner = WA_NAN_KEY_OWNER_LOCAL_NMI_BIGTK, cipher = 0x000fac06;
  if ((index != 6 && index != 7) || !pn || !key) return -EINVAL;
  vendor_begin(b, &m, &v, WA_NAN_OP_KEY_ADD);
  put(&v, WA_NAN_ATTR_KEY_OWNER, &owner, sizeof(owner));
  put(&v, WA_NAN_ATTR_KEY_CIPHER, &cipher, sizeof(cipher));
  put(&v, WA_NAN_ATTR_KEY_INDEX, &index, sizeof(index));
  put(&v, WA_NAN_ATTR_TX_PN, pn, 6);
  put(&v, WA_NAN_ATTR_KEY_DATA, key, 16);
  put(&m, NL80211_ATTR_VENDOR_DATA | NLA_F_NESTED, v.bytes, v.len);
  int r = exchange(b, &m, NULL);
  volatile unsigned char *wipe = v.bytes;
  for (size_t i=0;i<sizeof(v.bytes);i++) wipe[i]=0;
  wipe = m.bytes;
  for (size_t i=0;i<sizeof(m.bytes);i++) wipe[i]=0;
  return r;
}
int wa_backend_protected_tx(struct wa_backend *b, uint64_t cookie, uint32_t freq,
                            const void *frame, size_t len, const uint8_t *peer,
                            uint64_t key_cookie) {
  struct message m, v;
  uint32_t protection = WA_NAN_TX_PAIRWISE_CCMP128, index = 0;
  if (len > WA_NAN_FRAME_MAX) return -EMSGSIZE;
  if (!cookie || !key_cookie || !frame || len < 31 || !peer ||
      (peer[0] & 1) || !memcmp(peer, "\0\0\0\0\0\0", 6)) return -EINVAL;
  vendor_begin(b, &m, &v, WA_NAN_OP_TX);
  put(&v, WA_NAN_ATTR_COOKIE, &cookie, sizeof(cookie));
  put(&v, WA_NAN_ATTR_FREQ, &freq, sizeof(freq));
  put(&v, WA_NAN_ATTR_FRAME, frame, len);
  put(&v, WA_NAN_ATTR_TX_PROTECTION, &protection, sizeof(protection));
  put(&v, WA_NAN_ATTR_PEER_ADDR, peer, 6);
  put(&v, WA_NAN_ATTR_KEY_INDEX, &index, sizeof(index));
  put(&v, WA_NAN_ATTR_KEY_COOKIE, &key_cookie, sizeof(key_cookie));
  put(&m, NL80211_ATTR_VENDOR_DATA | NLA_F_NESTED, v.bytes, v.len);
  return exchange(b, &m, NULL);
}
static int vendor_attrs(struct message *m, const struct nlattr **v,
                        int *index) {
  const struct nlattr *a[NL80211_ATTR_MAX + 1];
  if (m->len < NLMSG_HDRLEN + GENL_HDRLEN)
    return -EBADMSG;
  if (attrs(m->bytes + NLMSG_HDRLEN + GENL_HDRLEN,
            m->len - NLMSG_HDRLEN - GENL_HDRLEN, a, NL80211_ATTR_MAX + 1))
    return -EBADMSG;
  if (!a[NL80211_ATTR_VENDOR_DATA])
    return -ENOENT;
  if (index && (u32(a[NL80211_ATTR_VENDOR_ID]) != WA_NAN_VENDOR_ID ||
                u32(a[NL80211_ATTR_VENDOR_SUBCMD]) != WA_NAN_VENDOR_SUBCMD))
    return -ENOENT;
  if (index)
    *index = u32(a[NL80211_ATTR_IFINDEX]);
  return attrs(value(a[NL80211_ATTR_VENDOR_DATA]),
               size(a[NL80211_ATTR_VENDOR_DATA]), v, WA_NAN_ATTR_MAX + 1);
}
int wa_backend_caps(struct wa_backend *b, struct wa_caps *caps) {
  struct message m, v, reply = {0};
  const struct nlattr *a[WA_NAN_ATTR_MAX + 1];
  vendor_begin(b, &m, &v, WA_NAN_OP_GET_CAPS);
  put(&m, NL80211_ATTR_VENDOR_DATA | NLA_F_NESTED, v.bytes, v.len);
  int ret = exchange(b, &m, &reply);
  if (ret)
    return ret;
  if ((ret = vendor_attrs(&reply, a, NULL)))
    return ret;
  if (size(a[WA_NAN_ATTR_NMI_ADDR]) != 6 ||
      size(a[WA_NAN_ATTR_CLUSTER_ID]) != 6 ||
      (a[WA_NAN_ATTR_HT_CAP] && size(a[WA_NAN_ATTR_HT_CAP]) != WA_NAN_HT_CAP_SIZE) ||
      (a[WA_NAN_ATTR_HT_CAP_5G] && size(a[WA_NAN_ATTR_HT_CAP_5G]) != WA_NAN_HT_CAP_SIZE) ||
      (a[WA_NAN_ATTR_VHT_CAP] && size(a[WA_NAN_ATTR_VHT_CAP]) != WA_NAN_VHT_CAP_SIZE) ||
      (a[WA_NAN_ATTR_HE_CAP] && size(a[WA_NAN_ATTR_HE_CAP]) != WA_NAN_HE_CAP_SIZE) ||
      (a[WA_NAN_ATTR_CHANNELS] && (size(a[WA_NAN_ATTR_CHANNELS]) > sizeof(caps->channels) ||
       size(a[WA_NAN_ATTR_CHANNELS]) % sizeof(caps->channels[0]))))
    return -EBADMSG;
  memset(caps, 0, sizeof(*caps));
  caps->version = u32(a[WA_NAN_ATTR_VERSION]);
  caps->features = u32(a[WA_NAN_ATTR_FEATURES]);
  caps->max_frame = u32(a[WA_NAN_ATTR_MAX_FRAME]);
  caps->freq = u32(a[WA_NAN_ATTR_FREQ]);
  caps->drops = u32(a[WA_NAN_ATTR_EVENT_DROPS]);
  caps->oversize = u32(a[WA_NAN_ATTR_RX_OVERSIZE]);
  memcpy(caps->nmi, value(a[WA_NAN_ATTR_NMI_ADDR]), 6);
  memcpy(caps->cluster, value(a[WA_NAN_ATTR_CLUSTER_ID]), 6);
  if (a[WA_NAN_ATTR_HT_CAP]) {
    caps->ht_supported = 1;
    memcpy(caps->ht_cap, value(a[WA_NAN_ATTR_HT_CAP]), sizeof(caps->ht_cap));
  }
  if (a[WA_NAN_ATTR_HT_CAP_5G]) {
    caps->ht_5g_supported=1;
    memcpy(caps->ht_cap_5g,value(a[WA_NAN_ATTR_HT_CAP_5G]),sizeof(caps->ht_cap_5g));
  }
  if (a[WA_NAN_ATTR_VHT_CAP]) {
    caps->vht_supported=1;
    memcpy(caps->vht_cap,value(a[WA_NAN_ATTR_VHT_CAP]),sizeof(caps->vht_cap));
  }
  if (a[WA_NAN_ATTR_CHANNELS]) {
    caps->n_channels=size(a[WA_NAN_ATTR_CHANNELS])/sizeof(caps->channels[0]);
    memcpy(caps->channels,value(a[WA_NAN_ATTR_CHANNELS]),size(a[WA_NAN_ATTR_CHANNELS]));
  }
  if (a[WA_NAN_ATTR_HE_CAP]) {
    caps->he_supported=1;
    memcpy(caps->he_cap,value(a[WA_NAN_ATTR_HE_CAP]),sizeof(caps->he_cap));
  }
  return caps->version == WA_NAN_ABI_VERSION ? 0 : -EPROTONOSUPPORT;
}
int wa_backend_local_schedule(struct wa_backend *b, const uint8_t *cluster) {
  static const uint8_t schedule[WA_NAN_SCHEDULE_SIZE] = {
    0, 2, 16, 1, 0, 0, 6, 0, 2
  };
  struct message m, v;
  if (!cluster) return -EINVAL;
  vendor_begin(b, &m, &v, WA_NAN_OP_SET_SCHEDULE);
  put(&v, WA_NAN_ATTR_SCHEDULE, schedule, sizeof(schedule));
  put(&v, WA_NAN_ATTR_CLUSTER_ID, cluster, 6);
  put(&m, NL80211_ATTR_VENDOR_DATA | NLA_F_NESTED, v.bytes, v.len);
  return exchange(b, &m, NULL);
}
int wa_backend_auth_schedule(struct wa_backend *b, const uint8_t *cluster, uint32_t slots) {
  uint8_t schedule[WA_NAN_SCHEDULE_SIZE] = {
    0, 2, 16, 4, 0, 0, 6, 0, 0xfe, 0xff, 0xff, 0xff
  };
  for (unsigned i=0;i<4;i++) schedule[8+i]=slots>>(8*i);
  struct message m, v;
  if (!cluster || !slots || (slots & 1)) return -EINVAL;
  vendor_begin(b, &m, &v, WA_NAN_OP_SET_SCHEDULE);
  put(&v, WA_NAN_ATTR_SCHEDULE, schedule, sizeof(schedule));
  put(&v, WA_NAN_ATTR_CLUSTER_ID, cluster, 6);
  put(&m, NL80211_ATTR_VENDOR_DATA | NLA_F_NESTED, v.bytes, v.len);
  return exchange(b, &m, NULL);
}
int wa_backend_auth_window(struct wa_backend *b, const uint8_t *cluster) {
  return wa_backend_auth_schedule(b, cluster, 0xfffffffe);
}

int wa_backend_peer_schedule(struct wa_backend *b, const uint8_t *cluster,
                              const uint8_t *peer, uint32_t slots,
                              const struct wa_nan_channel *channel) {
  uint8_t schedule[WA_NAN_SCHEDULE_SIZE] = {0,2,16,4,0,0,6,0};
  for (unsigned i=0;i<4;i++) schedule[8+i]=slots>>(8*i);
  struct message m, v;
  if (!cluster || !channel) return -EINVAL;
  schedule[6]=channel->center; schedule[7]=channel->width;
  vendor_begin(b, &m, &v, WA_NAN_OP_SET_SCHEDULE);
  put(&v, WA_NAN_ATTR_DATA_CHANNEL, channel, sizeof(*channel));
  put(&v, WA_NAN_ATTR_SCHEDULE, schedule, sizeof(schedule));
  put(&v, WA_NAN_ATTR_CLUSTER_ID, cluster, 6);
  if (peer) put(&v, WA_NAN_ATTR_PEER_ADDR, peer, 6);
  put(&m, NL80211_ATTR_VENDOR_DATA | NLA_F_NESTED, v.bytes, v.len);
  return exchange(b, &m, NULL);
}
int wa_backend_group_seq(struct wa_backend *b, uint32_t owner, uint32_t index,
                         const uint8_t *ndi, uint8_t pn[6]) {
  struct message m, v, reply = {0};
  const struct nlattr *a[WA_NAN_ATTR_MAX+1];
  vendor_begin(b, &m, &v, WA_NAN_OP_GET_SEQ);
  put(&v, WA_NAN_ATTR_KEY_OWNER, &owner, 4);
  put(&v, WA_NAN_ATTR_KEY_INDEX, &index, 4);
  if (ndi) put(&v, WA_NAN_ATTR_NDI_ADDR, ndi, 6);
  put(&m, NL80211_ATTR_VENDOR_DATA | NLA_F_NESTED, v.bytes, v.len);
  int r = exchange(b, &m, &reply);
  if (!r) r = vendor_attrs(&reply, a, NULL);
  if (r) return r;
  if (size(a[WA_NAN_ATTR_TX_PN]) != 6) return -EBADMSG;
  memcpy(pn, value(a[WA_NAN_ATTR_TX_PN]), 6);
  return 0;
}
int wa_backend_ndi(struct wa_backend *b, uint32_t op,
                   const struct wa_nan_ndi_config *config, uint32_t *ifindex) {
  struct message m, v, reply = {0};
  const struct nlattr *a[WA_NAN_ATTR_MAX+1];
  if (op != WA_NAN_OP_NDI_ADD && op != WA_NAN_OP_NDI_STA_ADD && op != WA_NAN_OP_NDI_CONNECT &&
      op != WA_NAN_OP_NDI_DEL && op != WA_NAN_OP_NDI_IF_ADD &&
      op != WA_NAN_OP_NDI_IF_DEL) return -EINVAL;
  if ((op == WA_NAN_OP_NDI_ADD || op == WA_NAN_OP_NDI_STA_ADD || op == WA_NAN_OP_NDI_CONNECT) && (!config || !ifindex)) return -EINVAL;
  if (op == WA_NAN_OP_NDI_IF_ADD && !ifindex) return -EINVAL;
  bool deletion = op == WA_NAN_OP_NDI_DEL || op == WA_NAN_OP_NDI_IF_DEL;
  vendor_begin(b, &m, &v, op);
  if (config) put(&v, WA_NAN_ATTR_NDI_CONFIG, config, sizeof(*config));
  put(&m, NL80211_ATTR_VENDOR_DATA | NLA_F_NESTED, v.bytes, v.len);
  int r = exchange(b, &m, deletion ? NULL : &reply);
  volatile uint8_t *wipe = m.bytes;
  for (size_t i=0; i<sizeof(m.bytes); i++) wipe[i]=0;
  wipe = v.bytes;
  for (size_t i=0; i<sizeof(v.bytes); i++) wipe[i]=0;
  if (r || deletion) return r;
  r = vendor_attrs(&reply, a, NULL);
  if (r) return r;
  if (size(a[WA_NAN_ATTR_NDI_IFINDEX]) != 4 || !u32(a[WA_NAN_ATTR_NDI_IFINDEX]))
    return -EBADMSG;
  *ifindex = u32(a[WA_NAN_ATTR_NDI_IFINDEX]);
  return 0;
}
int wa_backend_receive(struct wa_backend *b) {
  unsigned char buf[16384];
  ssize_t got = recv(b->events, buf, sizeof(buf), MSG_DONTWAIT);
  if (got < 0)
    return -errno;
  for (struct nlmsghdr *n = (void *)buf; NLMSG_OK(n, got);
       n = NLMSG_NEXT(n, got)) {
    struct message m;
    const struct nlattr *a[WA_NAN_ATTR_MAX + 1];
    int index = 0;
    if (n->nlmsg_type != b->family || n->nlmsg_len > sizeof(m.bytes))
      continue;
    memcpy(m.bytes, n, n->nlmsg_len);
    m.len = n->nlmsg_len;
    if (vendor_attrs(&m, a, &index) || index != b->ifindex)
      continue;
    struct wa_event e = {.type = u32(a[WA_NAN_ATTR_EVENT]),
                         .flags = u32(a[WA_NAN_ATTR_RX_FLAGS]),
                         .freq = u32(a[WA_NAN_ATTR_FREQ]),
                         .status = (int32_t)u32(a[WA_NAN_ATTR_STATUS]),
                         .tx_status = u32(a[WA_NAN_ATTR_TX_STATUS])};
    if (size(a[WA_NAN_ATTR_COOKIE]) == 8)
      memcpy(&e.cookie, value(a[WA_NAN_ATTR_COOKIE]), 8);
    if (size(a[WA_NAN_ATTR_RX_MONOTONIC_NS]) == 8)
      memcpy(&e.rx_monotonic_ns, value(a[WA_NAN_ATTR_RX_MONOTONIC_NS]), 8);
    if (size(a[WA_NAN_ATTR_SWITCH_NS]) == 8)
      memcpy(&e.switch_ns, value(a[WA_NAN_ATTR_SWITCH_NS]), 8);
    if (size(a[WA_NAN_ATTR_ACKED]) == 1)
      memcpy(&e.acked, value(a[WA_NAN_ATTR_ACKED]), 1);
    if (e.type == WA_NAN_EVENT_AWDL_SENDER) {
      if (size(a[WA_NAN_ATTR_PEER_ADDR]) != 6)
        continue;
      memcpy(e.peer, value(a[WA_NAN_ATTR_PEER_ADDR]), 6);
    }
    if (e.type == WA_NAN_EVENT_CLUSTER) {
      if (size(a[WA_NAN_ATTR_CLUSTER_ID]) != 6)
        continue;
      memcpy(e.cluster, value(a[WA_NAN_ATTR_CLUSTER_ID]), 6);
    }
    if (e.type == WA_NAN_EVENT_DW) {
      if (size(a[WA_NAN_ATTR_DW_START]) != 4 ||
          size(a[WA_NAN_ATTR_RX_COUNTER]) != 4)
        continue;
      e.dw_start = u32(a[WA_NAN_ATTR_DW_START]);
      e.counter = u32(a[WA_NAN_ATTR_RX_COUNTER]);
    }
    if (e.flags & WA_NAN_RX_CCMP_VERIFIED) {
      if (size(a[WA_NAN_ATTR_PEER_ADDR]) != 6 ||
          size(a[WA_NAN_ATTR_KEY_INDEX]) != 4 ||
          size(a[WA_NAN_ATTR_KEY_COOKIE]) != 8 ||
          size(a[WA_NAN_ATTR_RX_PN]) != 6)
        continue;
      memcpy(e.peer, value(a[WA_NAN_ATTR_PEER_ADDR]), 6);
      e.key_index = u32(a[WA_NAN_ATTR_KEY_INDEX]);
      memcpy(&e.key_cookie, value(a[WA_NAN_ATTR_KEY_COOKIE]), 8);
      memcpy(e.rx_pn, value(a[WA_NAN_ATTR_RX_PN]), 6);
    }
    e.frame_len = size(a[WA_NAN_ATTR_FRAME]);
    if (e.frame_len)
      e.frame = value(a[WA_NAN_ATTR_FRAME]);
    b->callback(b->ctx, &e);
  }
  return 0;
}
#else
int wa_backend_peer_schedule(struct wa_backend *b, const uint8_t *cluster,
                              const uint8_t *peer, uint32_t slots,
                              const struct wa_nan_channel *channel) {
  (void)b; (void)cluster; (void)peer; (void)slots; (void)channel; return -EOPNOTSUPP;
}
int wa_backend_group_seq(struct wa_backend *b, uint32_t owner, uint32_t index,
                         const uint8_t *ndi, uint8_t pn[6]) {
  (void)b; (void)owner; (void)index; (void)ndi; (void)pn; return -EOPNOTSUPP;
}
int wa_backend_ndi(struct wa_backend *b, uint32_t op,
                   const struct wa_nan_ndi_config *config, uint32_t *ifindex) {
  (void)b; (void)op; (void)config; (void)ifindex; return -EOPNOTSUPP;
}
struct wa_backend *wa_backend_open(const char *i, wa_event_fn f, void *c) {
  (void)i;
  (void)f;
  (void)c;
  errno = EOPNOTSUPP;
  return NULL;
}
void wa_backend_close(struct wa_backend *b) { (void)b; }
int wa_backend_fd(struct wa_backend *b) {
  (void)b;
  return -1;
}
int wa_backend_receive(struct wa_backend *b) {
  (void)b;
  return -EOPNOTSUPP;
}
int wa_backend_caps(struct wa_backend *b, struct wa_caps *c) {
  (void)b;
  (void)c;
  return -EOPNOTSUPP;
}
int wa_backend_command(struct wa_backend *b, uint32_t o, uint64_t c, uint32_t f,
                       const void *p, size_t n) {
  (void)b;
  (void)o;
  (void)c;
  (void)f;
  (void)p;
  (void)n;
  return -EOPNOTSUPP;
}
int wa_backend_peer(struct wa_backend *b, uint32_t op, const uint8_t *peer) {
  (void)b;
  (void)op;
  (void)peer;
  return -EOPNOTSUPP;
}
int wa_backend_key(struct wa_backend *b, uint32_t op, const uint8_t *peer,
                   uint32_t cipher, uint32_t index, const uint8_t *key,
                   size_t len) {
  (void)b;
  (void)op;
  (void)peer;
  (void)cipher;
  (void)index;
  (void)key;
  (void)len;
  return -EOPNOTSUPP;
}
int wa_backend_owned_key(struct wa_backend *b, uint32_t op, uint32_t owner,
                         const uint8_t *peer, uint32_t cipher, uint32_t index,
                         const uint8_t *key, size_t len) {
  (void)b; (void)op; (void)owner; (void)peer; (void)cipher; (void)index;
  (void)key; (void)len;
  return -EOPNOTSUPP;
}
int wa_backend_bigtk(struct wa_backend *b, uint32_t index,
                     const uint8_t pn[6], const uint8_t key[16]) {
  (void)b; (void)index; (void)pn; (void)key;
  return -EOPNOTSUPP;
}
int wa_backend_pairwise_key(struct wa_backend *b, uint32_t op,
                            const uint8_t *peer, uint64_t cookie,
                            const uint8_t *key, size_t len) {
  (void)b; (void)op; (void)peer; (void)cookie; (void)key; (void)len;
  return -EOPNOTSUPP;
}
int wa_backend_protected_tx(struct wa_backend *b, uint64_t cookie, uint32_t freq,
                            const void *frame, size_t len, const uint8_t *peer,
                            uint64_t key_cookie) {
  (void)b; (void)cookie; (void)freq; (void)frame; (void)len;
  (void)peer; (void)key_cookie;
  return -EOPNOTSUPP;
}
int wa_backend_local_schedule(struct wa_backend *b, const uint8_t *cluster) {
  (void)b; (void)cluster;
  return -EOPNOTSUPP;
}
int wa_backend_auth_schedule(struct wa_backend *b, const uint8_t *cluster, uint32_t slots) {
  (void)b; (void)cluster; (void)slots;
  return -EOPNOTSUPP;
}
int wa_backend_auth_window(struct wa_backend *b, const uint8_t *cluster) {
  (void)b; (void)cluster;
  return -EOPNOTSUPP;
}
#endif

#ifndef NAND_BACKEND_H
#define NAND_BACKEND_H
#include "../driver/include/wa_nan_uapi.h"
#include <stddef.h>
#include <stdint.h>
struct wa_caps {
  uint32_t version, features, max_frame, freq, drops, oversize;
  uint8_t nmi[6], cluster[6];
  uint8_t ht_cap[WA_NAN_HT_CAP_SIZE], ht_supported;
  uint8_t ht_cap_5g[WA_NAN_HT_CAP_SIZE], vht_cap[WA_NAN_VHT_CAP_SIZE];
  uint8_t ht_5g_supported, vht_supported;
  uint8_t he_cap[WA_NAN_HE_CAP_SIZE], he_supported;
  struct wa_nan_channel channels[WA_NAN_CHANNEL_MAX];
  size_t n_channels;
};
struct wa_event {
  uint32_t type, flags, freq, tx_status;
  uint32_t dw_start, counter;
  int32_t status;
  uint64_t cookie, key_cookie;
  uint32_t key_index;
  uint8_t peer[6], rx_pn[6], cluster[6];
  uint8_t acked;
  const uint8_t *frame;
  size_t frame_len;
};
struct wa_backend;
typedef void (*wa_event_fn)(void *, const struct wa_event *);
struct wa_backend *wa_backend_open(const char *, wa_event_fn, void *);
void wa_backend_close(struct wa_backend *);
int wa_backend_fd(struct wa_backend *);
int wa_backend_receive(struct wa_backend *);
int wa_backend_caps(struct wa_backend *, struct wa_caps *);
int wa_backend_local_schedule(struct wa_backend *, const uint8_t *cluster);
int wa_backend_auth_schedule(struct wa_backend *, const uint8_t *cluster, uint32_t slots);
int wa_backend_auth_window(struct wa_backend *, const uint8_t *cluster);
int wa_backend_peer_schedule(struct wa_backend *, const uint8_t *cluster,
                              const uint8_t *peer, uint32_t slots,
                              const struct wa_nan_channel *channel);
int wa_backend_group_seq(struct wa_backend *, uint32_t owner, uint32_t index,
                         const uint8_t *ndi, uint8_t pn[6]);
int wa_backend_ndi(struct wa_backend *, uint32_t op,
                   const struct wa_nan_ndi_config *, uint32_t *ifindex);
int wa_backend_command(struct wa_backend *, uint32_t, uint64_t, uint32_t,
                       const void *, size_t);
int wa_backend_peer(struct wa_backend *, uint32_t, const uint8_t *);
int wa_backend_key(struct wa_backend *, uint32_t, const uint8_t *, uint32_t,
                   uint32_t, const uint8_t *, size_t);
int wa_backend_owned_key(struct wa_backend *, uint32_t, uint32_t,
                         const uint8_t *, uint32_t, uint32_t,
                         const uint8_t *, size_t);
int wa_backend_bigtk(struct wa_backend *, uint32_t index, const uint8_t pn[6],
                     const uint8_t key[16]);
int wa_backend_pairwise_key(struct wa_backend *, uint32_t, const uint8_t *,
                            uint64_t, const uint8_t *, size_t);
int wa_backend_protected_tx(struct wa_backend *, uint64_t, uint32_t,
                            const void *, size_t, const uint8_t *, uint64_t);
#endif

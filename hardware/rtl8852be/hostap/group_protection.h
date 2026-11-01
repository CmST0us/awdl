#ifndef WA_GROUP_PROTECTION_H
#define WA_GROUP_PROTECTION_H
#include "crypto/aes_wrap.h"

struct wa_bip_key {
  bool installed;
  u8 peer[6], key[16], index;
  u64 pn;
};
static u64 wa_bip_pn(const u8 *p) {
  u64 n = 0;
  for (unsigned i=0; i<6; i++) n |= (u64)p[i] << (8*i);
  return n;
}
static void wa_bip_seq(u8 *p, u64 n) {
  for (unsigned i=0; i<6; i++) p[i] = n >> (8*i);
}
static int wa_bip_install(struct wa_bip_key *s, const u8 *peer, u8 index,
                           const u8 *key, const u8 *pn) {
  if (s->installed && s->index == index && !memcmp(s->peer, peer, 6) &&
      !memcmp(s->key, key, 16)) {
    if (pn && wa_bip_pn(pn) > s->pn) s->pn = wa_bip_pn(pn);
    return 0;
  }
  forced_memzero(s, sizeof(*s));
  memcpy(s->peer, peer, 6); memcpy(s->key, key, 16);
  s->index = index; s->pn = pn ? wa_bip_pn(pn) : 0; s->installed = true;
  return 0;
}
static int wa_bip_mic(const u8 *key, const u8 *f, size_t n, u8 mic[16]) {
  u8 aad[20], *body;
  const u8 *vectors[2];
  size_t lengths[2] = {sizeof(aad), n-24};
  if (n < 42) return -EINVAL;
  aad[0] = f[0]; aad[1] = f[1] & ~0x38;
  memcpy(aad+2, f+4, 18);
  body = os_memdup(f+24, n-24);
  if (!body) return -ENOMEM;
  memset(body+n-32, 0, 8);
  if (f[0] == 0x80) memset(body, 0, 8);
  vectors[0] = aad; vectors[1] = body;
  int r = omac1_aes_128_vector(key, 2, vectors, lengths, mic);
  bin_clear_free(body, n-24);
  return r ? -EIO : 0;
}
static int wa_bip_sign(struct wa_bip_key *s, struct wpabuf **frame) {
  size_t n = wpabuf_len(*frame);
  if (!s->installed || n < 30 || !(wpabuf_head_u8(*frame)[4]&1)) return -EINVAL;
  if (s->pn == 0xffffffffffffULL) return -EOVERFLOW;
  if (wpabuf_resize(frame, 18)) return -ENOMEM;
  u8 *mmie = wpabuf_put(*frame, 18), mic[16];
  memset(mmie, 0, 18); mmie[0]=WLAN_EID_MMIE; mmie[1]=16;
  WPA_PUT_LE16(mmie+2, s->index); wa_bip_seq(mmie+4, ++s->pn);
  int r = wa_bip_mic(s->key, wpabuf_head(*frame), n+18, mic);
  if (!r) memcpy(mmie+10, mic, 8);
  forced_memzero(mic, sizeof(mic));
  return r;
}
static int wa_bip_verify(struct wa_bip_key *s, const u8 *f, size_t n) {
  if (!s->installed || n < 42 || !(f[4]&1) || memcmp(f+10, s->peer, 6))
    return -ENOENT;
  const u8 *m = f+n-18;
  if (m[0] != WLAN_EID_MMIE || m[1] != 16 || WPA_GET_LE16(m+2) != s->index)
    return -EBADMSG;
  u64 pn = wa_bip_pn(m+4);
  if (pn <= s->pn) return -EALREADY;
  u8 mic[16];
  int r = wa_bip_mic(s->key, f, n, mic);
  if (!r && os_memcmp_const(mic, m+10, 8)) r = -EBADMSG;
  forced_memzero(mic, sizeof(mic));
  if (!r) s->pn = pn;
  return r;
}
#endif

#include "phy.h"
#include "backend.h"
#include "common/ieee802_11_defs.h"
#include "common/ieee802_11_common.h"

/* <=80 MHz TX map; both devices may expose more streams than this radio. */
static u16 wa_mcs_intersection(u16 tx, u16 rx)
{
  u16 map=0xffff;
  for (unsigned i=0;i<2;i++) {
    unsigned a=(tx>>(2*i))&3, b=(rx>>(2*i))&3;
    if (a!=3 && b!=3) map=(map&~(3U<<(2*i)))|((a<b?a:b)<<(2*i));
  }
  return map;
}

static unsigned wa_he_bits(const u8 *p, unsigned bit, unsigned count)
{
  unsigned v=0;
  for (unsigned i=0;i<count;i++) v|=((p[(bit+i)/8]>>((bit+i)%8))&1)<<i;
  return v;
}

static int wa_he_peer(const struct wa_caps *caps, const u8 *p, size_t len,
                      struct wa_nan_ndi_config *c)
{
  if (len<WA_NAN_HE_CAP_SIZE) return -EBADMSG;
  size_t end=21+((p[6]&BIT(3))?4:0)+((p[6]&BIT(4))?4:0);
  if (len<end) return -EBADMSG;
  u16 map=wa_mcs_intersection(WPA_GET_LE16(caps->he_cap+19),WPA_GET_LE16(p+17));
  unsigned streams=((map>>2)&3)==3?1:2;
  unsigned padding=p[15]>>6;
  if (padding==3 && !(p[12]&BIT(7))) return -EBADMSG;
  memset(c->he_padding,padding,3);
  if (p[12]&BIT(7)) {
    if (len==end) return -EBADMSG;
    const u8 *ppe=p+end;
    unsigned nss=(ppe[0]&7)+1, ru=(ppe[0]>>3)&15, nr=0;
    for (unsigned j=0;j<4;j++) nr+=!!(ru&BIT(j));
    if (!nr || len-end<(7+6*nss*nr+7)/8) return -EBADMSG;
    /* Max extension across the negotiated streams/MCS, so RA may select
     * either stream count without outgrowing the CMAC packet extension. */
    memset(c->he_padding,2,3);
    for (unsigned j=0;j<3;j++) if (ru&BIT(j)) c->he_padding[j]=0;
    unsigned bit=7;
    for (unsigned i=0;i<nss;i++) for (unsigned j=0;j<4;j++) if (ru&BIT(j)) {
      unsigned p16=wa_he_bits(ppe,bit,3),p8=wa_he_bits(ppe,bit+3,3);
      unsigned need=p16!=7?2:p8!=7?1:0;
      bit+=6;
      if (i<streams && j<3 && need>c->he_padding[j]) c->he_padding[j]=need;
    }
    if (nss<streams) return -EBADMSG;
  }
  if ((map&3)==3 || (c->channel.width && !(p[6]&caps->he_cap[6]&BIT(2))))
    return 0; /* Keep the negotiated VHT/HT path. */
  c->he=1;
  WPA_PUT_LE16(c->he_mcs,map);
  c->he_ldpc=!!(p[7]&caps->he_cap[7]&BIT(5));
  c->he_gi=(!!(p[7]&caps->he_cap[7]&BIT(6))) |
           (!!(p[13]&caps->he_cap[13]&BIT(2))<<1);
  return 0;
}

int wa_ndp_phy(const struct wa_caps *caps, const u8 *elements, size_t length,
               struct wa_nan_ndi_config *config)
{
  bool five=config->channel.primary>14;
  const u8 *local=five ? caps->ht_cap_5g : caps->ht_cap;
  const u8 *ht=get_ie(elements,length,WLAN_EID_HT_CAP);
  if (!ht || ht[1]!=WA_NAN_HT_CAP_SIZE ||
      !(five ? caps->ht_5g_supported : caps->ht_supported)) return -EOPNOTSUPP;
  config->ht_mcs[0]=ht[5]&local[3]; config->ht_mcs[1]=ht[6]&local[4];
  u16 flags=WPA_GET_LE16(ht+2)&WPA_GET_LE16(local);
  flags=(flags&~HT_CAP_INFO_MAX_AMSDU_SIZE)|(WPA_GET_LE16(ht+2)&HT_CAP_INFO_MAX_AMSDU_SIZE);
  if (!config->channel.width) flags &= ~(HT_CAP_INFO_SUPP_CHANNEL_WIDTH_SET | HT_CAP_INFO_SHORT_GI40MHZ);
  if (!(config->ht_mcs[0]|config->ht_mcs[1])) return -EOPNOTSUPP;
  WPA_PUT_LE16(config->ht_info,flags);
  config->ampdu_density=(ht[4]>>2)&7;
  config->ampdu_exp=ht[4]&3;
  config->vht=0; config->he=0;
  const u8 *vht=get_ie(elements,length,WLAN_EID_VHT_CAP);
  if (five && caps->vht_supported && vht && vht[1]==WA_NAN_VHT_CAP_SIZE) {
    u16 map=wa_mcs_intersection(WPA_GET_LE16(caps->vht_cap+8),WPA_GET_LE16(vht+6));
    if ((map&3)!=3) {
      config->vht=1; WPA_PUT_LE16(config->vht_mcs,map);
      config->ampdu_exp=(WPA_GET_LE32(vht+2)>>23)&7;
      /* Max MPDU describes peer RX, not a bitwise capability intersection. */
      u32 peer_flags=WPA_GET_LE32(vht+2);
      if ((peer_flags&3)==3) return -EBADMSG;
      WPA_PUT_LE32(config->vht_info,
          ((WPA_GET_LE32(caps->vht_cap)&peer_flags)&~3U)|(peer_flags&3));
    }
  }
  const u8 *he=get_ie_ext(elements,length,WLAN_EID_EXT_HE_CAPABILITIES);
  if (five && caps->he_supported && he) {
    int r=wa_he_peer(caps,he+3,he[1]-1,config);
    if (r) return r;
  }
  /* VHT peers may clear HT40 while advertising a current 20 MHz HT mode. */
  if (!config->he && !config->vht && (config->channel.width==2 ||
      (config->channel.width && !(flags&HT_CAP_INFO_SUPP_CHANNEL_WIDTH_SET))))
    return -EOPNOTSUPP;
  return 0;
}

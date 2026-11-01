#ifndef WA_HOSTAP_RTL_NDI_H
#define WA_HOSTAP_RTL_NDI_H
#include "utils/includes.h"
#include "utils/common.h"
#include "drivers/driver.h"
#include "../backend.h"

/* One native station; hostap retains NDP identities and service ownership. */
struct wa_hostap_ndi {
    struct wa_backend *backend;
    const struct wa_caps *caps;
    struct wa_nan_ndi_config config;
    u8 phy_peer[ETH_ALEN];
    bool phy_valid;
    u32 ifindex;
    bool interface_attempted, station, tk, local_gtk, peer_gtk, attempted, ready;
};
void wa_hostap_ndi_init(struct wa_hostap_ndi *, struct wa_backend *, const struct wa_caps *);
int wa_hostap_ndi_create(struct wa_hostap_ndi *);
int wa_hostap_ndi_destroy(struct wa_hostap_ndi *);
int wa_hostap_ndi_phy(struct wa_hostap_ndi *, const struct hostapd_sta_add_params *,
                      const struct wa_nan_channel *);
int wa_hostap_ndi_sta_add(struct wa_hostap_ndi *, const struct hostapd_sta_add_params *);
int wa_hostap_ndi_sta_remove(struct wa_hostap_ndi *, const u8 *);
int wa_hostap_ndi_set_key(struct wa_hostap_ndi *, const struct wpa_driver_set_key_params *);
int wa_hostap_ndi_set_flags(struct wa_hostap_ndi *, const u8 *, unsigned int, unsigned int);
int wa_hostap_ndi_get_seqnum(struct wa_hostap_ndi *, int, u8 *);
#endif

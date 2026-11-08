#ifndef WL_NAN_HOST_H
#define WL_NAN_HOST_H

#include "nan_vendor_abi.h"

struct dhd_pub;
bool wl_nan_host_device(struct dhd_pub *dhd);
bool wl_nan_host_supported(struct bcm_cfg80211 *cfg);
void wl_nan_host_ndi_init(struct bcm_cfg80211 *cfg);
void wl_nan_host_ndi_shutdown(struct bcm_cfg80211 *cfg);
void wl_nan_host_ndi_wake(struct bcm_cfg80211 *cfg);
int wl_nan_host_send_event(struct bcm_cfg80211 *cfg, u32 type, u32 reason,
                           u32 status, u8 ifidx, u8 bssidx,
                           const void *data, size_t length, gfp_t allocation);
int wl_nan_host_ndi_command(struct bcm_cfg80211 *cfg, u16 command,
                            const u8 *input, u8 *status);
bool wl_nan_host_ndi_if_event(struct bcm_cfg80211 *cfg,
                              const wl_event_data_if_t *event,
                              const char *name, const u8 *mac);
void wl_nan_host_ndi_flow_completion(struct bcm_cfg80211 *cfg, u8 ifidx,
                                    u16 flowid, u32 status, u32 operation);

int wl_nan_host_command(struct wiphy *wiphy, struct wireless_dev *wdev,
                        const void *data, int length);
int wl_nan_host_legacy_command(struct wiphy *wiphy, struct wireless_dev *wdev,
                               const void *data, int length);
s32 wl_nan_host_event(struct bcm_cfg80211 *cfg, bcm_struct_cfgdev *cfgdev,
                      const wl_event_msg_t *event, void *data);

#endif

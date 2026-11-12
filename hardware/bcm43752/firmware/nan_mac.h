#ifndef BCM43752_NAN_MAC_H
#define BCM43752_NAN_MAC_H

/* Fixed A2 context allocation, native MAC commands/beacon IE registries, WLC
 * notifications, active BSS lifecycle and host management-frame ownership.
 * Key/data transport and physical radio enable are not yet verified.
 * Detach requires the command parser to have been destroyed first.
 */
int nan_mac_context_attach(void *wlc, void *nan_context);
int nan_mac_context_detach(void *nan_context);
int nan_mac_initialize(void *wlc, void *nan_context);
int nan_mac_deinitialize(void *wlc, void *nan_context);
int nan_mac_disable_active(void *wlc, void *nan_context);
int nan_mac_enable_active(void *wlc, void *nan_context);
int nan_mac_merge_candidate(void *mac, void *bss);

/* Private tail after the native MAC extent and notification ownership word.
 * Keep both the manager and exact node until native removal releases it. */
#define NAN_MAC_BSS_MANAGER 0x118
#define NAN_MAC_BSS_NODE 0x11c
#define NAN_MAC_EVENT_BSS 0x120
#define NAN_MAC_EVENT_MANAGER 0x124
#define NAN_MAC_HOST_TX 0x128
#define NAN_MAC_HOST_TX_COUNT 0x12c
#define NAN_MAC_HOST_STOPPING 0x130
#define NAN_MAC_HOST_STATIONS 0x134
#define NAN_MAC_HOST_GROUP_KEYS 0x138
#define NAN_MAC_AWDL 0x13c
#define NAN_MAC_CONTEXT_SIZE 0x140
int nan_mac_prepare_bsscfg(void *wlc, void *mac, const unsigned char *nmi,
                           unsigned char option);
int nan_mac_remove_bsscfg(void *mac);
int nan_bss_prepare_enable(void *nan, void *bss, unsigned band, unsigned index);
int nan_bss_stop(void *nan, void *bss, unsigned index);
int nan_bss_events(void *nan, unsigned enable);

#endif

#ifndef BCM43752_NAN_PEER_H
#define BCM43752_NAN_PEER_H

/* SCB cubby callback storage belongs to WLC lifetime. Runtime peer storage
 * belongs to NAN initialize/deinitialize and may be released only when idle. */
int nan_peer_context_attach(void *wlc, void *nan);
int nan_peer_check_idle(void *nan);
int nan_peer_check_stations(void *nan, void *peer, const unsigned char *ndi);
int nan_peer_initialize(void *wlc, void *nan);
int nan_peer_deinitialize(void *wlc, void *nan);
int nan_peer_release(void *nan, void *peer);
int nan_peer_disable(void *wlc, void *nan);

#endif

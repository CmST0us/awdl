#ifndef BCM43752_NAN_DISCOVERY_H
#define BCM43752_NAN_DISCOVERY_H

/* Native A2 discovery storage and attribute builders. Hostap owns service
 * discovery policy; this does not register firmware publish/subscribe commands
 * or the B0-only Instant Mode beacon response callback. */
int nan_discovery_context_attach(void *nan);
int nan_discovery_context_detach(void *nan);
int nan_discovery_initialize(void *wlc, void *nan);
int nan_discovery_deinitialize(void *wlc, void *nan);

#endif

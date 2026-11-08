#ifndef BCM43752_NAN_INITIALIZE_H
#define BCM43752_NAN_INITIALIZE_H

/* Runtime initialization after all WLC-lifetime contexts have attached.
 * Active radio/TX ownership must be stopped before deinitialization. */
int nan_initialize(void *wlc, void *nan);
int nan_deinitialize(void *wlc, void *nan);
int nan_modules_initialize(void *wlc, void *nan);
int nan_modules_deinitialize(void *wlc, void *nan);
int nan_modules_enable(void *wlc, void *nan);
int nan_modules_disable(void *wlc, void *nan);
void nan_watchdog(void *wrapper);
int nan_common_enable(void *nan, unsigned band);
int nan_common_disable(void *nan, unsigned band);
int nan_wlc_down(void *wrapper);

#endif

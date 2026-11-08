#ifndef BCM43752_NAN_AVAILABILITY_H
#define BCM43752_NAN_AVAILABILITY_H

int nan_availability_context_attach(void *nan);
int nan_availability_context_detach(void *nan);
int nan_availability_initialize(void *wlc, void *nan);
int nan_availability_deinitialize(void *wlc, void *nan);

#endif

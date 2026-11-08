#ifndef BCM43752_NAN_SCHEDULER_H
#define BCM43752_NAN_SCHEDULER_H

int nan_scheduler_context_attach(void *wlc, void *nan);
int nan_scheduler_context_detach(void *nan);
int nan_scheduler_initialize(void *wlc, void *nan);
int nan_scheduler_deinitialize(void *wlc, void *nan);
int nan_scheduler_unregister(void *nan);
int nan_msch_unregister_owned(void *msch, void **owner);
int nan_scheduler_register(void *nan, void *const *schedules, unsigned count);
void nan_scheduler_slot_start(void *nan);
void nan_scheduler_mac_event(void *mac, unsigned event, unsigned channel);

#endif

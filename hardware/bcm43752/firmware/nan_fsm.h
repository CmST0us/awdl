#ifndef BCM43752_NAN_FSM_H
#define BCM43752_NAN_FSM_H

int nan_fsm_context_attach(void *nan);
int nan_fsm_check_idle(void *nan);
int nan_fsm_initialize(void *wlc, void *nan);
int nan_fsm_deinitialize(void *wlc, void *nan);

#endif

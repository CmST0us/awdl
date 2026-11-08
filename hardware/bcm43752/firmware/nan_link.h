#ifndef BCM43752_NAN_LINK_H
#define BCM43752_NAN_LINK_H

int nan_link_check_idle(void *nan);

/* Native control bits, shared with the pinned DHD wlioctl.h. Hostap performs
 * negotiation and allocates NDP IDs; firmware automatic replies and AP
 * association coexistence are incompatible with this single-NAN backend. */
#define NAN_CONTROL_AUTOMATIC 0x000001e0u
#define NAN_CONTROL_HOST_NDP_ID 0x00010000u
#define NAN_CONTROL_DELETE_INACTIVE 0x00020000u
#define NAN_CONTROL_INFRA_COEX 0x00040000u

int nan_link_context_attach(void *nan);
int nan_link_context_detach(void *nan);
int nan_ndl_initialize(void *wlc, void *nan);
int nan_ndl_deinitialize(void *wlc, void *nan);
int nan_ndp_initialize(void *wlc, void *nan);
int nan_ndp_deinitialize(void *wlc, void *nan);
void *nan_ndp_entry_allocate(void *ndp, const unsigned char *nmi,
                             const unsigned char *id, unsigned host_id);
int nan_ndp_entry_release(void *ndp, void *record);

#endif

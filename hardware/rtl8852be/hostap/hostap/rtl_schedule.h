#ifndef WA_HOSTAP_RTL_SCHEDULE_H
#define WA_HOSTAP_RTL_SCHEDULE_H
#include "rtl_ndi.h"
struct wa_hostap_schedule {
    struct wa_nan_channel channel;
    u32 slots, auth_slots;
};
int wa_hostap_schedule_local(const struct wa_caps *, const struct nan_schedule_config *,
                            struct wa_hostap_schedule *);
int wa_hostap_schedule_peer(const struct wa_caps *, const struct wa_hostap_schedule *,
                           const struct nan_peer_schedule_config *, struct wa_hostap_schedule *);
#endif

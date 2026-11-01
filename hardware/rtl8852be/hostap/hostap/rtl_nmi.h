#ifndef WA_HOSTAP_RTL_NMI_H
#define WA_HOSTAP_RTL_NMI_H
#include "utils/includes.h"
#include "utils/common.h"
#include "utils/wpabuf.h"
#include "drivers/driver.h"
#include "../backend.h"
#include "../igtk.h"

struct wa_hostap_nmi;
/* Events use the hostap driver event ABI. TX status is emitted on completion. */
typedef void (*wa_hostap_event_fn)(void *, enum wpa_event_type, union wpa_event_data *);
struct wa_hostap_nmi *wa_hostap_nmi_alloc(struct wa_backend *, struct wa_caps *,
                                         wa_hostap_event_fn, void *);
int wa_hostap_nmi_stop(struct wa_hostap_nmi *);
/* Free is allowed only after a successful native STOP. */
int wa_hostap_nmi_free(struct wa_hostap_nmi *);
int wa_hostap_nmi_release_other_peer(struct wa_hostap_nmi *, const u8 *, bool busy);
int wa_hostap_nmi_peer_add(struct wa_hostap_nmi *, const u8 *);
int wa_hostap_nmi_peer_remove(struct wa_hostap_nmi *, const u8 *);
int wa_hostap_nmi_set_flags(struct wa_hostap_nmi *, const u8 *, unsigned, unsigned);
int wa_hostap_nmi_set_key(struct wa_hostap_nmi *, const struct wpa_driver_set_key_params *);
int wa_hostap_nmi_get_seqnum(struct wa_hostap_nmi *, int, u8 *);
int wa_hostap_nmi_send(struct wa_hostap_nmi *, const u8 *, size_t, unsigned int);
void wa_hostap_nmi_event(struct wa_hostap_nmi *, const struct wa_event *);
#endif

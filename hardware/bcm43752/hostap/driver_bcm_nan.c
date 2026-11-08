/* SPDX-License-Identifier: BSD-3-Clause */
#include "startup.h"
#include "management.h"
#include "keys.h"
#include "stations.h"
#include "schedule.h"
#include "ndi.h"
#include "utils/eloop.h"
#include "utils/wpabuf.h"
#include "common/ieee802_11_defs.h"
#include <poll.h>

struct bcm_interface;
struct queued_notification {
	struct queued_notification *next;
	void *context;
	enum wpa_event_type type;
	union wpa_event_data event;
	u16 channel;
	size_t bytes;
	u8 payload[];
};
struct peer_schedule { bool owned, ready; u8 peer[6]; struct bcm_schedule schedule; };
struct peer_channel { u8 peer[6]; u16 channel; };
struct bcm_radio {
	struct bcm_link *link;
	struct bcm_phy phy;
	struct bcm_startup startup;
	struct bcm_management *management;
	struct bcm_keys keys;
	struct bcm_stations stations;
	struct bcm_ndi ndi;
	struct bcm_schedule local;
	struct peer_schedule peers[BCM_NAN_HOST_STATION_LIMIT];
	struct peer_channel receive_channels[BCM_NAN_HOST_STATION_LIMIT];
	unsigned next_receive_channel;
	struct bcm_interface *nmi_interface;
	struct queued_notification *first, **tail;
	unsigned references, queued, dispatching, ndi_events;
	bool nmi_created, starting, running, stopping, stop_seen, local_owned, local_ready, dw_notify;
	int failure;
	u16 received_channel, management_channel;
	u8 nmi[6], cluster[6];
};
struct bcm_interface { struct bcm_radio *radio; void *context; bool bootstrap, data; };
static struct bcm_radio *radio;
static void dispatch(void *, void *);
static int stop_radio(struct bcm_radio *);

static void fail(void *context, int error)
{
	struct bcm_radio *r = context;
	if (!r->failure) {
		r->failure = error ? error : -EIO;
		wpa_printf(MSG_ERROR, "BCM NAN: lifecycle failed (%d); unfinished owners retained", r->failure);
	}
	eloop_terminate();
}
static void discard_notifications(struct bcm_radio *r)
{
	while (r->first) {
		struct queued_notification *q = r->first;
		r->first = q->next; bin_clear_free(q, sizeof(*q) + q->bytes);
	}
	r->tail = &r->first; r->queued = 0;
	eloop_cancel_timeout(dispatch, r, NULL);
}
static void notify(void *context, enum wpa_event_type type, union wpa_event_data *event)
{
	struct bcm_radio *r = context;
	if (!r->nmi_interface || !r->nmi_interface->context) return;
	const u8 *payload = NULL;
	size_t bytes = 0;
	switch (type) {
	case EVENT_TX_STATUS: payload = event->tx_status.data; bytes = event->tx_status.data_len; break;
	case EVENT_RX_MGMT: payload = event->rx_mgmt.frame; bytes = event->rx_mgmt.frame_len; break;
	case EVENT_NAN_CLUSTER_JOIN: payload = event->nan_cluster_join_info.bssid; bytes = 6; break;
	case EVENT_NAN_NEXT_DW: break;
	default: fail(r, -EPROTO); return;
	}
	if (bytes > BCM_NAN_HOST_MAX_FRAME || (bytes && !payload) || r->queued >= 128) {
		fail(r, -ENOBUFS); return;
	}
	struct queued_notification *q = os_zalloc(sizeof(*q) + bytes);
	if (!q) { fail(r, -ENOMEM); return; }
	q->type = type; q->context = r->nmi_interface->context; q->event = *event; q->bytes = bytes;
	q->channel = r->received_channel;
	if (bytes) os_memcpy(q->payload, payload, bytes);
	if (type == EVENT_TX_STATUS) {
		if (bytes < 24) { os_free(q); fail(r, -EPROTO); return; }
		q->event.tx_status.data = q->payload; q->event.tx_status.dst = q->payload + 4;
	} else if (type == EVENT_RX_MGMT) q->event.rx_mgmt.frame = q->payload;
	else if (type == EVENT_NAN_CLUSTER_JOIN) q->event.nan_cluster_join_info.bssid = q->payload;
	*r->tail = q; r->tail = &q->next; ++r->queued;
	/* This queues an already received completion. It does not infer state
	 * from elapsed time, and prevents protocol reentry inside a driver op. */
	if (!eloop_is_timeout_registered(dispatch, r, NULL) &&
	    eloop_register_timeout(0, 0, dispatch, r, NULL)) fail(r, -ENOMEM);
}
static void dispatch(void *context, void *unused)
{
	struct bcm_radio *r = context;
	++r->dispatching;
	while (r->first && !r->failure) {
		struct queued_notification *q = r->first;
		r->first = q->next; if (!r->first) r->tail = &r->first; --r->queued;
		if (r->nmi_interface && q->context == r->nmi_interface->context) {
			/* Synchronized hostap TX uses frequency zero. Preserve the
			 * actual DW/RX channel across the deferred event delivery. */
			if (q->channel) r->management_channel = q->channel;
			if (q->type == EVENT_RX_MGMT && q->bytes >= 24 &&
			    !os_memcmp(q->payload + 4, r->nmi, 6) &&
			    bcm_unicast(q->payload + 10) &&
			    (q->channel == r->startup.discovery[0] ||
			     q->channel == r->startup.discovery[1])) {
				unsigned index;
				for (index = 0; index < ARRAY_SIZE(r->receive_channels); ++index)
					if (!os_memcmp(r->receive_channels[index].peer, q->payload + 10, 6)) break;
				if (index == ARRAY_SIZE(r->receive_channels)) {
					index = r->next_receive_channel;
					r->next_receive_channel = (index + 1) % ARRAY_SIZE(r->receive_channels);
				}
				os_memcpy(r->receive_channels[index].peer, q->payload + 10, 6);
				r->receive_channels[index].channel = q->channel;
			}
			wpa_supplicant_event(q->context, q->type, &q->event);
		}
		bin_clear_free(q, sizeof(*q) + q->bytes);
	}
	--r->dispatching;
}
static void cluster_join(struct bcm_radio *r, const struct bcm_notification *n, bool fresh)
{
	os_memcpy(r->cluster, n->cluster, 6);
	union wpa_event_data event = {0};
	event.nan_cluster_join_info.bssid = r->cluster;
	event.nan_cluster_join_info.new_cluster = fresh;
	notify(r, EVENT_NAN_CLUSTER_JOIN, &event);
}
static void event(void *context, const struct bcm_event *raw)
{
	struct bcm_radio *r = context;
	if (bcm_ndi_changed(&r->ndi, raw)) ++r->ndi_events;
	struct bcm_notification n;
	int error = bcm_event_decode(raw, &n);
	if (error) { fail(r, error); return; }
	r->received_channel = n.chanspec;
	if (n.kind == BCM_EVENT_START || n.kind == BCM_EVENT_STOP ||
	    n.kind == BCM_EVENT_CLUSTER || n.kind == BCM_EVENT_ROLE) {
		if (os_memcmp(n.nmi, r->nmi, 6)) { fail(r, -ESTALE); return; }
	}
	switch (n.kind) {
	case BCM_EVENT_START:
		if (!r->starting || r->stopping) { fail(r, -EPROTO); return; }
		r->starting = false; r->running = true; r->stop_seen = false;
		bcm_management_started(r->management); cluster_join(r, &n, true); break;
	case BCM_EVENT_STOP:
		if (!r->stopping) { fail(r, -ENETDOWN); return; }
		r->running = r->starting = false; r->stop_seen = true;
		if (bcm_management_stopped(r->management)) { fail(r, -EBUSY); return; }
		bcm_keys_stopped(&r->keys); bcm_stations_stopped(&r->stations);
		os_memset(r->peers, 0, sizeof(r->peers)); r->local_owned = r->local_ready = false;
		os_memset(r->receive_channels, 0, sizeof(r->receive_channels)); r->next_receive_channel = 0;
		break;
	case BCM_EVENT_CLUSTER:
		if (r->running && !r->stopping) cluster_join(r, &n, false);
		break;
	case BCM_EVENT_KEY_DRAINED:
		++r->keys.drain_events;
		break;
	case BCM_EVENT_TX:
		if (r->management) bcm_management_event(r->management, &n);
		break;
	case BCM_EVENT_RX:
	case BCM_EVENT_DW:
		if (r->running && !r->stopping && (n.kind != BCM_EVENT_DW || r->dw_notify))
			bcm_management_event(r->management, &n);
		break;
	default: break;
	}
}
static void receive(int fd, void *context, void *unused)
{
	struct bcm_radio *r = context;
	int error = bcm_link_receive(r->link);
	if (error < 0) fail(r, error);
}
static int ndi_error(const struct bcm_ndi *ndi)
{
	if (ndi->host_result) return ndi->host_result < 0 ? ndi->host_result : -EIO;
	return ndi->native_result ? -EIO : 0;
}
enum completion { STARTED, STOPPED, NDI_CREATED, NDI_GONE, MANAGEMENT_IDLE };
static bool completed(struct bcm_radio *r, enum completion kind, const u8 *peer)
{
	switch (kind) {
	case STARTED: return r->running;
	case STOPPED: return r->stop_seen;
	case NDI_CREATED: return bcm_ndi_created(&r->ndi);
	case NDI_GONE: return bcm_ndi_gone(&r->ndi);
	case MANAGEMENT_IDLE: return !bcm_management_busy(r->management, peer);
	}
	return false;
}
static int wait_completion(struct bcm_radio *r, enum completion kind, const u8 *peer)
{
	struct os_reltime begin, now;
	if (os_get_reltime(&begin)) return -EIO;
	unsigned changed = r->ndi_events;
	for (;;) {
		if (completed(r, kind, peer)) return 0;
		if (r->failure && !r->stopping) return r->failure;
		if (os_get_reltime(&now)) return -EIO;
		long elapsed = (now.sec - begin.sec) * 1000 + (now.usec - begin.usec) / 1000;
		if (elapsed >= 15000) return -ETIMEDOUT;
		struct pollfd fd = {.fd = bcm_link_fd(r->link), .events = POLLIN};
		int result = poll(&fd, 1, 15000 - elapsed);
		if (result < 0) { if (errno == EINTR) continue; return -errno; }
		if (!result) return -ETIMEDOUT;
		if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -EIO;
		result = bcm_link_receive(r->link);
		if (result < 0) return result;
		if ((kind == NDI_CREATED || kind == NDI_GONE) && changed != r->ndi_events) {
			changed = r->ndi_events;
			result = bcm_ndi_command(&r->ndi, r->link, BCM_NAN_NDI_QUERY);
			if (result) return result;
			result = ndi_error(&r->ndi);
			if (result && !(kind == NDI_GONE && result == -EBUSY)) return result;
		}
	}
}
static int quiesce(struct bcm_radio *r)
{
	if (!r->ndi.attempted || bcm_ndi_gone(&r->ndi)) return 0;
	int error = bcm_ndi_command(&r->ndi, r->link, BCM_NAN_NDI_QUIESCE);
	if (!error) error = ndi_error(&r->ndi);
	return error ? error : r->ndi.flags & BCM_NAN_NDI_QUIESCED ? 0 : -EBUSY;
}
static int resume(struct bcm_radio *r)
{
	if (!r->ndi.attempted || bcm_ndi_gone(&r->ndi)) return 0;
	int error = bcm_ndi_command(&r->ndi, r->link, BCM_NAN_NDI_RESUME);
	if (!error) error = ndi_error(&r->ndi);
	return error ? error : bcm_ndi_created(&r->ndi) ? 0 : -EBUSY;
}
static int drain_management(struct bcm_radio *r, const u8 *peer)
{
	int error = bcm_management_cancel(r->management, peer);
	if (error && error != -EINPROGRESS) return error;
	return wait_completion(r, MANAGEMENT_IDLE, peer);
}
static int remove_ndi(struct bcm_radio *r)
{
	if (!r->ndi.attempted || bcm_ndi_gone(&r->ndi)) return 0;
	int error = quiesce(r);
	if (!error) error = bcm_keys_remove(&r->keys, r->link, r->ndi.mac, NULL);
	for (unsigned i = 0; !error && i < ARRAY_SIZE(r->stations.entry); ++i) {
		struct bcm_station *s = &r->stations.entry[i];
		if (s->owned && s->data) error = bcm_station_remove(&r->stations, r->link, s->local, s->peer);
	}
	if (!error) error = bcm_ndi_command(&r->ndi, r->link, BCM_NAN_NDI_DELETE);
	if (!error) error = ndi_error(&r->ndi);
	if (!error) error = wait_completion(r, NDI_GONE, NULL);
	return error;
}
static int stop_radio(struct bcm_radio *r)
{
	r->stopping = true;
	discard_notifications(r);
	int error = quiesce(r);
	if (!error && r->management) error = drain_management(r, NULL);
	if (!error) error = remove_ndi(r);
	if (!error) error = bcm_keys_remove(&r->keys, r->link, NULL, NULL);
	for (unsigned i = 0; !error && i < ARRAY_SIZE(r->stations.entry); ++i) {
		struct bcm_station *s = &r->stations.entry[i];
		if (s->owned) error = bcm_station_remove(&r->stations, r->link, s->local, s->peer);
	}
	for (unsigned i = 0; !error && i < ARRAY_SIZE(r->peers); ++i) {
		struct peer_schedule *p = &r->peers[i];
		if (!p->owned) continue;
		error = bcm_result_errno(bcm_schedule_install(r->link, p->peer, NULL));
		if (!error) os_memset(p, 0, sizeof(*p));
	}
	if (!error && r->startup.owned) {
		u8 zero = 0;
		error = bcm_result_errno(bcm_set(r->link, 0x0116, &zero, 1));
		if (!error && r->running) error = wait_completion(r, STOPPED, NULL);
		/* Failed START can own partial resources without ever issuing START.
		 * Native deinit itself checks every owner; it is not a timeout. */
		if (!error) error = bcm_startup_release(&r->startup, r->link);
	}
	if (error) return error;
	r->starting = r->running = r->stopping = false;
	r->local_owned = r->local_ready = false;
	discard_notifications(r);
	return 0;
}
static void *initialize(void *context, const char *name)
{
	struct bcm_interface *i = os_zalloc(sizeof(*i));
	if (!i) return NULL;
	i->context = context; i->data = !os_strcmp(name, "ndi0");
	i->bootstrap = os_strcmp(name, "nan0") && !i->data;
	if (!radio) {
		if (!i->bootstrap) goto failure;
		radio = os_zalloc(sizeof(*radio)); if (!radio) goto failure;
		radio->tail = &radio->first;
		radio->link = bcm_link_open(name, event, radio);
		if (!radio->link || bcm_phy_read(radio->link, &radio->phy)) goto radio_failure;
		os_memcpy(radio->nmi, bcm_link_address(radio->link), 6); radio->nmi[0] |= 2;
		if (!bcm_unicast(radio->nmi)) goto radio_failure;
		radio->management = bcm_management_new(radio->link, radio->nmi, notify, fail, radio);
		if (!radio->management || eloop_register_read_sock(bcm_link_fd(radio->link), receive, radio, NULL))
			goto radio_failure;
	} else if (i->bootstrap || radio->failure) goto failure;
	if (i->data && !bcm_ndi_created(&radio->ndi)) goto failure;
	if (!i->bootstrap && !i->data && (!radio->nmi_created || radio->nmi_interface)) goto failure;
	i->radio = radio; ++radio->references;
	if (!i->bootstrap && !i->data) radio->nmi_interface = i;
	return i;
radio_failure:
	bcm_management_free(radio->management); bcm_phy_clear(&radio->phy);
	if (radio->link) bcm_link_close(radio->link);
	os_free(radio); radio = NULL;
failure:
	os_free(i); return NULL;
}
static void deinitialize(void *private)
{
	struct bcm_interface *i = private;
	struct bcm_radio *r = i->radio;
	int error = 0;
	if (r->nmi_interface == i || r->references == 1) error = stop_radio(r);
	else if (i->data) error = remove_ndi(r);
	if (error || r->dispatching) { i->context = NULL; fail(r, error ? error : -EBUSY); return; }
	if (r->nmi_interface == i) r->nmi_interface = NULL;
	--r->references; os_free(i);
	if (!r->references) {
		error = bcm_management_free(r->management);
		if (error) { fail(r, error); return; }
		eloop_unregister_read_sock(bcm_link_fd(r->link)); discard_notifications(r);
		bcm_link_close(r->link); bcm_phy_clear(&r->phy); forced_memzero(r, sizeof(*r));
		os_free(r); radio = NULL;
	}
}
static const u8 *address(void *private)
{
	struct bcm_interface *i = private;
	return i->data ? i->radio->ndi.mac : i->radio->nmi;
}
static int capability(void *private, struct wpa_driver_capa *c)
{
	struct bcm_interface *i = private;
	os_memset(c, 0, sizeof(*c));
	c->enc = WPA_DRIVER_CAPA_ENC_CCMP | WPA_DRIVER_CAPA_ENC_GCMP_256 |
		WPA_DRIVER_CAPA_ENC_BIP | WPA_DRIVER_CAPA_ENC_BIP_GMAC_256;
	c->flags = WPA_DRIVER_FLAGS_OFFCHANNEL_TX;
	if (i->bootstrap || i->data) return 0;
	int error = bcm_phy_nan_capability(&i->radio->phy, &c->nan_capa);
	if (!error) c->flags2 = WPA_DRIVER_FLAGS2_SUPPORT_NAN;
	return error;
}
static struct hostapd_hw_modes *hardware(void *private, u16 *count, u16 *flags, u8 *dfs,
                                         char *country, size_t size)
{
	struct bcm_phy *phy = &((struct bcm_interface *)private)->radio->phy;
	*count = *flags = 0; *dfs = 0;
	if (size >= 3) os_memcpy(country, phy->country, 3);
	return bcm_phy_copy(phy, count);
}
static int start(void *private, const struct nan_cluster_config *config)
{
	struct bcm_interface *i = private; struct bcm_radio *r = i->radio;
	if (i->bootstrap || i->data || r->failure || r->starting || r->running || r->stopping) return -EBUSY;
	int error = bcm_startup_prepare(&r->startup, r->link, &r->phy, r->nmi, config);
	if (error) return error;
	r->dw_notify = config->enable_dw_notif; r->starting = true; r->stop_seen = false;
	r->management_channel = r->startup.discovery[0];
	u8 one = 1;
	error = bcm_result_errno(bcm_set(r->link, 0x0116, &one, 1));
	if (!error) error = wait_completion(r, STARTED, NULL);
	if (error) fail(r, error);
	return error;
}
static void stop(void *private)
{
	struct bcm_radio *r = ((struct bcm_interface *)private)->radio;
	int error = stop_radio(r); if (error) fail(r, error);
}
static int add_interface(void *private, enum wpa_driver_if_type type, const char *name,
                          const u8 *mac, void *context, void **out, char *forced, u8 *result,
                          const char *bridge, int existing, int ap)
{
	struct bcm_radio *r = ((struct bcm_interface *)private)->radio;
	if (r->failure || existing || ap || (bridge && bridge[0])) return -EOPNOTSUPP;
	int error = 0;
	if (type == WPA_IF_NAN && !os_strcmp(name, "nan0") && !r->nmi_created) {
		if (mac && os_memcmp(mac, r->nmi, 6)) return -EINVAL;
		r->nmi_created = true; if (result) os_memcpy(result, r->nmi, 6);
	} else if (type == WPA_IF_NAN_DATA && !os_strcmp(name, "ndi0") && r->running && r->local_ready) {
		u8 ndi_mac[6]; os_memcpy(ndi_mac, mac ? mac : r->nmi, 6);
		if (!mac) ndi_mac[5] ^= 1;
		error = bcm_ndi_prepare(&r->ndi, ndi_mac, r->nmi, name);
		if (!error) error = bcm_ndi_command(&r->ndi, r->link, BCM_NAN_NDI_CREATE);
		if (!error) error = ndi_error(&r->ndi);
		if (!error) error = wait_completion(r, NDI_CREATED, NULL);
		if (!error && result) os_memcpy(result, r->ndi.mac, 6);
	} else return -EOPNOTSUPP;
	if (!error && forced) os_strlcpy(forced, name, IFNAMSIZ);
	return error;
}
static int remove_interface(void *private, enum wpa_driver_if_type type, const char *name)
{
	struct bcm_radio *r = ((struct bcm_interface *)private)->radio;
	/* Common hostap uses WPA_IF_STATION for failed iface initialization.
	 * Resolve only an already-owned exact name, never create a STA. */
	if (!os_strcmp(name, "ndi0")) return remove_ndi(r);
	if (os_strcmp(name, "nan0") || !r->nmi_created) return -ENOENT;
	int error = stop_radio(r); if (!error) r->nmi_created = false; return error;
}
static int key(void *private, struct wpa_driver_set_key_params *p)
{
	struct bcm_interface *i = private; struct bcm_radio *r = i->radio;
	if (i->bootstrap || (!r->running && p->alg != WPA_ALG_NONE)) return -ENETDOWN;
	int error = 0;
	if (i->data) error = quiesce(r);
	else if (p->alg == WPA_ALG_NONE || !(p->key_flag & KEY_FLAG_GROUP))
		error = drain_management(r, p->addr && !is_broadcast_ether_addr(p->addr) ? p->addr : NULL);
	if (!error) error = bcm_key_set(&r->keys, r->link, address(i), i->data, p);
	/* Pairwise programming and GTK removal revoke the native data port.
	 * Keep the cached authorization in agreement before a later AUTHORIZE. */
	if (i->data && (!(p->key_flag & KEY_FLAG_GROUP) || p->alg == WPA_ALG_NONE))
		for (unsigned n = 0; n < ARRAY_SIZE(r->stations.entry); ++n) {
			struct bcm_station *s = &r->stations.entry[n];
			if (s->owned && s->data && (!p->addr || is_broadcast_ether_addr(p->addr) ||
			    !os_memcmp(s->peer, p->addr, 6))) s->flags &= ~WPA_STA_AUTHORIZED;
		}
	if (!error && i->data) error = resume(r);
	if (error) fail(r, error);
	return error;
}
static int sequence(const char *name, void *private, const u8 *peer, int id, int link, u8 *pn)
{
	struct bcm_interface *i = private;
	if (i->bootstrap || link >= 0) return -EINVAL;
	return bcm_key_sequence(&i->radio->keys, i->radio->link, address(i), peer, id, pn);
}
static int station(void *private, struct hostapd_sta_add_params *p)
{
	struct bcm_interface *i = private; struct bcm_radio *r = i->radio;
	if (i->bootstrap || !r->running || r->failure) return -ENETDOWN;
	if (i->data && !bcm_ndi_created(&r->ndi)) return -EBUSY;
	if (i->data) {
		if (!p || !bcm_unicast(p->nmi_addr)) return -EINVAL;
		/* NDP completion can overlap an outstanding directed SDF. The
		 * native peer binding requires its NMI TX references to drain. */
		int error = drain_management(r, p->nmi_addr);
		if (error) return error;
	}
	return bcm_station_add(&r->stations, r->link, address(i), i->data, p);
}
static int remove_station(void *private, const u8 *peer)
{
	struct bcm_interface *i = private; struct bcm_radio *r = i->radio;
	if (i->bootstrap || !peer) return -EINVAL;
	int error = i->data ? quiesce(r) : drain_management(r, peer);
	if (!error) error = bcm_keys_remove(&r->keys, r->link, address(i), peer);
	if (!error) error = bcm_station_remove(&r->stations, r->link, address(i), peer);
	if (!error && i->data) error = resume(r);
	if (error) fail(r, error);
	return error;
}
static int flags(void *private, const u8 *peer, unsigned total, unsigned set, unsigned mask)
{
	struct bcm_interface *i = private; struct bcm_radio *r = i->radio;
	if (i->bootstrap || !r->running || r->failure) return -ENETDOWN;
	if (!i->data && !bcm_station_find(&r->stations, r->nmi, peer)) {
		struct hostapd_sta_add_params p = {.addr = peer, .flags = set};
		int error = bcm_station_add(&r->stations, r->link, r->nmi, false, &p);
		if (error) return error;
	}
	int error = 0;
	/* Deauthorization must stop host TX before native data references drain.
	 * Other peers keep their stations and resume on the same NDI afterwards. */
	bool closing = i->data && !(set & WPA_STA_AUTHORIZED) && !(mask & WPA_STA_AUTHORIZED);
	if (closing) error = quiesce(r);
	if (!error) error = bcm_station_flags(&r->stations, r->link, address(i), peer, set, mask);
	if (!error && closing) error = resume(r);
	if (error && closing) fail(r, error);
	return error;
}
static int inactivity(void *private, const u8 *peer)
{
	struct bcm_interface *i = private; struct bcm_radio *r = i->radio;
	if (i->bootstrap || !i->data || !bcm_unicast(peer)) return -EINVAL;
	if (!r->running || r->failure) return -ENETDOWN;
	return bcm_station_inactivity(&r->stations, r->link, address(i), peer);
}
static int mlme(void *private, const u8 *frame, size_t length, int noack, unsigned frequency,
                 const u16 *offsets, size_t count, int plain, unsigned wait, int link)
{
	struct bcm_interface *i = private; struct bcm_radio *r = i->radio;
	if (i->bootstrap || i->data || !r->running || r->failure || count || link >= 0) return -EOPNOTSUPP;
	if (!frequency) {
		u16 channel = r->management_channel;
		/* Pairing approval and NDP acceptance can return asynchronously.
		 * A later DW or another peer's TX completion must not redirect the
		 * response to the other discovery band. Group discovery keeps the
		 * current DW; only directed RX supplies a peer's response channel. */
		if (frame && length >= 24 && bcm_unicast(frame + 4))
			for (unsigned n = 0; n < ARRAY_SIZE(r->receive_channels); ++n)
				if (r->receive_channels[n].channel &&
				    !os_memcmp(r->receive_channels[n].peer, frame + 4, 6)) {
					channel = r->receive_channels[n].channel;
					break;
				}
		if (channel != r->management_channel)
			wpa_printf(MSG_DEBUG, "BCM NAN: directed response channel=%04x current-event=%04x",
				   channel, r->management_channel);
		frequency = bcm_chanspec_frequency(channel);
	}
	for (unsigned n = 0; n < 2; ++n)
		if (r->startup.discovery[n] && frequency == (unsigned)bcm_chanspec_frequency(r->startup.discovery[n]))
			return bcm_management_send(r->management, frame, length, r->startup.discovery[n]);
	return -EINVAL;
}
static int action(void *private, unsigned freq, unsigned wait, const u8 *dst, const u8 *src,
                   const u8 *bssid, const u8 *body, size_t length, int no_cck, int link)
{
	if (!dst || !src || !bssid || !body || length > BCM_NAN_HOST_MAX_FRAME - 24) return -EINVAL;
	u8 frame[BCM_NAN_HOST_MAX_FRAME] = {0xd0};
	os_memcpy(frame + 4, dst, 6); os_memcpy(frame + 10, src, 6); os_memcpy(frame + 16, bssid, 6);
	os_memcpy(frame + 24, body, length);
	return mlme(private, frame, length + 24, 0, freq, NULL, 0, 0, wait, link);
}
static int local_schedule(void *private, u8 map, struct nan_schedule_config *config)
{
	struct bcm_interface *i = private; struct bcm_radio *r = i->radio;
	if (i->bootstrap || i->data || map != 1) return -EINVAL;
	if (!config || !config->num_channels) return r->local_owned ? -EBUSY : 0;
	if (!r->running || r->failure) return -ENETDOWN;
	struct bcm_schedule next;
	int error = bcm_schedule_local(&r->phy, map, config, &next);
	if (error) return error;
	r->local_owned = true; r->local_ready = false;
	error = bcm_result_errno(bcm_schedule_install(r->link, NULL, &next));
	if (!error) { r->local = next; r->local_ready = true; }
	else fail(r, error);
	return error;
}
static int peer_schedule(void *private, const u8 *peer, u16 cdw, u8 sequence_id,
                          u16 switching, const struct wpabuf *ulw, struct nan_peer_schedule_config *config)
{
	struct bcm_interface *i = private; struct bcm_radio *r = i->radio;
	if (i->bootstrap || i->data || !bcm_unicast(peer) || !config) return -EINVAL;
	if (ulw && wpabuf_len(ulw)) return -EOPNOTSUPP;
	struct peer_schedule *p = NULL, *free_slot = NULL;
	for (unsigned n = 0; n < ARRAY_SIZE(r->peers); ++n) {
		if (r->peers[n].owned && !os_memcmp(r->peers[n].peer, peer, 6)) p = &r->peers[n];
		if (!r->peers[n].owned) free_slot = &r->peers[n];
	}
	bool any = false;
	if (config->n_maps > MAX_NUM_NAN_MAPS) return -EINVAL;
	for (unsigned n = 0; n < config->n_maps; ++n) any |= config->maps[n].sched.num_channels != 0;
	if (!any) {
		if (!p) return 0;
		/* An idle NDL can still have a directed discovery response queued.
		 * Complete/cancel its actual TX owner before releasing the native
		 * peer map. A failed drain must retain the map for radio cleanup. */
		int error = drain_management(r, peer);
		if (!error) error = bcm_result_errno(bcm_schedule_install(r->link, peer, NULL));
		if (!error) os_memset(p, 0, sizeof(*p));
		else fail(r, error);
		return error;
	}
	if (!r->running || !r->local_ready || r->failure) return -ENETDOWN;
	if (!p) p = free_slot;
	if (!p) return -ENOSPC;
	struct bcm_schedule next;
	int error = bcm_schedule_peer(&r->phy, &r->local, sequence_id, config, &next);
	if (error) return error;
	p->owned = true; p->ready = false; os_memcpy(p->peer, peer, 6);
	error = bcm_result_errno(bcm_schedule_install(r->link, peer, &next));
	if (!error) { p->schedule = next; p->ready = true; }
	else fail(r, error);
	return error;
}
const struct wpa_driver_ops wpa_driver_bcm_nan_ops = {
	.name = "bcm_nan", .desc = "BCM43752 A2 native NAN firmware transport",
	.init = initialize, .deinit = deinitialize, .get_mac_addr = address,
	.get_capa = capability, .get_hw_feature_data = hardware,
	.nan_start = start, .nan_stop = stop,
	.nan_config_schedule = local_schedule, .nan_config_peer_schedule = peer_schedule,
	.if_add = add_interface, .if_remove = remove_interface,
	.set_key = key, .get_seqnum = sequence, .sta_add = station, .sta_remove = remove_station,
	.sta_set_flags = flags, .send_mlme = mlme, .send_action = action,
	.get_inact_sec = inactivity,
};

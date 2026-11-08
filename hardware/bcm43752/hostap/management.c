/* SPDX-License-Identifier: BSD-3-Clause */
#include "management.h"
#include "phy.h"
#include "utils/eloop.h"
#include "common/ieee802_11_defs.h"

struct bcm_tx {
	u8 *frame;
	size_t length;
	u64 cookie;
	u16 channel;
	bool reported;
	struct os_reltime submitted;
};
struct bcm_management {
	struct bcm_link *link;
	bcm_notify_fn notify;
	bcm_failure_fn failure;
	void *context;
	u8 nmi[6];
	u64 serial;
	bool stopped, faulted;
	unsigned dispatching;
	struct bcm_tx tx[BCM_NAN_HOST_TX_LIMIT];
};
static void expired(void *, void *);
static void report(struct bcm_management *m, struct bcm_tx *tx, bool ack)
{
	if (tx->reported) return;
	tx->reported = true;
	union wpa_event_data data = {0};
	data.tx_status.type = WLAN_FC_TYPE_MGMT;
	data.tx_status.stype = (tx->frame[0] >> 4) & 15;
	data.tx_status.dst = tx->frame + 4;
	data.tx_status.data = tx->frame; data.tx_status.data_len = tx->length;
	data.tx_status.ack = ack; data.tx_status.link_id = -1;
	m->dispatching++; m->notify(m->context, EVENT_TX_STATUS, &data); m->dispatching--;
}
static void fault(struct bcm_management *m, int result)
{
	if (m->faulted) return;
	m->faulted = true;
	m->dispatching++; m->failure(m->context, result); m->dispatching--;
}
static void expired(void *context, void *unused)
{
	struct bcm_management *m = context;
	struct os_reltime now;
	(void)unused;
	if (os_get_reltime(&now)) { fault(m, -EIO); return; }
	bool pending = false, failed = false;
	for (unsigned i = 0; i < ARRAY_SIZE(m->tx); ++i) {
		struct bcm_tx *tx = &m->tx[i];
		if (!tx->frame || tx->reported) continue;
		if (os_reltime_expired(&now, &tx->submitted, 5)) {
			/* Failure may be reported, but the actual firmware cookie stays
			 * owned until TX_STATUS or a completed native STOP. */
			m->dispatching++; report(m, tx, false); m->dispatching--; failed = true;
		} else pending = true;
	}
	if (failed) fault(m, -ETIMEDOUT);
	if (pending && eloop_register_timeout(1, 0, expired, m, NULL)) fault(m, -ENOMEM);
}
struct bcm_management *bcm_management_new(struct bcm_link *link, const u8 *nmi,
	bcm_notify_fn notify, bcm_failure_fn failure, void *context)
{
	if (!link || !bcm_unicast(nmi) || !notify || !failure) return NULL;
	struct bcm_management *m = os_zalloc(sizeof(*m));
	if (!m) return NULL;
	if (os_get_random((u8 *)&m->serial, sizeof(m->serial))) { os_free(m); return NULL; }
	m->serial &= 0x7fffffffffffffffULL;
	m->link = link; m->notify = notify; m->failure = failure; m->context = context;
	m->stopped = true; os_memcpy(m->nmi, nmi, 6); return m;
}
bool bcm_management_busy(const struct bcm_management *m, const u8 *peer)
{
	for (unsigned i = 0; i < ARRAY_SIZE(m->tx); ++i)
		if (m->tx[i].frame && (!peer || !os_memcmp(m->tx[i].frame + 4, peer, 6))) return true;
	return false;
}
int bcm_management_free(struct bcm_management *m)
{
	if (!m) return 0;
	if (!m->stopped || m->dispatching || bcm_management_busy(m, NULL)) return -EBUSY;
	eloop_cancel_timeout(expired, m, NULL); forced_memzero(m, sizeof(*m)); os_free(m); return 0;
}
void bcm_management_started(struct bcm_management *m) { m->stopped = false; }
int bcm_management_stopped(struct bcm_management *m)
{
	if (m->dispatching) return -EBUSY;
	m->stopped = true; eloop_cancel_timeout(expired, m, NULL);
	m->dispatching++;
	for (unsigned i = 0; i < ARRAY_SIZE(m->tx); ++i) {
		struct bcm_tx *tx = &m->tx[i];
		if (tx->frame) { report(m, tx, false); bin_clear_free(tx->frame, tx->length); os_memset(tx, 0, sizeof(*tx)); }
	}
	m->dispatching--;
	return 0;
}
int bcm_management_send(struct bcm_management *m, const u8 *frame, size_t length, u16 channel)
{
	if (m->stopped || m->faulted) return -ENETDOWN;
	if (!frame || length < 30 || length > BCM_NAN_HOST_MAX_FRAME ||
	    frame[0] & 0x0c || frame[1] & 3 || os_memcmp(frame + 10, m->nmi, 6) ||
	    !bcm_chanspec_frequency(channel)) return -EINVAL;
	struct bcm_tx *tx = NULL;
	for (unsigned i = 0; i < ARRAY_SIZE(m->tx); ++i)
		if (!m->tx[i].frame) { tx = &m->tx[i]; break; }
	if (!tx) return -ENOBUFS;
	if (m->serial == UINT64_MAX) return -EOVERFLOW;
	u8 wire[BCM_NAN_HOST_MAX_MESSAGE];
	tx->frame = os_memdup(frame, length);
	if (!tx->frame) return -ENOMEM;
	tx->length = length; tx->channel = channel; tx->cookie = ++m->serial;
	if ((frame[0] & 0xfc) == 0xb0 && length >= 30)
		wpa_printf(MSG_DEBUG, "BCM NAN: auth TX cookie=%llu channel=%04x algorithm=%u transaction=%u status=%u",
			   (unsigned long long)tx->cookie, channel, WPA_GET_LE16(frame + 24),
			   WPA_GET_LE16(frame + 26), WPA_GET_LE16(frame + 28));
	if (os_get_reltime(&tx->submitted)) {
		bin_clear_free(tx->frame, tx->length); os_memset(tx, 0, sizeof(*tx)); return -EIO;
	}
	bcm_header(wire, BCM_NAN_HOST_TX, BCM_NAN_HOST_HEADER_SIZE + length);
	WPA_PUT_LE16(wire + 6, length); WPA_PUT_LE64(wire + 8, tx->cookie);
	WPA_PUT_LE16(wire + 16, channel); os_memcpy(wire + BCM_NAN_HOST_HEADER_SIZE, frame, length);
	/* hostap selects Protected Dual as plaintext. The firmware request bit
	 * selects the owned native pairwise/IGTK path, never host encryption. */
	u8 *out = wire + BCM_NAN_HOST_HEADER_SIZE;
	if ((out[0] & 0xfc) == 0xd0 && out[24] == WLAN_ACTION_PROTECTED_DUAL) out[1] |= 0x40;
	struct bcm_result result = bcm_set(m->link, BCM_NAN_HOST_TX_COMMAND, wire,
		BCM_NAN_HOST_HEADER_SIZE + length);
	forced_memzero(wire, sizeof(wire));
	if (!result.transport && result.native) {
		bin_clear_free(tx->frame, tx->length); os_memset(tx, 0, sizeof(*tx));
		return bcm_result_errno(result);
	}
	if (result.transport) {
		/* Caller receives this failure directly; do not issue a second
		 * failure callback if the actual completion arrives later. */
		tx->reported = true; fault(m, result.transport); return result.transport;
	}
	if (!eloop_is_timeout_registered(expired, m, NULL) &&
	    eloop_register_timeout(5, 0, expired, m, NULL)) {
		tx->reported = true; fault(m, -ENOMEM); return -ENOMEM;
	}
	return 0;
}
int bcm_management_cancel(struct bcm_management *m, const u8 *peer)
{
	int error = 0;
	for (unsigned i = 0; i < ARRAY_SIZE(m->tx); ++i) {
		struct bcm_tx *tx = &m->tx[i];
		if (!tx->frame || (peer && os_memcmp(tx->frame + 4, peer, 6))) continue;
		u8 wire[BCM_NAN_HOST_HEADER_SIZE];
		bcm_header(wire, BCM_NAN_HOST_CANCEL, sizeof(wire)); WPA_PUT_LE64(wire + 8, tx->cookie);
		struct bcm_result result = bcm_set(m->link, BCM_NAN_HOST_CANCEL_COMMAND, wire, sizeof(wire));
		/* BUSY means a submitted packet still owns its completion. NOTFOUND
		 * can race with firmware completion on the separate event socket.
		 * Neither releases our frame: only the matching TX event does that. */
		if (result.transport ||
		    (result.native && result.native != -16 && result.native != -30))
			error = bcm_result_errno(result);
	}
	return error ? error : bcm_management_busy(m, peer) ? -EINPROGRESS : 0;
}
void bcm_management_event(struct bcm_management *m, const struct bcm_notification *n)
{
	if (n->kind == BCM_EVENT_TX) {
		for (unsigned i = 0; i < ARRAY_SIZE(m->tx); ++i) {
			struct bcm_tx *tx = &m->tx[i];
			if (!tx->frame || tx->cookie != n->cookie) continue;
			if (tx->channel != n->chanspec) { fault(m, -EPROTO); return; }
			if ((tx->frame[0] & 0xfc) == 0xb0)
				wpa_printf(MSG_DEBUG, "BCM NAN: auth TX complete cookie=%llu channel=%04x result=%d ack=%u",
					   (unsigned long long)tx->cookie, n->chanspec, n->result,
					   !!(n->flags & BCM_NAN_HOST_ACK));
			m->dispatching++;
			report(m, tx, n->result >= 0 && n->flags & BCM_NAN_HOST_ACK && !(tx->frame[4] & 1));
			bin_clear_free(tx->frame, tx->length); os_memset(tx, 0, sizeof(*tx));
			m->dispatching--;
			if (!bcm_management_busy(m, NULL)) eloop_cancel_timeout(expired, m, NULL);
			return;
		}
		return; /* A completion predating this host process has no host owner. */
	}
	if (m->stopped || m->faulted) return;
	int frequency = bcm_chanspec_frequency(n->chanspec);
	if (!frequency) { fault(m, -EPROTO); return; }
	union wpa_event_data data = {0};
	m->dispatching++;
	if (n->kind == BCM_EVENT_RX) {
		if (n->length >= 30 && (n->frame[0] & 0xfc) == 0xb0)
			wpa_printf(MSG_DEBUG, "BCM NAN: auth RX channel=%04x algorithm=%u transaction=%u status=%u sequence=%u retry=%u",
				   n->chanspec, WPA_GET_LE16(n->frame + 24), WPA_GET_LE16(n->frame + 26),
				   WPA_GET_LE16(n->frame + 28), WPA_GET_LE16(n->frame + 22) >> 4,
				   !!(n->frame[1] & 8));
		data.rx_mgmt.frame = n->frame; data.rx_mgmt.frame_len = n->length;
		data.rx_mgmt.freq = frequency; data.rx_mgmt.link_id = -1;
		m->notify(m->context, EVENT_RX_MGMT, &data);
	} else if (n->kind == BCM_EVENT_DW) {
		data.nan_next_dw_info.freq = frequency;
		m->notify(m->context, EVENT_NAN_NEXT_DW, &data);
	}
	m->dispatching--;
}

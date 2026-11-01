#include "rtl_nmi.h"
#include "utils/eloop.h"
#include "common/ieee802_11_defs.h"
#include "common/nan_defs.h"
#include "../group_protection.h"

struct wa_hostap_nmi {
    struct wa_backend *backend;
    struct wa_caps *caps;
    wa_hostap_event_fn notify;
    void *context;
    bool stopped, faulted, dispatching, allocated, peer_ready, key_attempted, key_ready;
    bool bigtk_attempted, bigtk_ready;
    u8 peer[6], tk[16], bigtk[16], bigtk_index;
    u64 counter, key_cookie, tx_cookie;
    struct wa_igtk igtk;
    struct wa_bip_key local_bip, peer_bip[2];
    struct wpabuf *pending;
    size_t pending_size;
    struct { struct wpabuf *frame; u64 key; unsigned int freq; size_t size; } queue[16];
    size_t queued;
};

static void Expired(void *, void *);
static int Submit(struct wa_hostap_nmi *, bool);

struct wa_hostap_nmi *wa_hostap_nmi_alloc(struct wa_backend *backend, struct wa_caps *caps,
                                         wa_hostap_event_fn notify, void *context)
{
    struct wa_hostap_nmi *n = os_zalloc(sizeof(*n));
    if (!n) return NULL;
    n->backend = backend; n->caps = caps; n->notify = notify; n->context = context;
    if (os_get_random((u8 *)&n->counter, sizeof(n->counter))) {
        os_free(n); return NULL;
    }
    /* Keep room for monotonically allocated key and TX identities. */
    n->counter &= 0x7fffffffffffffffULL;
    return n;
}

static void ClearTX(struct wa_hostap_nmi *n)
{
    eloop_cancel_timeout(Expired, n, NULL);
    wpabuf_free(n->pending); n->pending = NULL; n->tx_cookie = 0;
    for (size_t i = 0; i < n->queued; i++) wpabuf_free(n->queue[i].frame);
    n->queued = 0;
}

int wa_hostap_nmi_stop(struct wa_hostap_nmi *n)
{
    if (n->stopped) return 0;
    n->faulted = true;
    int result = wa_backend_command(n->backend, WA_NAN_OP_STOP, 0, 0, NULL, 0);
    if (result) return result;
    ClearTX(n);
    n->stopped = true;
    n->allocated = n->peer_ready = n->key_attempted = n->key_ready = false;
    n->bigtk_attempted = n->bigtk_ready = false;
    n->key_cookie = 0;
    forced_memzero(n->tk, sizeof(n->tk)); forced_memzero(n->bigtk, sizeof(n->bigtk));
    forced_memzero(&n->local_bip, sizeof(n->local_bip));
    forced_memzero(n->peer_bip, sizeof(n->peer_bip));
    os_memset(&n->igtk, 0, sizeof(n->igtk));
    return 0;
}

int wa_hostap_nmi_free(struct wa_hostap_nmi *n)
{
    if (!n) return 0;
    if (!n->stopped || n->dispatching) return -EBUSY;
    forced_memzero(n, sizeof(*n)); os_free(n); return 0;
}

int wa_hostap_nmi_peer_add(struct wa_hostap_nmi *n, const u8 *peer)
{
    if (n->faulted || n->stopped) return -ENETDOWN;
    if (!(n->caps->features & WA_NAN_FEAT_PEER)) return -EOPNOTSUPP;
    if (!peer || is_zero_ether_addr(peer) || is_multicast_ether_addr(peer) ||
        !os_memcmp(peer, n->caps->nmi, 6)) return -EINVAL;
    if (n->allocated) {
        if (os_memcmp(peer, n->peer, 6)) return -ENOSPC;
        return n->peer_ready ? 0 : -EIO;
    }
    os_memcpy(n->peer, peer, 6); n->allocated = true;
    int result = wa_backend_peer(n->backend, WA_NAN_OP_PEER_ADD, peer);
    n->peer_ready = !result;
    return result;
}

static bool PendingPeer(struct wa_hostap_nmi *n)
{
    if (n->pending && !os_memcmp(wpabuf_head_u8(n->pending) + 4, n->peer, 6)) return true;
    for (size_t i = 0; i < n->queued; i++)
        if (!os_memcmp(wpabuf_head_u8(n->queue[i].frame) + 4, n->peer, 6)) return true;
    return false;
}

static int DeleteTK(struct wa_hostap_nmi *n)
{
    if (!n->key_attempted) return 0;
    if (PendingPeer(n)) return -EBUSY;
    n->key_ready = false;
    int result = wa_backend_pairwise_key(n->backend, WA_NAN_OP_KEY_DEL, n->peer,
                                         n->key_cookie, NULL, 0);
    if (!result) {
        n->key_attempted = false; n->key_cookie = 0;
        forced_memzero(n->tk, sizeof(n->tk));
        forced_memzero(n->peer_bip, sizeof(n->peer_bip));
    }
    return result;
}

int wa_hostap_nmi_peer_remove(struct wa_hostap_nmi *n, const u8 *peer)
{
    if (!n->allocated) return 0;
    if (!peer || os_memcmp(peer, n->peer, 6)) return -ENOENT;
    if (PendingPeer(n)) return -EBUSY;
    int result = DeleteTK(n);
    if (result) return result;
    n->peer_ready = false;
    result = wa_backend_peer(n->backend, WA_NAN_OP_PEER_DEL, peer);
    if (!result) {
        n->allocated = false; os_memset(n->peer, 0, 6);
        forced_memzero(n->peer_bip, sizeof(n->peer_bip));
    }
    return result;
}

/* Release only the old station. Local IGTK/BIGTK and discovery remain live. */
int wa_hostap_nmi_release_other_peer(struct wa_hostap_nmi *n, const u8 *peer, bool busy)
{
    if (!peer || is_zero_ether_addr(peer) || is_multicast_ether_addr(peer) ||
        !os_memcmp(peer, n->caps->nmi, 6)) return -EINVAL;
    if (!n->allocated || !os_memcmp(peer, n->peer, 6)) return 0;
    if (busy) return -EBUSY;
    int result = wa_hostap_nmi_peer_remove(n, n->peer);
    return result ? result : 1;
}

int wa_hostap_nmi_set_flags(struct wa_hostap_nmi *n, const u8 *peer, unsigned set, unsigned mask)
{
    /* Native NMI keys always enforce CCMP/BIP. MFP requests select that
     * existing path; NDI authorization remains a separate operation. */
    if (set != WPA_STA_MFP || mask != ~0U) return -EOPNOTSUPP;
    if (n->stopped || n->faulted) return -ENETDOWN;
    return wa_hostap_nmi_peer_add(n, peer);
}

int wa_hostap_nmi_set_key(struct wa_hostap_nmi *n, const struct wpa_driver_set_key_params *p)
{
    bool local = !p->addr || is_broadcast_ether_addr(p->addr);
    if (p->key_idx < 0 || p->key_idx > 7) return -EINVAL;
    if (p->alg == WPA_ALG_NONE) {
        if (p->key || p->key_len) return -EINVAL;
        if (!local) {
            if (!n->allocated) return 0;
            if (os_memcmp(p->addr, n->peer, 6)) return -ENOENT;
            if (!p->key_idx && p->key_flag == KEY_FLAG_PAIRWISE) return DeleteTK(n);
            if (p->key_idx < 4 || p->key_flag != KEY_FLAG_GROUP) return -EOPNOTSUPP;
            struct wa_bip_key *key = &n->peer_bip[p->key_idx >= 6];
            if (key->installed && key->index != p->key_idx) return -ENOENT;
            forced_memzero(key, sizeof(*key)); return 0;
        }
        if (p->key_flag != KEY_FLAG_GROUP) return -EOPNOTSUPP;
        if (p->key_idx == 4 || p->key_idx == 5) {
            if (n->igtk.attempted && n->igtk.index != (u32)p->key_idx) return -ENOENT;
            if (n->pending || n->queued) return -EBUSY;
            n->local_bip.installed = false;
            int result = wa_igtk_delete(&n->igtk, n->backend);
            if (!result) forced_memzero(&n->local_bip, sizeof(n->local_bip));
            return result;
        }
        if (p->key_idx < 6) return -EOPNOTSUPP;
        if (!n->bigtk_attempted) return 0;
        if (p->key_idx != n->bigtk_index) return -ENOENT;
        /* Native beacons own BIGTK until NAN has actually stopped. */
        return wa_hostap_nmi_stop(n);
    }
    if (n->stopped || n->faulted) return -ENETDOWN;
    if (!p->key || p->key_len != 16 || (p->seq_len && (p->seq_len != 6 || !p->seq))) return -EINVAL;
    if (!local && !p->key_idx && p->alg == WPA_ALG_CCMP && p->key_flag == KEY_FLAG_PAIRWISE_RX_TX) {
        if (!(n->caps->features & WA_NAN_FEAT_MGMT_PAIRWISE_CCMP128)) return -EOPNOTSUPP;
        if (p->seq && !is_zero_ether_addr(p->seq)) return -EOPNOTSUPP;
        int result = wa_hostap_nmi_peer_add(n, p->addr);
        if (result) return result;
        if (n->key_attempted)
            return n->key_ready && !os_memcmp(n->tk, p->key, 16) ? 0 : -EBUSY;
        n->key_cookie = ++n->counter; n->key_attempted = true;
        os_memcpy(n->tk, p->key, 16);
        result = wa_backend_pairwise_key(n->backend, WA_NAN_OP_KEY_ADD, n->peer,
                                         n->key_cookie, p->key, p->key_len);
        n->key_ready = !result;
        return result;
    }
    if (p->alg != WPA_ALG_BIP_CMAC_128 || p->key_idx < 4) return -EOPNOTSUPP;
    if (!local) {
        if (!n->key_ready || os_memcmp(p->addr, n->peer, 6)) return -ENOENT;
        if (p->key_flag != KEY_FLAG_GROUP_RX || !p->seq || p->seq_len != 6) return -EINVAL;
        return wa_bip_install(&n->peer_bip[p->key_idx >= 6], p->addr, (u8)p->key_idx, p->key, p->seq);
    }
    if (p->key_flag != KEY_FLAG_GROUP_TX_DEFAULT) return -EOPNOTSUPP;
    if (p->key_idx >= 6) {
        if (!(n->caps->features & WA_NAN_FEAT_LOCAL_BIGTK_TX)) return -EOPNOTSUPP;
        if (n->bigtk_attempted)
            return n->bigtk_ready && n->bigtk_index == p->key_idx && !os_memcmp(n->bigtk, p->key, 16) ? 0 : -EBUSY;
        const u8 zero[6] = {0};
        n->bigtk_attempted = true; n->bigtk_index = (u8)p->key_idx;
        os_memcpy(n->bigtk, p->key, 16);
        int result = wa_backend_bigtk(n->backend, (u32)p->key_idx, p->seq ? p->seq : zero, p->key);
        n->bigtk_ready = !result;
        return result;
    }
    if (p->seq && !is_zero_ether_addr(p->seq)) return -EOPNOTSUPP;
    if (n->igtk.attempted)
        return n->igtk.installed && !n->igtk.cleanup_failed && n->local_bip.installed &&
            n->local_bip.index == p->key_idx && !os_memcmp(n->local_bip.key, p->key, 16) ? 0 : -EBUSY;
    int result = wa_igtk_install(&n->igtk, n->backend, n->caps->features, (u32)p->key_idx, p->key, p->key_len);
    if (!result) wa_bip_install(&n->local_bip, n->caps->nmi, (u8)p->key_idx, p->key, p->seq);
    return result;
}

int wa_hostap_nmi_get_seqnum(struct wa_hostap_nmi *n, int index, u8 *seq)
{
    if (!seq || n->faulted || n->stopped) return -ENETDOWN;
    os_memset(seq, 0, 8);
    if (n->local_bip.installed && index == n->local_bip.index) {
        wa_bip_seq(seq, n->local_bip.pn); return 0;
    }
    if (n->bigtk_ready && index == n->bigtk_index)
        return wa_backend_group_seq(n->backend, WA_NAN_KEY_OWNER_LOCAL_NMI_BIGTK, (u32)index, NULL, seq);
    return -ENOKEY;
}

static bool Protected(const u8 *f, size_t size)
{
    return size >= 30 && (f[0] & 0xfc) == 0xd0 && f[24] == WLAN_ACTION_PROTECTED_DUAL &&
        !(f[1] & 3) && !os_memcmp(f + 25, "\x09\x50\x6f\x9a", 4) && (f[29] == 0x13 || f[29] == 0x18);
}

static int TXKey(struct wa_hostap_nmi *n, const u8 *f, size_t size, u64 *key)
{
    *key = 0;
    if (f[1] & 0x40) return -EOPNOTSUPP;
    if (size < 25 || f[24] != WLAN_ACTION_PROTECTED_DUAL || (f[0] & 0xfc) != 0xd0) return 0;
    if (!Protected(f, size) || os_memcmp(f + 4, n->peer, 6)) return -EOPNOTSUPP;
    if (!n->key_ready) return -ENOKEY;
    *key = n->key_cookie; return 0;
}

static void Report(struct wa_hostap_nmi *n, struct wpabuf *frame, size_t original_size, bool ack)
{
    const u8 *f = wpabuf_head(frame);
    union wpa_event_data event = {0};
    event.tx_status.type = WLAN_FC_TYPE_MGMT;
    event.tx_status.stype = (f[0] >> 4) & 15;
    event.tx_status.dst = f + 4; event.tx_status.data = f;
    event.tx_status.data_len = original_size; event.tx_status.ack = ack;
    event.tx_status.link_id = -1;
    bool dispatching = n->dispatching; n->dispatching = true;
    n->notify(n->context, EVENT_TX_STATUS, &event);
    n->dispatching = dispatching;
}

static void Expired(void *context, void *unused)
{
    (void)unused;
    struct wa_hostap_nmi *n = context;
    n->faulted = true;
    if (n->pending) {
        struct wpabuf *frame = n->pending; n->pending = NULL; n->tx_cookie = 0;
        Report(n, frame, n->pending_size, false); wpabuf_free(frame);
    }
    while (n->queued) {
        struct wpabuf *frame = n->queue[--n->queued].frame;
        Report(n, frame, n->queue[n->queued].size, false); wpabuf_free(frame);
    }
}

static int Submit(struct wa_hostap_nmi *n, bool reportFailure)
{
    while (!n->pending && n->queued && !n->dispatching && !n->faulted) {
        struct wpabuf *frame = n->queue[0].frame;
        u64 queued_key = n->queue[0].key, key = 0;
        unsigned int freq = n->queue[0].freq;
        size_t original_size = n->queue[0].size;
        os_memmove(n->queue, n->queue + 1, --n->queued * sizeof(*n->queue));
        int result = TXKey(n, wpabuf_head(frame), wpabuf_len(frame), &key);
        if (!result && key != queued_key) result = -ESTALE;
        if (!result) {
            n->pending = frame; n->pending_size = original_size; n->tx_cookie = ++n->counter;
            result = key ? wa_backend_protected_tx(n->backend, n->tx_cookie, freq,
                            wpabuf_head(frame), wpabuf_len(frame), n->peer, key) :
                wa_backend_command(n->backend, WA_NAN_OP_TX, n->tx_cookie, freq,
                                    wpabuf_head(frame), wpabuf_len(frame));
        }
        if (result) {
            n->pending = NULL; n->tx_cookie = 0;
            if (result == -ETIMEDOUT || result == -EBADMSG || result == -EIO)
                n->faulted = true;
            if (reportFailure) Report(n, frame, original_size, false);
            wpabuf_free(frame);
            if (n->faulted) Expired(n, NULL);
            if (!reportFailure) return result;
        } else {
            if (eloop_register_timeout(5, 0, Expired, n, NULL)) {
                n->faulted = true; return -ENOMEM;
            }
        }
    }
    return 0;
}

int wa_hostap_nmi_send(struct wa_hostap_nmi *n, const u8 *frame, size_t size, unsigned int freq)
{
    if (n->faulted || n->stopped) return -ENETDOWN;
    if (!frame || size < 24 || size > WA_NAN_FRAME_MAX || (frame[0] & 0x0c) ||
        os_memcmp(frame + 10, n->caps->nmi, 6)) return -EINVAL;
    if (n->queued == ARRAY_SIZE(n->queue)) return -ENOBUFS;
    u64 key;
    int result = TXKey(n, frame, size, &key);
    if (result) return result;
    struct wpabuf *copy = wpabuf_alloc_copy(frame, size);
    if (!copy) return -ENOMEM;
    if (is_multicast_ether_addr(frame + 4) && n->local_bip.installed) {
        result = wa_bip_sign(&n->local_bip, &copy);
        if (result || wpabuf_len(copy) > WA_NAN_FRAME_MAX) {
            wpabuf_free(copy); return result ? result : -EMSGSIZE;
        }
    }
    n->queue[n->queued].frame = copy; n->queue[n->queued].key = key;
    n->queue[n->queued].size = size;
    n->queue[n->queued++].freq = freq ? freq : n->caps->freq;
    return Submit(n, false);
}

void wa_hostap_nmi_event(struct wa_hostap_nmi *n, const struct wa_event *e)
{
    if (n->stopped) return;
    if (e->type == WA_NAN_EVENT_TX_DONE) {
        if (!n->pending || n->tx_cookie != e->cookie) return;
        struct wpabuf *frame = n->pending; n->pending = NULL; n->tx_cookie = 0;
        eloop_cancel_timeout(Expired, n, NULL);
        Report(n, frame, n->pending_size, !e->status && e->acked == WA_NAN_ACK_YES);
        wpabuf_free(frame); Submit(n, true); return;
    }
    if (n->faulted || e->type != WA_NAN_EVENT_RX || !e->frame || e->frame_len < 24 ||
        e->flags & (WA_NAN_RX_ICV_ERROR | WA_NAN_RX_CRC_ERROR)) return;
    const u8 *f = e->frame; size_t size = e->frame_len;
    if ((f[0] & 0x0c) || (n->key_attempted && !n->key_ready && !os_memcmp(f + 10, n->peer, 6))) return;
    bool verified = e->flags & WA_NAN_RX_CCMP_VERIFIED;
    bool protected = (e->flags & WA_NAN_RX_PROTECTED) || (f[1] & 0x40);
    if (verified || protected || Protected(f, size)) {
        if (!verified || !n->key_ready || !(e->flags & WA_NAN_RX_PROTECTED) || !(f[1] & 0x40) ||
            (e->flags & WA_NAN_RX_RAW) || !Protected(f, size) || e->key_index ||
            e->key_cookie != n->key_cookie || os_memcmp(e->peer, n->peer, 6) ||
            os_memcmp(f + 10, n->peer, 6) || os_memcmp(f + 4, n->caps->nmi, 6)) return;
    }
    bool keyed = n->key_ready && !os_memcmp(f + 10, n->peer, 6), group = false;
    if (keyed && is_multicast_ether_addr(f + 4)) {
        struct wa_bip_key *bip = &n->peer_bip[(f[0] & 0xfc) == 0x80];
        if (wa_bip_verify(bip, f, size)) return;
        group = true; size -= 18;
    }
    if ((f[0] & 0xfc) == 0x80) return;
    if (keyed && size >= 30 && (f[0] & 0xfc) == 0xd0 && !verified && !group &&
        f[24] == WLAN_ACTION_PUBLIC && !os_memcmp(f + 25, "\x09\x50\x6f\x9a", 4) &&
        (f[29] == 0x13 || (f[29] == 0x18 && size >= 31 &&
         f[30] >= NAN_SUBTYPE_DATA_PATH_REQUEST && f[30] <= NAN_SUBTYPE_DATA_PATH_TERMINATION))) return;
    union wpa_event_data event = {0};
    event.rx_mgmt.frame = f; event.rx_mgmt.frame_len = size;
    event.rx_mgmt.freq = (int)e->freq; event.rx_mgmt.link_id = -1;
    n->notify(n->context, EVENT_RX_MGMT, &event);
}

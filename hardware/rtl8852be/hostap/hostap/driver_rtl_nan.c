#include "rtl_nmi.h"
#include "rtl_ndi.h"
#include "rtl_schedule.h"
#include "utils/eloop.h"
#include "common/ieee802_11_defs.h"
#include "common/nan_defs.h"
#include <net/if.h>

struct rtl_interface;
struct rtl_radio {
    struct wa_backend *backend;
    struct wa_caps caps;
    struct wa_hostap_nmi *nmi;
    struct wa_hostap_ndi ndi;
    struct wa_hostap_schedule local, peer;
    struct rtl_interface *management;
    unsigned refs;
    bool started, management_created;
};
struct rtl_interface { struct rtl_radio *radio; void *context; bool data, bootstrap; };
static struct rtl_radio *radio;

static int InstallLocal(struct rtl_radio *r, const struct wa_hostap_schedule *s)
{
    if (!s->channel.primary) return 0;
    int result = s->channel.primary == 6 ? 0 :
        wa_backend_peer_schedule(r->backend, r->caps.cluster, NULL,
                                 s->slots | 2, &s->channel);
    if (!result) result = wa_backend_auth_schedule(r->backend, r->caps.cluster, s->auth_slots);
    return result;
}
static int RestoreLocal(struct rtl_radio *r) { return InstallLocal(r, &r->local); }

static void Notify(void *ctx, enum wpa_event_type type, union wpa_event_data *event)
{
    struct rtl_radio *r = ctx;
    if (r->management) wpa_supplicant_event(r->management->context, type, event);
}

static void Join(void *ctx, void *unused)
{
    struct rtl_radio *r = ctx;
    if (!r->started) return;
    union wpa_event_data event = {0};
    event.nan_cluster_join_info.bssid = r->caps.cluster;
    event.nan_cluster_join_info.new_cluster = true;
    Notify(r, EVENT_NAN_CLUSTER_JOIN, &event);
}

static void Event(void *ctx, const struct wa_event *event)
{
    struct rtl_radio *r = ctx;
    if (!r->started) return;
    if (event->type == WA_NAN_EVENT_CLUSTER) {
        os_memcpy(r->caps.cluster, event->cluster, 6);
        int result = wa_backend_auth_schedule(r->backend, r->caps.cluster,
                                                r->local.auth_slots ? r->local.auth_slots : 2);
        if (result) {
            wpa_printf(MSG_ERROR, "RTL NAN: cluster schedule failed: %d", result);
            eloop_terminate(); return;
        }
        Join(r, NULL);
    } else if (event->type == WA_NAN_EVENT_DW) {
        union wpa_event_data data = {0};
        data.nan_next_dw_info.freq = event->freq ? event->freq : 2437;
        Notify(r, EVENT_NAN_NEXT_DW, &data);
    } else if (r->nmi) wa_hostap_nmi_event(r->nmi, event);
}

static void Receive(int fd, void *ctx, void *unused)
{
    struct rtl_radio *r = ctx;
    if (wa_backend_receive(r->backend) < 0) {
        wpa_printf(MSG_ERROR, "RTL NAN: vendor event channel failed");
        eloop_terminate();
    }
}

static void *Init(void *ctx, const char *name)
{
    struct rtl_interface *i = os_zalloc(sizeof(*i));
    if (!i) return NULL;
    i->context = ctx;
    i->data = !os_strcmp(name, "ndi0");
    i->bootstrap = os_strcmp(name, "nan0") && !i->data;
    if (!radio) {
        if (!i->bootstrap) goto fail;
        radio = os_zalloc(sizeof(*radio));
        if (!radio) goto fail;
        radio->backend = wa_backend_open(name, Event, radio);
        if (!radio->backend || wa_backend_caps(radio->backend, &radio->caps) ||
            !(radio->caps.features & WA_NAN_FEAT_LOCAL_DATA_SCHEDULE)) goto radio_fail;
        wa_hostap_ndi_init(&radio->ndi, radio->backend, &radio->caps);
        if (eloop_register_read_sock(wa_backend_fd(radio->backend), Receive, radio, NULL)) goto radio_fail;
    } else if (i->bootstrap) goto fail;
    if (i->data && !radio->ndi.ifindex) goto fail;
    if (!i->bootstrap && !i->data && (!radio->management_created || radio->management)) goto fail;
    i->radio = radio; radio->refs++;
    if (!i->bootstrap && !i->data) radio->management = i;
    return i;
radio_fail:
    if (radio->backend) wa_backend_close(radio->backend);
    os_free(radio); radio = NULL;
fail:
    os_free(i); return NULL;
}

static int StopRadio(struct rtl_radio *r)
{
    eloop_cancel_timeout(Join, r, NULL);
    int result = wa_hostap_ndi_sta_remove(&r->ndi, r->ndi.config.peer);
    if (!result && r->nmi) result = wa_hostap_nmi_stop(r->nmi);
    if (result) {
        wpa_printf(MSG_ERROR, "RTL NAN: native cleanup incomplete: %d", result);
        return result;
    }
    r->started = false;
    os_memset(&r->local, 0, sizeof(r->local));
    os_memset(&r->peer, 0, sizeof(r->peer));
    if (r->nmi) { result = wa_hostap_nmi_free(r->nmi); if (result) return result; r->nmi = NULL; }
    return 0;
}

static void Deinit(void *priv)
{
    struct rtl_interface *i = priv;
    struct rtl_radio *r = i->radio;
    if (r->management == i) {
        if (StopRadio(r)) { eloop_terminate(); return; }
        r->management = NULL;
    }
    if (--r->refs == 0) {
        if (StopRadio(r) || wa_hostap_ndi_destroy(&r->ndi)) {
            wpa_printf(MSG_ERROR, "RTL NAN: retained radio after failed cleanup");
            r->refs++; eloop_terminate(); return;
        }
        eloop_unregister_read_sock(wa_backend_fd(r->backend));
        wa_backend_close(r->backend); os_free(r); radio = NULL;
    }
    os_free(i);
}

static const u8 *Address(void *priv) { return ((struct rtl_interface *)priv)->radio->caps.nmi; }

static int Capability(void *priv, struct wpa_driver_capa *c)
{
    struct rtl_interface *i = priv;
    const struct wa_caps *s = &i->radio->caps;
    os_memset(c, 0, sizeof(*c));
    c->enc = WPA_DRIVER_CAPA_ENC_CCMP | WPA_DRIVER_CAPA_ENC_BIP;
    c->flags = WPA_DRIVER_FLAGS_OFFCHANNEL_TX;
    if (i->bootstrap || i->data) return 0;
    c->flags2 = WPA_DRIVER_FLAGS2_SUPPORT_NAN;
    struct nan_capa *n = &c->nan_capa;
    n->drv_flags = WPA_DRIVER_FLAGS_NAN_SUPPORT_SYNC_CONFIG |
        WPA_DRIVER_FLAGS_NAN_SUPPORT_USERSPACE_DE | WPA_DRIVER_FLAGS_NAN_SUPPORT_NDP |
        WPA_DRIVER_FLAGS_NAN_SUPPORT_BEACON_PROT;
    n->num_radios = 1; n->sched_chans = 2; n->slot_duration = 16; n->schedule_period = 512;
    n->num_antennas = 0x22; n->max_channel_switch_time = 16;
    n->dev_capa = NAN_DEV_CAPA_NDPE_ATTR_SUPP;
    n->ht_capab = WPA_GET_LE16(s->ht_cap); n->ht_ampdu_params = s->ht_cap[2];
    os_memcpy(n->ht_mcs_set, s->ht_cap + 3, 16);
    n->vht_valid = s->vht_supported;
    n->vht_capab = WPA_GET_LE32(s->vht_cap);
    os_memcpy(n->vht_mcs_set, s->vht_cap + 4, 8);
    if (s->vht_supported) n->op_modes |= NAN_DEV_CAPA_OP_MODE_PHY_MODE_VHT;
    if (s->he_supported) {
        n->op_modes |= NAN_DEV_CAPA_OP_MODE_PHY_MODE_HE;
        n->he_valid = true; n->he_capab.he_supported = true;
        os_memcpy(n->he_capab.mac_cap, s->he_cap, 6);
        os_memcpy(n->he_capab.phy_cap, s->he_cap + 6, 11);
        os_memcpy(n->he_capab.mcs, s->he_cap + 17, 4);
    }
    return 0;
}

static struct hostapd_hw_modes *Hardware(void *priv, u16 *count, u16 *flags,
                                       u8 *dfs, char *country, size_t country_size)
{
    const struct wa_caps *s = &((struct rtl_interface *)priv)->radio->caps;
    struct hostapd_hw_modes *m = os_calloc(2, sizeof(*m));
    if (!m) return NULL;
    *count = 2; *flags = 0; *dfs = 0;
    if (country_size >= 3) os_memcpy(country, "00", 3);
    for (unsigned b = 0; b < 2; b++) {
        m[b].mode = b ? HOSTAPD_MODE_IEEE80211A : HOSTAPD_MODE_IEEE80211G;
        m[b].channels = os_calloc(s->n_channels + 1, sizeof(*m[b].channels));
        m[b].rates = os_calloc(8, sizeof(int));
        if (!m[b].channels || !m[b].rates) {
            for (unsigned j = 0; j <= b; j++) { os_free(m[j].channels); os_free(m[j].rates); }
            os_free(m); return NULL;
        }
        int rates[] = {60, 90, 120, 180, 240, 360, 480, 540};
        os_memcpy(m[b].rates, rates, sizeof(rates)); m[b].num_rates = 8;
        const u8 *ht = b ? s->ht_cap_5g : s->ht_cap;
        m[b].ht_capab = WPA_GET_LE16(ht); m[b].a_mpdu_params = ht[2];
        os_memcpy(m[b].mcs_set, ht + 3, 16);
        if (b) { m[b].vht_capab = WPA_GET_LE32(s->vht_cap); os_memcpy(m[b].vht_mcs_set, s->vht_cap + 4, 8); }
        if (!b && s->ht_supported) {
            m[b].channels[0] = (struct hostapd_channel_data){.chan=6,.freq=2437,.allowed_bw=HOSTAPD_CHAN_WIDTH_20};
            m[b].num_channels = 1;
        }
        for (size_t j = 0; j < s->n_channels; j++) {
            const struct wa_nan_channel *ch = &s->channels[j];
            if ((unsigned)(ch->primary > 14) != b || ch->width) continue;
            struct hostapd_channel_data *c = &m[b].channels[m[b].num_channels++];
            c->chan = ch->primary; c->freq = (b ? 5000 : 2407) + 5 * c->chan;
            c->allowed_bw = HOSTAPD_CHAN_WIDTH_20;
            for (size_t k = 0; k < s->n_channels; k++) if (s->channels[k].primary == c->chan) {
                if (s->channels[k].width == 2) {
                    c->allowed_bw |= HOSTAPD_CHAN_WIDTH_80;
                    c->flag |= HOSTAPD_CHAN_VHT_80MHZ_SUBCHANNEL;
                }
                if (s->channels[k].width == 1) {
                    bool above = s->channels[k].center > c->chan;
                    c->allowed_bw |= above ? HOSTAPD_CHAN_WIDTH_40P : HOSTAPD_CHAN_WIDTH_40M;
                    c->flag |= above ? HOSTAPD_CHAN_HT40PLUS : HOSTAPD_CHAN_HT40MINUS;
                }
            }
        }
    }
    return m;
}

static int Start(void *priv, const struct nan_cluster_config *config)
{
    struct rtl_interface *i = priv;
    struct rtl_radio *r = i->radio;
    if (i->bootstrap || i->data || r->started || config->dual_band ||
        config->master_pref != 200 || config->discovery_beacon_interval != 100) return -EOPNOTSUPP;
    u8 frame[30] = {0xd0};
    const u8 destination[6] = {0x51,0x6f,0x9a,1,0,0};
    os_memcpy(frame + 4, destination, 6); os_memcpy(frame + 10, r->caps.nmi, 6);
    os_memcpy(frame + 16, "\x50\x6f\x9a\x01", 4);
    if (!is_zero_ether_addr(config->cluster_id)) {
        if (os_memcmp(config->cluster_id, frame + 16, 4)) return -EOPNOTSUPP;
        os_memcpy(frame + 16, config->cluster_id, 6);
    } else if (os_get_random(frame + 20, 2)) return -EIO;
    os_memcpy(frame + 24, "\x04\x09\x50\x6f\x9a\x13", 6);
    r->nmi = wa_hostap_nmi_alloc(r->backend, &r->caps, Notify, r);
    if (!r->nmi) return -ENOMEM;
    int result = wa_backend_command(r->backend, WA_NAN_OP_START, 0, 2437, frame, sizeof(frame));
    if (!result) result = wa_backend_caps(r->backend, &r->caps);
    if (!result) result = wa_backend_local_schedule(r->backend, r->caps.cluster);
    if (result) { StopRadio(r); return result; }
    r->started = true;
    /* Defer notification until hostap has committed its successful START. */
    eloop_register_timeout(0, 0, Join, r, NULL);
    return 0;
}

static void Stop(void *priv) { if (StopRadio(((struct rtl_interface *)priv)->radio)) eloop_terminate(); }
static int Change(void *priv, const struct nan_cluster_config *config) { return -EOPNOTSUPP; }
static int AddInterface(void *priv, enum wpa_driver_if_type type, const char *name, const u8 *addr,
                        void *ctx, void **out, char *forced, u8 *mac, const char *bridge, int existing, int ap)
{
    struct rtl_radio *r = ((struct rtl_interface *)priv)->radio;
    if (addr && os_memcmp(addr, r->caps.nmi, 6)) return -EOPNOTSUPP;
    int result;
    if (type == WPA_IF_NAN && !os_strcmp(name, "nan0") && !r->management_created) {
        r->management_created = true; result = 0;
    } else if (type == WPA_IF_NAN_DATA && !os_strcmp(name, "ndi0")) result = wa_hostap_ndi_create(&r->ndi);
    else return -EOPNOTSUPP;
    if (!result && mac) os_memcpy(mac, r->caps.nmi, 6);
    if (!result && forced) os_strlcpy(forced, name, IFNAMSIZ);
    return result;
}
static int RemoveInterface(void *priv, enum wpa_driver_if_type type, const char *name)
{
    struct rtl_radio *r = ((struct rtl_interface *)priv)->radio;
    if (!os_strcmp(name, "ndi0")) return wa_hostap_ndi_destroy(&r->ndi);
    if (!os_strcmp(name, "nan0")) { int result = StopRadio(r); if (!result) r->management_created = false; return result; }
    return -EOPNOTSUPP;
}
static int Key(void *priv, struct wpa_driver_set_key_params *p)
{
    struct rtl_interface *i = priv;
    return i->data ? wa_hostap_ndi_set_key(&i->radio->ndi, p) :
        i->radio->nmi ? wa_hostap_nmi_set_key(i->radio->nmi, p) : -ENETDOWN;
}
static int Seq(const char *name, void *priv, const u8 *addr, int index, int link, u8 *seq)
{
    struct rtl_interface *i = priv;
    return i->data ? wa_hostap_ndi_get_seqnum(&i->radio->ndi, index, seq) :
        i->radio->nmi ? wa_hostap_nmi_get_seqnum(i->radio->nmi, index, seq) : -ENETDOWN;
}
static int Station(void *priv, struct hostapd_sta_add_params *p)
{
    struct rtl_interface *i = priv; struct rtl_radio *r = i->radio;
    if (i->data) return wa_hostap_ndi_sta_add(&r->ndi, p);
    if (!r->nmi) return -ENETDOWN;
    int result = wa_hostap_nmi_peer_add(r->nmi, p->addr);
    if (!result && p->ht_capabilities) result = wa_hostap_ndi_phy(&r->ndi, p, &r->peer.channel);
    return result;
}
static int RemoveStation(void *priv, const u8 *addr)
{
    struct rtl_interface *i = priv;
    struct rtl_radio *r = i->radio;
    if (i->data) return wa_hostap_ndi_sta_remove(&r->ndi, addr);
    int result = r->nmi ? wa_hostap_nmi_peer_remove(r->nmi, addr) : 0;
    if (!result) {
        os_memset(&r->peer, 0, sizeof(r->peer));
        if (r->started) result = RestoreLocal(r);
        if (result) {
            wpa_printf(MSG_ERROR, "RTL NAN: local schedule restore failed: %d", result);
            eloop_terminate();
        }
    }
    return result;
}
static int Flags(void *priv, const u8 *addr, unsigned total, unsigned set, unsigned mask)
{
    struct rtl_interface *i = priv;
    struct rtl_radio *r = i->radio;
    if (i->data) return wa_hostap_ndi_set_flags(&r->ndi, addr, set, mask);
    if (!r->nmi) return -ENETDOWN;
    if (set != WPA_STA_MFP || mask != ~0U) return -EOPNOTSUPP;
    int result = wa_hostap_nmi_release_other_peer(r->nmi, addr,
                                                r->ndi.attempted || r->ndi.station);
    if (result < 0) {
        wpa_printf(MSG_INFO, "RTL NAN: peer transition rejected: %d", result);
        return result;
    }
    if (result) {
        os_memset(&r->peer, 0, sizeof(r->peer));
        r->ndi.phy_valid = false;
        result = RestoreLocal(r);
        if (result) return result;
        wpa_printf(MSG_INFO, "RTL NAN: idle peer released; discovery retained");
    }
    return wa_hostap_nmi_set_flags(r->nmi, addr, set, mask);
}
static int MLME(void *priv, const u8 *frame, size_t length, int noack, unsigned freq,
                const u16 *offsets, size_t count, int plain, unsigned wait, int link)
{
    struct rtl_interface *i = priv;
    /* Native NMI frames use the management MACID's channel-6 reservation;
     * the peer data schedule belongs to NDI and does not move this queue. */
    if (!freq) freq = 2437;
    return i->radio->nmi ? wa_hostap_nmi_send(i->radio->nmi, frame, length, freq) : -ENETDOWN;
}
static int Action(void *priv, unsigned freq, unsigned wait, const u8 *dst, const u8 *src,
                  const u8 *bssid, const u8 *body, size_t length, int no_cck, int link)
{
    if (length > WA_NAN_FRAME_MAX - 24) return -EMSGSIZE;
    u8 frame[WA_NAN_FRAME_MAX] = {0xd0};
    os_memcpy(frame + 4, dst, 6); os_memcpy(frame + 10, src, 6); os_memcpy(frame + 16, bssid, 6);
    os_memcpy(frame + 24, body, length);
    return MLME(priv, frame, length + 24, 0, freq, NULL, 0, 0, wait, link);
}
static int LocalSchedule(void *priv, u8 map, struct nan_schedule_config *config)
{
    struct rtl_radio *r = ((struct rtl_interface *)priv)->radio;
    if (!r->started || map != 1) return -EINVAL;
    struct wa_hostap_schedule next;
    int result = wa_hostap_schedule_local(&r->caps, config, &next);
    if (!result) result = InstallLocal(r, &next);
    if (!result) r->local = next;
    return result;
}
static int PeerSchedule(void *priv, const u8 *peer, u16 cdw, u8 sequence, u16 switching,
                        const struct wpabuf *ulw, struct nan_peer_schedule_config *config)
{
    struct rtl_radio *r = ((struct rtl_interface *)priv)->radio;
    if (!r->started || (ulw && wpabuf_len(ulw))) return -EOPNOTSUPP;
    if (!config->n_maps) {
        int result = RestoreLocal(r);
        if (!result) os_memset(&r->peer, 0, sizeof(r->peer));
        return result;
    }
    struct wa_hostap_schedule next;
    int result = wa_hostap_schedule_peer(&r->caps, &r->local, config, &next);
    if (!result) result = wa_backend_peer_schedule(r->backend, r->caps.cluster, peer, next.slots, &next.channel);
    if (!result) result = wa_backend_auth_schedule(r->backend, r->caps.cluster, r->local.auth_slots);
    if (!result) r->peer = next;
    return result;
}

const struct wpa_driver_ops wpa_driver_rtl_nan_ops = {
    .name = "rtl_nan", .desc = "RTL native NAN vendor interface",
    .init = Init, .deinit = Deinit, .get_mac_addr = Address,
    .get_capa = Capability, .get_hw_feature_data = Hardware,
    .nan_start = Start, .nan_stop = Stop, .nan_change_config = Change,
    .nan_config_schedule = LocalSchedule, .nan_config_peer_schedule = PeerSchedule,
    .if_add = AddInterface, .if_remove = RemoveInterface,
    .set_key = Key, .get_seqnum = Seq, .sta_add = Station, .sta_remove = RemoveStation,
    .sta_set_flags = Flags, .send_mlme = MLME, .send_action = Action,
};

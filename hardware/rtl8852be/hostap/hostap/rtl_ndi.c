#include "rtl_ndi.h"
#include "../phy.h"
#include "common/ieee802_11_defs.h"

void wa_hostap_ndi_init(struct wa_hostap_ndi *n, struct wa_backend *backend,
                       const struct wa_caps *caps)
{
    os_memset(n, 0, sizeof(*n));
    n->backend = backend;
    n->caps = caps;
    n->config.peer_gtk_index = 0xff;
}

int wa_hostap_ndi_create(struct wa_hostap_ndi *n)
{
    const u32 needed = WA_NAN_FEAT_NDI | WA_NAN_FEAT_PERSISTENT_NDI | WA_NAN_FEAT_NDI_STA;
    if ((n->caps->features & needed) != needed)
        return -EOPNOTSUPP;
    if (n->interface_attempted)
        return -EEXIST;
    n->interface_attempted = true;
    int result = wa_backend_ndi(n->backend, WA_NAN_OP_NDI_IF_ADD, NULL, &n->ifindex);
    if (result == -EEXIST)
        n->interface_attempted = false;
    return result;
}

int wa_hostap_ndi_destroy(struct wa_hostap_ndi *n)
{
    if (n->station || n->attempted)
        return -EBUSY;
    if (!n->interface_attempted)
        return 0;
    int result = wa_backend_ndi(n->backend, WA_NAN_OP_NDI_IF_DEL, NULL, NULL);
    if (!result) {
        struct wa_backend *backend = n->backend;
        const struct wa_caps *caps = n->caps;
        forced_memzero(n, sizeof(*n));
        wa_hostap_ndi_init(n, backend, caps);
    }
    return result;
}

static void ClearKeys(struct wa_hostap_ndi *n)
{
    forced_memzero(n->config.tk, sizeof(n->config.tk));
    forced_memzero(n->config.gtk, sizeof(n->config.gtk));
    forced_memzero(n->config.peer_gtk, sizeof(n->config.peer_gtk));
    os_memset(n->config.peer_pn, 0, sizeof(n->config.peer_pn));
    n->config.gtk_index = 0;
    n->config.peer_gtk_index = 0xff;
    n->tk = n->local_gtk = n->peer_gtk = false;
}

static int Release(struct wa_hostap_ndi *n)
{
    if (n->attempted) {
        n->ready = false;
        int result = wa_backend_ndi(n->backend, WA_NAN_OP_NDI_DEL, NULL, NULL);
        if (result)
            return result;
    }
    n->attempted = n->ready = false;
    ClearKeys(n);
    return 0;
}

int wa_hostap_ndi_phy(struct wa_hostap_ndi *n,
                     const struct hostapd_sta_add_params *params,
                     const struct wa_nan_channel *channel)
{
    if (!params->addr || is_zero_ether_addr(params->addr) ||
        is_multicast_ether_addr(params->addr) || !params->ht_capabilities || !channel)
        return -EINVAL;
    if (n->attempted || (n->station && os_memcmp(n->config.peer_nmi, params->addr, ETH_ALEN)))
        return -EBUSY;
    bool supported = n->caps->ht_supported && channel->primary == 6 &&
                     channel->center == 6 && !channel->width;
    for (size_t i = 0; i < n->caps->n_channels; i++)
        supported |= !os_memcmp(channel, &n->caps->channels[i], sizeof(*channel));
    if (!supported)
        return -EOPNOTSUPP;
    u8 elements[256], *cursor = elements;
    *cursor++ = WLAN_EID_HT_CAP; *cursor++ = WA_NAN_HT_CAP_SIZE;
    os_memcpy(cursor, params->ht_capabilities, WA_NAN_HT_CAP_SIZE); cursor += WA_NAN_HT_CAP_SIZE;
    if (params->vht_capabilities) {
        *cursor++ = WLAN_EID_VHT_CAP; *cursor++ = WA_NAN_VHT_CAP_SIZE;
        os_memcpy(cursor, params->vht_capabilities, WA_NAN_VHT_CAP_SIZE); cursor += WA_NAN_VHT_CAP_SIZE;
    }
    if (params->he_capab) {
        if (params->he_capab_len < WA_NAN_HE_CAP_SIZE ||
            params->he_capab_len > sizeof(elements) - (size_t)(cursor - elements) - 3)
            return -EINVAL;
        *cursor++ = WLAN_EID_EXTENSION; *cursor++ = (u8)(params->he_capab_len + 1);
        *cursor++ = WLAN_EID_EXT_HE_CAPABILITIES;
        os_memcpy(cursor, params->he_capab, params->he_capab_len); cursor += params->he_capab_len;
    }
    struct wa_nan_ndi_config config = n->config;
    config.channel = *channel;
    int result = wa_ndp_phy(n->caps, elements, (size_t)(cursor - elements), &config);
    if (!result) {
        n->config = config;
        os_memcpy(n->phy_peer, params->addr, ETH_ALEN);
        n->phy_valid = true;
    }
    forced_memzero(&config, sizeof(config));
    return result;
}

int wa_hostap_ndi_sta_add(struct wa_hostap_ndi *n, const struct hostapd_sta_add_params *params)
{
    if (!n->ifindex)
        return -ENODEV;
    if (!params->addr || !params->nmi_addr || is_multicast_ether_addr(params->addr) ||
        is_zero_ether_addr(params->addr) || !os_memcmp(params->addr, n->caps->nmi, ETH_ALEN))
        return -EINVAL;
    if (n->station)
        return -EEXIST;
    if (!n->phy_valid || os_memcmp(params->nmi_addr, n->phy_peer, ETH_ALEN))
        return -ENODATA;
    if (params->set || (params->flags & (WPA_STA_AUTHENTICATED | WPA_STA_ASSOCIATED | WPA_STA_MFP)) !=
        (WPA_STA_AUTHENTICATED | WPA_STA_ASSOCIATED | WPA_STA_MFP))
        return -EOPNOTSUPP;
    os_memcpy(n->config.local, n->caps->nmi, ETH_ALEN);
    os_memcpy(n->config.peer_nmi, params->nmi_addr, ETH_ALEN);
    os_memcpy(n->config.peer, params->addr, ETH_ALEN);
    n->station = true;
    return 0;
}

int wa_hostap_ndi_sta_remove(struct wa_hostap_ndi *n, const u8 *addr)
{
    if (!n->station)
        return 0;
    if (!addr || os_memcmp(addr, n->config.peer, ETH_ALEN))
        return -ENOENT;
    int result = Release(n);
    if (!result) {
        n->station = false;
        forced_memzero(&n->config, sizeof(n->config));
        n->config.peer_gtk_index = 0xff;
        n->phy_valid = false;
        os_memset(n->phy_peer, 0, ETH_ALEN);
    }
    return result;
}

int wa_hostap_ndi_set_key(struct wa_hostap_ndi *n, const struct wpa_driver_set_key_params *p)
{
    bool local = !p->addr || is_broadcast_ether_addr(p->addr);
    if (!n->station)
        return p->alg == WPA_ALG_NONE ? 0 : -ENOENT;
    if (!local && os_memcmp(p->addr, n->config.peer, ETH_ALEN))
        return -ENOENT;
    if (p->alg == WPA_ALG_NONE) {
        if (p->key || p->key_len || p->key_idx < 0 || p->key_idx > 3 ||
            !((p->key_flag == KEY_FLAG_PAIRWISE && !local && !p->key_idx) ||
              p->key_flag == KEY_FLAG_GROUP))
            return -EINVAL;
        /* Revocation closes the only data station before removing any key. */
        return Release(n);
    }
    if (p->alg != WPA_ALG_CCMP || !p->key || p->key_len != 16 ||
        (p->seq_len && (p->seq_len != 6 || !p->seq)))
        return -EOPNOTSUPP;
    u8 *key = NULL;
    bool *present = NULL;
    if (local && p->key_flag == KEY_FLAG_GROUP_TX_DEFAULT && p->key_idx >= 0 && p->key_idx <= 3) {
        key = n->config.gtk; present = &n->local_gtk;
    } else if (!local && p->key_flag == KEY_FLAG_PAIRWISE && p->key_idx == 0) {
        key = n->config.tk; present = &n->tk;
    } else if (!local && p->key_flag == KEY_FLAG_GROUP_RX && p->key_idx >= 0 && p->key_idx <= 3) {
        key = n->config.peer_gtk; present = &n->peer_gtk;
    } else {
        return -EOPNOTSUPP;
    }
    if (p->seq && present != &n->peer_gtk && !is_zero_ether_addr(p->seq))
        return -EOPNOTSUPP;
    if (*present || n->attempted) {
        bool same = *present && !os_memcmp(key, p->key, 16);
        if (present == &n->local_gtk) same &= n->config.gtk_index == p->key_idx;
        if (present == &n->peer_gtk)
            same &= n->config.peer_gtk_index == p->key_idx &&
                    (!p->seq || !os_memcmp(n->config.peer_pn, p->seq, 6));
        /* An identical install never resets a live PN. */
        return same && (!n->attempted || n->ready) ? 0 : -EBUSY;
    }
    os_memcpy(key, p->key, 16); *present = true;
    if (present == &n->local_gtk) n->config.gtk_index = (u8)p->key_idx;
    if (present == &n->peer_gtk) {
        n->config.peer_gtk_index = (u8)p->key_idx;
        if (p->seq) os_memcpy(n->config.peer_pn, p->seq, 6);
    }
    return 0;
}

int wa_hostap_ndi_set_flags(struct wa_hostap_ndi *n, const u8 *addr,
                          unsigned int flags_or, unsigned int flags_and)
{
    if (!n->station || !addr || os_memcmp(addr, n->config.peer, ETH_ALEN))
        return -ENOENT;
    if (!(flags_or & WPA_STA_AUTHORIZED))
        return flags_and & WPA_STA_AUTHORIZED ? 0 : Release(n);
    if (n->ready)
        return 0;
    if (n->attempted)
        return -EBUSY;
    if (!n->tk || !n->local_gtk || !n->phy_valid)
        return -ENOKEY;
    n->attempted = true;
    u32 index = 0;
    int result = wa_backend_ndi(n->backend, WA_NAN_OP_NDI_STA_ADD, &n->config, &index);
    if (!result && index != n->ifindex) result = -EIO;
    if (!result) result = wa_backend_ndi(n->backend, WA_NAN_OP_NDI_CONNECT, &n->config, &index);
    if (!result && index != n->ifindex) result = -EIO;
    if (result) {
        int released = Release(n);
        return released ? released : result;
    }
    n->ready = true;
    return 0;
}

int wa_hostap_ndi_get_seqnum(struct wa_hostap_ndi *n, int index, u8 *seq)
{
    if (!seq || !n->local_gtk || index != n->config.gtk_index)
        return -ENOKEY;
    if (n->attempted && !n->ready)
        return -EBUSY;
    os_memset(seq, 0, 8);
    if (!n->ready)
        return 0;
    return wa_backend_group_seq(n->backend, WA_NAN_KEY_OWNER_LOCAL_NDI_GTK,
                                (u32)index, n->config.local, seq);
}

/* SPDX-License-Identifier: BSD-3-Clause */
#include "keys.h"
#include <poll.h>

static bool matching(const struct bcm_key *key, const u8 *local, const u8 *peer)
{
	return key->owned && (!local || !os_memcmp(key->local, local, 6)) &&
		(!peer || !os_memcmp(key->peer, peer, 6));
}
static struct bcm_key *find(struct bcm_keys *keys, u16 command, const u8 *local,
                            const u8 *peer, u8 id)
{
	for (unsigned i = 0; i < ARRAY_SIZE(keys->entry); ++i) {
		struct bcm_key *key = &keys->entry[i];
		if (matching(key, local, peer) && key->command == command && key->id == id) return key;
	}
	return NULL;
}
static int remove_key(struct bcm_keys *keys, struct bcm_link *link, struct bcm_key *key)
{
	u8 wire[BCM_NAN_HOST_DATA_KEY_SIZE];
	bcm_header(wire, BCM_NAN_HOST_REMOVE, key->length);
	os_memcpy(wire + 8, key->peer, 6); wire[14] = key->id;
	if (key->command == BCM_NAN_HOST_GROUP_KEY_COMMAND) {
		os_memcpy(wire + 24, key->local, 6); wire[30] = key->request[30];
	} else if (key->command == BCM_NAN_HOST_DATA_KEY_COMMAND)
		os_memcpy(wire + 64, key->local, 6);
	key->ready = false;
	unsigned changed = keys->drain_events;
	struct bcm_result result = bcm_set(link, key->command, wire, key->length);
	if (key->command == BCM_NAN_HOST_GROUP_KEY_COMMAND &&
	    !result.transport && result.native == -16) {
		struct os_reltime begin, now;
		if (os_get_reltime(&begin)) result.transport = -EIO;
		while (!result.transport && result.native == -16) {
			if (changed != keys->drain_events) {
				changed = keys->drain_events;
				result = bcm_set(link, key->command, wire, key->length);
				continue;
			}
			if (os_get_reltime(&now)) { result.transport = -EIO; break; }
			long elapsed = (now.sec - begin.sec) * 1000 + (now.usec - begin.usec) / 1000;
			if (elapsed >= 15000) { result.transport = -ETIMEDOUT; break; }
			struct pollfd fd = {.fd = bcm_link_fd(link), .events = POLLIN};
			int ready = poll(&fd, 1, 15000 - elapsed);
			if (ready < 0 && errno == EINTR) continue;
			if (ready <= 0) { result.transport = ready ? -errno : -ETIMEDOUT; break; }
			if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) { result.transport = -EIO; break; }
			int error = bcm_link_receive(link);
			if (error < 0) result.transport = error;
		}
	}
	forced_memzero(wire, sizeof(wire));
	if (!bcm_result_errno(result)) forced_memzero(key, sizeof(*key));
	return bcm_result_errno(result);
}
int bcm_key_set(struct bcm_keys *keys, struct bcm_link *link, const u8 *local, bool data,
                const struct wpa_driver_set_key_params *p)
{
	if (!bcm_unicast(local) || !p || p->key_idx < 0 || p->key_idx > 7 ||
	    p->link_id >= 0) return -EINVAL;
	bool group = p->key_flag & KEY_FLAG_GROUP;
	bool own = !p->addr || is_broadcast_ether_addr(p->addr);
	const u8 zero[6] = {0};
	const u8 *peer = own ? zero : p->addr;
	u16 command = group ? BCM_NAN_HOST_GROUP_KEY_COMMAND : data ?
		BCM_NAN_HOST_DATA_KEY_COMMAND : BCM_NAN_HOST_KEY_COMMAND;
	size_t length = command == BCM_NAN_HOST_DATA_KEY_COMMAND ? 80 : 64;
	if ((!own && !bcm_unicast(peer)) || (!group && own)) return -EINVAL;
	struct bcm_key *key = find(keys, command, local, peer, p->key_idx);
	if (p->alg == WPA_ALG_NONE) {
		/* nan_supplicant's group-key callback keeps RSN_PN_LEN for REMOVE
		 * while passing seq=NULL. It is an absent PN, not key material. */
		if (p->key_len || (p->seq_len && (p->seq || p->seq_len != 6)) ||
		    (group ? p->key_flag != KEY_FLAG_GROUP : p->key_flag != KEY_FLAG_PAIRWISE))
			return -EINVAL;
		/* Common hostap clears slots 0..5 on every new interface, including
		 * NMI slots that cannot hold GTKs. An absent owner needs no firmware
		 * mutation; an existing owner must still complete native removal. */
		return key ? remove_key(keys, link, key) : 0;
	}
	if ((!group && (p->key_idx || !(p->key_flag & KEY_FLAG_PAIRWISE))) ||
	    (group && ((data && p->key_idx >= 4) || (!data && p->key_idx < 4)))) return -EINVAL;
	if (!p->key || (p->seq_len && (p->seq_len != 6 || !p->seq)) ||
	    (group ? p->key_flag != (own ? KEY_FLAG_GROUP_TX_DEFAULT : KEY_FLAG_GROUP_RX) :
	     p->key_flag != KEY_FLAG_PAIRWISE && p->key_flag != KEY_FLAG_PAIRWISE_RX_TX) ||
	    (group && !own && p->seq_len != 6)) return -EINVAL;
	u8 algorithm;
	if (!group || data) {
		if (p->alg == WPA_ALG_CCMP && p->key_len == 16) algorithm = 4;
		else if (p->alg == WPA_ALG_GCMP_256 && p->key_len == 32) algorithm = 16;
		else return -EOPNOTSUPP;
	} else {
		if (p->alg == WPA_ALG_BIP_CMAC_128 && p->key_len == 16) algorithm = 13;
		else if (p->alg == WPA_ALG_BIP_GMAC_256 && p->key_len == 32) algorithm = 19;
		else return -EOPNOTSUPP;
	}
	u8 wire[BCM_NAN_HOST_DATA_KEY_SIZE] = {0};
	bcm_header(wire, BCM_NAN_HOST_ADD, length);
	WPA_PUT_LE16(wire + 6, p->key_len); os_memcpy(wire + 8, peer, 6);
	wire[14] = p->key_idx; wire[15] = algorithm;
	if (p->seq_len) os_memcpy(wire + 16, p->seq, 6);
	os_memcpy(wire + 32, p->key, p->key_len);
	if (group) { os_memcpy(wire + 24, local, 6); wire[30] = own ? 1 : 2; }
	else if (data) os_memcpy(wire + 64, local, 6);
	int error = 0;
	if (key && key->ready && key->length == length &&
	    !os_memcmp_const(key->request, wire, length))
		goto done; /* Do not reset a live key's packet numbers. */
	if (key) {
		/* Reauthentication derives a fresh NM-TK for the same NMI while
		 * an independent NDI can remain active. The caller has drained
		 * NMI TX; remove only this management key before installing it.
		 * Same material with changed PN is not a fresh key and must not
		 * reset replay state. Partial installs still require explicit REMOVE. */
		if (!key->ready || command != BCM_NAN_HOST_KEY_COMMAND ||
		    (key->request[15] == algorithm &&
		     WPA_GET_LE16(key->request + 6) == p->key_len &&
		     !os_memcmp_const(key->request + 32, p->key, p->key_len)))
			error = -EBUSY;
		else
			error = remove_key(keys, link, key);
	}
	if (!error) {
		if (!key)
			for (unsigned i = 0; i < ARRAY_SIZE(keys->entry); ++i)
				if (!keys->entry[i].owned) { key = &keys->entry[i]; break; }
		if (!key) error = -ENOSPC;
		else {
			key->owned = true; key->command = command; key->id = p->key_idx; key->length = length;
			os_memcpy(key->peer, peer, 6); os_memcpy(key->local, local, 6);
			os_memcpy(key->request, wire, length);
			struct bcm_result result = bcm_set(link, command, wire, length);
			error = bcm_result_errno(result); key->ready = !error;
			/* Native key programming can partially mutate on failure. Keep
			 * this owner until explicit REMOVE or completed native STOP. */
		}
	}
done:
	forced_memzero(wire, sizeof(wire)); return error;
}
int bcm_key_sequence(struct bcm_keys *keys, struct bcm_link *link, const u8 *local,
                     const u8 *peer, int id, u8 sequence[8])
{
	const u8 zero[6] = {0};
	if (!sequence || id < 0 || id > 7) return -EINVAL;
	if (!peer || is_broadcast_ether_addr(peer)) peer = zero;
	struct bcm_key *key = find(keys, BCM_NAN_HOST_GROUP_KEY_COMMAND, local, peer, id);
	if (!key || !key->ready) return -ENOKEY;
	u8 wire[BCM_NAN_HOST_GROUP_KEY_SIZE];
	bcm_header(wire, BCM_NAN_HOST_GET_SEQUENCE, sizeof(wire));
	os_memcpy(wire + 8, key->peer, 6); wire[14] = key->id;
	os_memcpy(wire + 24, key->local, 6); wire[30] = key->request[30];
	u8 pn[6];
	struct bcm_result result = bcm_get(link, BCM_NAN_HOST_GROUP_KEY_COMMAND,
		wire, sizeof(wire), pn, sizeof(pn));
	int error = bcm_result_errno(result);
	if (!error && result.used != sizeof(pn)) error = -EPROTO;
	if (!error) { os_memcpy(sequence, pn, 6); sequence[6] = sequence[7] = 0; }
	forced_memzero(wire, sizeof(wire)); forced_memzero(pn, sizeof(pn)); return error;
}
bool bcm_keys_owned(const struct bcm_keys *keys, const u8 *local, const u8 *peer)
{
	for (unsigned i = 0; i < ARRAY_SIZE(keys->entry); ++i)
		if (matching(&keys->entry[i], local, peer)) return true;
	return false;
}
int bcm_keys_remove(struct bcm_keys *keys, struct bcm_link *link, const u8 *local, const u8 *peer)
{
	/* Remove group keys before the pairwise owner/SCB they reference. */
	for (unsigned pass = 0; pass < 2; ++pass)
		for (unsigned i = 0; i < ARRAY_SIZE(keys->entry); ++i) {
			struct bcm_key *key = &keys->entry[i];
			if (!matching(key, local, peer) ||
			    (key->command != BCM_NAN_HOST_GROUP_KEY_COMMAND) != pass) continue;
			int error = remove_key(keys, link, key); if (error) return error;
		}
	return 0;
}
void bcm_keys_stopped(struct bcm_keys *keys) { forced_memzero(keys, sizeof(*keys)); }

#ifndef BCM43752_AWDL_PEER_H
#define BCM43752_AWDL_PEER_H
#include "awdl_session.h"
/* Internal command target: requires syntax-validated unicast address and nonempty IEs. */
int awdl_peer_set(struct awdl_session *, const uint8_t *, const uint8_t *, unsigned);
int awdl_peer_remove(struct awdl_session *);
int awdl_peer_available(void *nan, void *scb, unsigned mode, uint16_t *channel);
void awdl_peer_reinit(void *nan, void *bss, void *scb);
void awdl_peer_recover(struct awdl_session *);
void awdl_peer_bar_complete(void *, unsigned, void *);
void *awdl_peer_tx_owner(struct awdl_session *, void *bss, const uint8_t *ethernet);
void *awdl_peer_rx_owner(struct awdl_session *, const uint8_t *header);
void awdl_group_window(struct awdl_session *);
int awdl_group_ring_owner(void *, unsigned, unsigned, const uint8_t *, unsigned);
#endif

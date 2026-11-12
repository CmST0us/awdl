#ifndef BCM43752_AWDL_TX_H
#define BCM43752_AWDL_TX_H
#include "awdl_session.h"
int awdl_tx_enqueue(struct awdl_session *, uint64_t cookie, uint16_t channel,
                    uint64_t origin, const uint8_t *frame, unsigned length);
int awdl_tx_cancel(struct awdl_session *, uint64_t cookie);
int awdl_tx_drain(struct awdl_session *);
void awdl_tx_kick(struct awdl_session *);
int awdl_tx_stamp(struct awdl_session *, void *packet);
#endif

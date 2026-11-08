#ifndef BCM43752_NAN_HOST_H
#define BCM43752_NAN_HOST_H

#include "nan_commands.h"

/* One NMI owns this transport. Native radio callbacks, never a timer, submit
 * queued frames. PCB completion owns submitted frames until actual release. */
int nan_host_validate(const struct nan_command_digest *, uint32_t,
                       const uint8_t *, size_t, uint8_t *, size_t *);
int nan_host_tx_command(const struct nan_command_digest *, const uint8_t *,
                        size_t, uint8_t *, size_t *);
int nan_host_cancel_command(const struct nan_command_digest *, const uint8_t *,
                            size_t, uint8_t *, size_t *);
int nan_host_stop(void *nan);
int nan_host_idle(void *nan);
void *nan_host_nmi(void *nan);
int nan_host_active(void *nan);
void *nan_tx_packet_header(void *wlc, void *packet);
int nan_host_peer_busy(void *nan, const uint8_t *address);
int nan_host_peer_protected_busy(void *nan, const uint8_t *address);
int nan_host_group_busy(void *nan, void *bss);
/* NAN discovery group or broadcast; never an arbitrary multicast group. */
int nan_host_group_address(const uint8_t *address);
void nan_host_dw_start(void *wlc, void *mac, void *bss, void *runtime,
                        void *event, unsigned band, unsigned flags);
int nan_host_rx_action(void *mac, void *bss, const uint8_t *header,
                        const uint8_t *body, unsigned length, const void *rx);

void nan_host_group_received(void *nan, const uint8_t *header, const uint8_t *body,
                              unsigned length, const void *rx);
#endif

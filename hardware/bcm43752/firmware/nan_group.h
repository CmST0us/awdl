#ifndef BCM43752_NAN_GROUP_H
#define BCM43752_NAN_GROUP_H
#include "nan_commands.h"
int nan_group_validate(const struct nan_command_digest *, uint32_t, const uint8_t *, size_t,
                       uint8_t *, size_t *);
int nan_group_command(const struct nan_command_digest *, const uint8_t *, size_t, uint8_t *,
                      size_t *);
int nan_group_sequence(const struct nan_command_digest *, const uint8_t *, size_t, uint8_t *,
                       size_t *);
int nan_group_stop(void *nan);
int nan_group_idle(void *nan, void *bss, void *scb);
int nan_group_data_ready(void *nan, void *bss, void *scb);
void *nan_group_bss_key(void *km, void *bss, unsigned id, void *info);
void *nan_group_scb_key(void *km, void *scb, unsigned id, unsigned flags, void *info);
unsigned nan_group_tx_id(void *km, void *bss, unsigned management);
int nan_group_tx_crypto(void *nan, unsigned *iv, unsigned *icv);
int nan_group_bip_tx(void *key, void *packet, const uint8_t *header, void *body, unsigned length,
                     void *descriptor);
int nan_group_bip_mic(void *key, void *packet, const uint8_t *header, void *body, unsigned length,
                      void *mmie);
void *nan_group_beacon_copy(void *mac, void *bss, void *template);
unsigned nan_group_beacon_rx(void *mac, void *bss, void *rx, void *plcp, const uint8_t *header,
                              void *body, unsigned length, unsigned band);
int nan_group_mfp_rx(void *mfp, void *bss, void *scb, void *rx, const uint8_t *header,
                     void *packet);
#endif

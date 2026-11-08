#ifndef BCM43752_NAN_STATION_H
#define BCM43752_NAN_STATION_H

#include "nan_commands.h"

int nan_station_validate(const struct nan_command_digest *, uint32_t,
                           const uint8_t *, size_t, uint8_t *, size_t *);
int nan_station_command(const struct nan_command_digest *, const uint8_t *,
                          size_t, uint8_t *, size_t *);
int nan_station_key_command(const struct nan_command_digest *, const uint8_t *,
                              size_t, uint8_t *, size_t *);
int nan_data_station_validate(const struct nan_command_digest *, uint32_t,
                              const uint8_t *, size_t, uint8_t *, size_t *);
int nan_data_station_command(const struct nan_command_digest *, const uint8_t *,
                             size_t, uint8_t *, size_t *);
int nan_data_station_inactivity(const struct nan_command_digest *, const uint8_t *,
                                size_t, uint8_t *, size_t *);
int nan_station_stop(void *nan);
int nan_station_quiesce(void *nan, void *bss, void *scb);
int nan_station_idle(void *nan);
int nan_station_tx_crypto(void *nan, const uint8_t *address,
                             unsigned *iv_length, unsigned *icv_length);
int nan_station_rx_crypto(void *nan, const uint8_t *address);

int nan_station_group_owner(void *nan, void *bss, const uint8_t *address, void **scb);
int nan_station_group_busy(void *nan, void *bss, void *scb);
#endif

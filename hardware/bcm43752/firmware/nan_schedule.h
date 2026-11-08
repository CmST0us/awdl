#ifndef BCM43752_NAN_SCHEDULE_H
#define BCM43752_NAN_SCHEDULE_H

#include "nan_commands.h"

/* Private DAM tail, after the native 0x58 bytes and initialized word. */
#define NAN_DAM_HOST_PEERS 0x5c
#define NAN_DAM_BYTES 0x74

int nan_schedule_validate(const struct nan_command_digest *, uint32_t,
                          const uint8_t *, size_t, uint8_t *, size_t *);
int nan_schedule_command(const struct nan_command_digest *, const uint8_t *,
                         size_t, uint8_t *, size_t *);
int nan_schedule_stop(void *nan);

#endif

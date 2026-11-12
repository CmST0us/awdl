#ifndef BCM43752_AWDL_COMMAND_H
#define BCM43752_AWDL_COMMAND_H
#include "nan_commands.h"

int awdl_command_validate(const struct nan_command_digest *, uint32_t,
                          const uint8_t *, size_t, uint8_t *, size_t *);
int awdl_command_query(const struct nan_command_digest *, const uint8_t *,
                       size_t, uint8_t *, size_t *);
int awdl_command_set(const struct nan_command_digest *, const uint8_t *,
                     size_t, uint8_t *, size_t *);
#endif

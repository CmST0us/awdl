#ifndef BCM43752_NAN_COMMANDS_H
#define BCM43752_NAN_COMMANDS_H

#include <stddef.h>
#include <stdint.h>

/* The A2 bcmiov ABI uses 16-bit XTLV options and 32-bit pointers/size_t. */
struct nan_command_digest;
typedef int (*nan_command_handler)(const struct nan_command_digest *,
                                  const uint8_t *, size_t, uint8_t *, size_t *);
typedef int (*nan_command_validator)(const struct nan_command_digest *, uint32_t,
                                    const uint8_t *, size_t, uint8_t *, size_t *);

struct nan_command {
    uint16_t id, flags, module_flags, xtlv_options;
    nan_command_validator validate;
    nan_command_handler get, set;
    uint16_t xtlv_offset, min_set, max_set, min_get, max_get;
};

_Static_assert(sizeof(void *) == 4, "A2 requires a 32-bit firmware compiler");
_Static_assert(sizeof(struct nan_command) == 32, "A2 command table stride");
_Static_assert(offsetof(struct nan_command, validate) == 8, "A2 validator offset");
_Static_assert(offsetof(struct nan_command, min_get) == 26, "A2 get length offset");

int nan_command_validate(const struct nan_command_digest *digest, uint32_t action,
                          const uint8_t *input, size_t length,
                          uint8_t *output, size_t *output_length);

/* Caller provides the A2 NAN common context. Its +0x28 field owns the parser.
 * Attach rejects a duplicate; detach releases actual parser allocations.
 * This is a submodule API, not a radio enable or capability advertisement.
 */
int nan_commands_attach(void *nan_context);
int nan_commands_detach(void *nan_context);
int nan_commands_register(void *nan_context, void *command_context,
                          const struct nan_command *commands, size_t count);
int nan_statistics(void *nan_context, const uint8_t *request,
                   uint8_t *output, size_t *output_length);

#endif

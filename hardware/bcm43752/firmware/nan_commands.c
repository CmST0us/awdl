/* Native A2 batch-command registration. The handler addresses below belong
 * only to the hash-locked 18.35.387.23.146 ROM/RAM pair. See PATCHING.md.
 * Handlers keep their actual validation, state changes, events and errors.
 */
#include "nan_commands.h"
#include "nan_link.h"
#include "nan_initialize.h"
#include "nan_mac.h"
#include "nan_host.h"
#include "nan_station.h"
#include "nan_group.h"
#include "nan_schedule.h"
#include "nan_host_abi.h"

typedef void *(*parse_alloc_fn)(void *, size_t);
typedef void (*parse_free_fn)(void *, void *, size_t);
typedef int (*parse_digest_fn)(void *, struct nan_command_digest **);

struct nan_parse_config {
    uint32_t options;
    parse_alloc_fn allocate;
    parse_free_fn release;
    parse_digest_fn digest;
    uint32_t max_registrations;
    void *allocation_context;
};

_Static_assert(sizeof(struct nan_parse_config) == 24, "A2 parse config size");

#define ROM_HANDLER(address) ((nan_command_handler)((address) | 1u))
#define VALIDATE nan_command_validate

/* A2 0x1f75c registers thirteen borrowed records at core+0x74. The missing
 * 0x151070 dispatcher serves both command 0x801 and native ecounters. Keep
 * the actual module functions and the native generic XTLV encoder; no
 * synthetic counters, allocations or ownership changes belong here. */
typedef int (*nan_statistics_fn)(void *, unsigned, unsigned, unsigned,
                                 uint8_t *, size_t, size_t *);
struct nan_statistics_entry {
    nan_statistics_fn get;
    void *data;
    uint16_t bytes, type;
};
_Static_assert(sizeof(struct nan_statistics_entry) == 12, "A2 statistics stride");

static void statistics_word(uint8_t *output, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        output[i] = (uint8_t)(value >> (8 * i));
}

int nan_statistics(void *nan, const uint8_t *request,
                   uint8_t *output, size_t *output_length)
{
    if (!output_length)
        return -2;
    size_t capacity = *output_length;
    *output_length = 0;
    if (!nan || !request || !output || request[4] > 1)
        return -2;
    struct nan_statistics_entry *entries =
        *(struct nan_statistics_entry **)((uint8_t *)nan + 0x74);
    if (!entries)
        return -30;
    if (capacity < 8)
        return -14;
    uint32_t mask = (uint32_t)request[0] | (uint32_t)request[1] << 8 |
                    (uint32_t)request[2] << 16 | (uint32_t)request[3] << 24;
    size_t used = 8;
    unsigned count = 0;
    statistics_word(output, 0);
    statistics_word(output + 4, 8);
    for (unsigned i = 0; i < 13; ++i) {
        if (!(mask & (1u << i)))
            continue;
        size_t bytes = 0;
        int error = 0;
        if (entries[i].get)
            error = entries[i].get(nan, request[4], request[5], request[6],
                                   output + used, capacity - used, &bytes);
        else if (entries[i].data)
            error = ((int (*)(void *, unsigned, uint8_t *, size_t,
                               size_t *, unsigned))0x1e35du)(
                nan, request[4], output + used, capacity - used, &bytes, i);
        if (error)
            return error;
        if (bytes > capacity - used)
            return -14;
        used += bytes;
        count += bytes != 0;
    }
    statistics_word(output, count);
    statistics_word(output + 4, used);
    *output_length = used;
    return 0;
}

static int statistics_command(const struct nan_command_digest *digest,
                               const uint8_t *input, size_t length,
                               uint8_t *output, size_t *output_length)
{
    /* Original 0x1defc discards the dispatch error. Do not report success
     * for a short buffer or a failed module; ecounters uses the same core. */
    if (length != 8) {
        *output_length = 0;
        return -14;
    }
    return nan_statistics(((void *const *)digest)[1], input, output, output_length);
}

int nan_command_validate(const struct nan_command_digest *digest, uint32_t action,
                          const uint8_t *input, size_t length,
                          uint8_t *output, size_t *output_length)
{
    void *nan = ((void *const *)digest)[1];
    void *common = *(void **)((uint8_t *)nan + 0x10);
    const struct nan_command *command = ((const struct nan_command *const *)digest)[3];
    if (*(uint8_t *)((uint8_t *)common + 0x30) && command->id != 0x101 &&
        !(command->id == 0x116 && length == 1 && !input[0]))
        return -16;
    if ((command->id == 0x101 || command->id == 0x116) &&
        length == 1 && input[0] > 1)
        return -2; /* Reject the entire batch before any earlier command mutates. */
    return ((nan_command_validator)0x29069u)(
        digest, action, input, length, output, output_length);
}

static int initialize_command(const struct nan_command_digest *digest,
                               const uint8_t *input, size_t length,
                               uint8_t *output, size_t *output_length)
{
    (void)output;
    if (length != 1 || input[0] > 1)
        return -2;
    void *nan = ((void *const *)digest)[1];
    uint8_t *wlc = *(uint8_t **)((uint8_t *)nan + 8);
    uint8_t *common = *(uint8_t **)((uint8_t *)nan + 0x10);
    uint8_t power = wlc[0x131];
    wlc[0x131] = 1;
    ((void (*)(void *))0x8c3bdu)(wlc);
    int error;
    if (!input[0])
        error = nan_deinitialize(wlc, nan);
    else if (common[0x30])
        error = -16;
    else if (*(uint32_t *)(common + 0x34) & 0x80000000u)
        error = 0;
    else
        error = nan_initialize(wlc, nan);
    /* Single NAN does not enable the reference's scan/STA coexistence hook.
     * Preserve native power bracketing and report deinit errors to the host. */
    wlc[0x131] = power;
    ((void (*)(void *))0x8c3bdu)(wlc);
    *output_length = 0;
    return error;
}

static int enable_command(const struct nan_command_digest *digest,
                           const uint8_t *input, size_t length,
                           uint8_t *output, size_t *output_length)
{
    (void)output;
    if (length != 1 || input[0] > 1)
        return -2;
    uint8_t *nan = ((uint8_t *const *)digest)[1];
    uint8_t *wlc = *(uint8_t **)(nan + 8);
    uint8_t *mac = *(uint8_t **)(nan + 0x14);
    uint8_t *pub = *(uint8_t **)wlc;
    uint8_t *pub_common = *(uint8_t **)(pub + 4);
    *output_length = 0;
    if (input[0] && pub_common[0x30]) {
        uint8_t *runtime = *(uint8_t **)(mac + 0x38);
        uint8_t *dam = *(uint8_t **)(nan + 0x5c);
        return runtime[0x4a] && dam[0x25] &&
            !*(uint32_t *)(mac + NAN_MAC_HOST_STOPPING) &&
            *(void **)(mac + NAN_MAC_EVENT_BSS) ?
            nan_bss_events(nan, 1) : -16;
    }
    /* A failed enable may own timers/schedules before common state commits.
     * The native setter skips STOP in that state. Keep it reachable whenever
     * the prepared NMI BSS exists, including after a failed cancellation. */
    if (!input[0] && !*(void **)(mac + NAN_MAC_BSS_MANAGER))
        return 0;
    unsigned band = ((int (*)(void *))0x2f5b9u)(mac) ? 2 : 0;
    return ((int (*)(void *, void *, unsigned, unsigned))0x1e05du)(
        wlc, nan, input[0], band);
}

static int validate_host_control(const struct nan_command_digest *digest,
                                 uint32_t action, const uint8_t *input,
                                 size_t length, uint8_t *output, size_t *output_length)
{
    int error = VALIDATE(digest, action, input, length, output, output_length);
    if (error || length != 4)
        return error;
    uint32_t value = (uint32_t)input[0] | (uint32_t)input[1] << 8 |
                     (uint32_t)input[2] << 16 | (uint32_t)input[3] << 24;
    if ((value & (NAN_CONTROL_AUTOMATIC | NAN_CONTROL_INFRA_COEX |
                  NAN_CONTROL_DELETE_INACTIVE)) || !(value & NAN_CONTROL_HOST_NDP_ID))
        return -2;
    return 0;
}

static int validate_peer_capacity(const struct nan_command_digest *digest,
                                  uint32_t action, const uint8_t *input,
                                  size_t length, uint8_t *output, size_t *output_length)
{
    int error = VALIDATE(digest, action, input, length, output, output_length);
    if (error || length != 1)
        return error;
    if (!input[0] || input[0] > 32)
        return -2;
    /* The A2 command digest's second word is the registered NAN context.
     * Native SET writes cfg[2] without resizing any allocated peer arrays. */
    void *nan = ((void *const *)digest)[1];
    void *common = *(void **)((uint8_t *)nan + 0x10);
    uint8_t *config = *(uint8_t **)((uint8_t *)nan + 0x6c);
    if (input[0] != config[2] &&
        ((*(uint32_t *)((uint8_t *)common + 0x34) & 0x80000000u) ||
         *(void **)((uint8_t *)nan + 0x40)))
        return -16;
    return 0;
}

static int validate_host_control2(const struct nan_command_digest *digest,
                                  uint32_t action, const uint8_t *input,
                                  size_t length, uint8_t *output, size_t *output_length)
{
    int error = VALIDATE(digest, action, input, length, output, output_length);
    if (error || length != 8)
        return error;
    /* The native SET copies both words without checking their consumers.
     * Only SDF transmission during host-owned negotiation may be changed here.
     * Autonomous schedule updates, Instant Mode and firmware security/pairing
     * must not activate a second protocol owner or missing A2 callbacks.
     * Preserve module-owned bits, including scheduler SLOT_PRE_CLOSE. */
    void *nan = ((void *const *)digest)[1];
    const uint8_t *flags = *(uint8_t **)((uint8_t *)nan + 0x10) + 0x44;
    if ((input[0] & ~3u) != (flags[0] & ~3u))
        return -2;
    for (size_t i = 1; i < 8; ++i)
        if (input[i] != flags[i])
            return -2;
    return 0;
}

#define COMMAND(id_, mflags_, get_, set_, smin_, smax_, gmin_, gmax_) \
    { .id = (id_), .module_flags = (mflags_), .validate = VALIDATE, \
      .get = (get_), .set = (set_), .min_set = (smin_), .max_set = (smax_), \
      .min_get = (gmin_), .max_get = (gmax_) }

/* Native configuration command IDs and bounds. Init/enable wrappers preserve
 * native core/module behavior while retaining failures and cleanup ownership.
 * Host frames, stations and pairwise keys share the same batch validation. */
static const struct nan_command core_commands[] = {
    { .id = BCM_NAN_HOST_GROUP_KEY_COMMAND, .validate = nan_group_validate,
      .set = nan_group_command, .get = nan_group_sequence,
      .min_set = BCM_NAN_HOST_GROUP_KEY_SIZE, .max_set = BCM_NAN_HOST_GROUP_KEY_SIZE,
      .min_get = BCM_NAN_HOST_GROUP_KEY_SIZE, .max_get = BCM_NAN_HOST_GROUP_KEY_SIZE },
    { .id = BCM_NAN_HOST_DATA_STATION_COMMAND, .validate = nan_data_station_validate,
      .set = nan_data_station_command, .get = nan_data_station_inactivity,
      .min_set = BCM_NAN_HOST_DATA_STATION_SIZE,
      .max_set = BCM_NAN_HOST_DATA_STATION_SIZE + BCM_NAN_HOST_DATA_IE_MAX,
      .min_get = BCM_NAN_HOST_DATA_STATION_SIZE, .max_get = BCM_NAN_HOST_DATA_STATION_SIZE },
    { .id = BCM_NAN_HOST_DATA_KEY_COMMAND, .validate = nan_data_station_validate,
      .set = nan_station_key_command, .min_set = BCM_NAN_HOST_DATA_KEY_SIZE,
      .max_set = BCM_NAN_HOST_DATA_KEY_SIZE },
    { .id = BCM_NAN_HOST_LOCAL_SCHEDULE_COMMAND, .validate = nan_schedule_validate,
      .set = nan_schedule_command, .min_set = BCM_NAN_HOST_SCHEDULE_SIZE,
      .max_set = BCM_NAN_HOST_SCHEDULE_SIZE },
    { .id = BCM_NAN_HOST_PEER_SCHEDULE_COMMAND, .validate = nan_schedule_validate,
      .set = nan_schedule_command, .min_set = BCM_NAN_HOST_SCHEDULE_SIZE,
      .max_set = BCM_NAN_HOST_SCHEDULE_SIZE },
    { .id = BCM_NAN_HOST_TX_COMMAND, .validate = nan_host_validate,
      .set = nan_host_tx_command, .min_set = 54, .max_set = BCM_NAN_HOST_MAX_MESSAGE },
    { .id = BCM_NAN_HOST_CANCEL_COMMAND, .validate = nan_host_validate,
      .set = nan_host_cancel_command, .min_set = 24, .max_set = 24 },
    { .id = BCM_NAN_HOST_STATION_COMMAND, .validate = nan_station_validate,
      .set = nan_station_command, .min_set = 24, .max_set = 24 },
    { .id = BCM_NAN_HOST_KEY_COMMAND, .validate = nan_station_validate,
      .set = nan_station_key_command, .min_set = 64, .max_set = 64 },
    COMMAND(0x0000, 0, ROM_HANDLER(0x1df1c), 0, 0, 0, 2, 2), /* version */
    COMMAND(0x0103, 0, ROM_HANDLER(0x1da64), ROM_HANDLER(0x1da84), 1, 1, 0, 0), /* hop */
    COMMAND(0x0104, 0, ROM_HANDLER(0x1daa0), ROM_HANDLER(0x1dac4), 1, 1, 0, 0), /* limit */
    COMMAND(0x0105, 0, ROM_HANDLER(0x1de90), ROM_HANDLER(0x1deb8), 4, 4, 0, 0), /* warmup */
    COMMAND(0x0106, 4, ROM_HANDLER(0x1db1c), 0, 0, 0, 0, 0), /* info */
    COMMAND(0x010c, 0, ROM_HANDLER(0x1da18), ROM_HANDLER(0x1dd98), 6, 6, 0, 0), /* cluster */
    COMMAND(0x010d, 1, ROM_HANDLER(0x1dae4), ROM_HANDLER(0x1db08), 6, 6, 0, 0), /* NMI */
    COMMAND(0x010e, 0, ROM_HANDLER(0x1d8a8), ROM_HANDLER(0x1d8cc), 2, 2, 0, 0), /* discovery beacon */
    COMMAND(0x010f, 0, ROM_HANDLER(0x1dd60), ROM_HANDLER(0x1dd84), 2, 2, 0, 0), /* SDF TX time */
    COMMAND(0x0110, 0, ROM_HANDLER(0x1ddb4), ROM_HANDLER(0x1ddd8), 4, 4, 0, 0), /* SIDs */
    COMMAND(0x0111, 0, ROM_HANDLER(0x1d8e0), ROM_HANDLER(0x1d904), 2, 2, 0, 0), /* DW */
    COMMAND(0x0114, 0, ROM_HANDLER(0x1d97c), ROM_HANDLER(0x1d9cc), 4, 20, 0, 0), /* events */
    { .id = 0x0115, .validate = validate_host_control,
      .get = ROM_HANDLER(0x1dbf8), .set = ROM_HANDLER(0x1dc44),
      .min_set = 4, .max_set = 4 }, /* single-NAN, host-owned negotiation/lifecycle */
    { .id = 0x0510, .module_flags = 1, .validate = validate_peer_capacity,
      .get = ROM_HANDLER(0x29114), .set = ROM_HANDLER(0x29130),
      .min_set = 1, .max_set = 1 }, /* peers: configure before initialization */
    COMMAND(0x0101, 1, ROM_HANDLER(0x1db44), initialize_command, 1, 1, 0, 0), /* init */
    COMMAND(0x0116, 0, ROM_HANDLER(0x1d918), enable_command, 1, 1, 0, 0), /* enable */
    COMMAND(0x0102, 0, ROM_HANDLER(0x1d848), ROM_HANDLER(0x1d86c), 48, 48, 0, 0), /* role */
    COMMAND(0x0801, 0, statistics_command, 0, 0, 0, 8, 8), /* statistics */
    COMMAND(0x011b, 0, ROM_HANDLER(0x1ddec), ROM_HANDLER(0x1de30), 8, 1024, 0, 1024), /* vendor payload */
    COMMAND(0x0511, 0, ROM_HANDLER(0x1dd18), ROM_HANDLER(0x1dd40), 2, 2, 0, 0), /* NDP idle */
    COMMAND(0x0513, 0, ROM_HANDLER(0x1dcd0), ROM_HANDLER(0x1dcf8), 2, 2, 0, 0), /* NDP heartbeat */
    COMMAND(0x011d, 0, ROM_HANDLER(0x1dbc8), ROM_HANDLER(0x1dbdc), 1, 1, 0, 0), /* min TX rate */
    COMMAND(0x0802, 0, ROM_HANDLER(0x1df8c), 0, 0, 0, 0, 0), /* capabilities */
    { .id = 0x0118, .validate = validate_host_control2,
      .get = ROM_HANDLER(0x1df34), .set = ROM_HANDLER(0x1df64),
      .min_set = 8, .max_set = 8 }, /* host-owned control2 policies */
};

static void **parser_slot(void *nan_context)
{
    return (void **)((uint8_t *)nan_context + 0x28);
}

static int allocate_digest(void *context, struct nan_command_digest **digest)
{
    (void)context;
    void *(*allocate_zero)(size_t) = (void *(*)(size_t))0x7146du;
    *digest = allocate_zero(28);
    return *digest ? 0 : -27;
}

int nan_commands_detach(void *nan_context)
{
    int (*free_parse_context)(void **, parse_free_fn) =
        (int (*)(void **, parse_free_fn))0x20c0e5u;
    if (!nan_context)
        return -2;
    return free_parse_context(parser_slot(nan_context), (parse_free_fn)0x29105u);
}

int nan_commands_register(void *nan_context, void *command_context,
                          const struct nan_command *commands, size_t count)
{
    int (*register_commands)(void *, void *, const struct nan_command *, size_t,
                             const void *, size_t) =
        (int (*)(void *, void *, const struct nan_command *, size_t,
                  const void *, size_t))0x20c1c1u;
    if (!nan_context || !*parser_slot(nan_context) || !commands || !count)
        return -2;
    return register_commands(*parser_slot(nan_context), command_context, commands, count, 0, 0);
}

int nan_commands_attach(void *nan_context)
{
    int (*create_parse_context)(const struct nan_parse_config *, void **) =
        (int (*)(const struct nan_parse_config *, void **))0x20c131u;
    if (!nan_context)
        return -2;
    if (*parser_slot(nan_context))
        return -16;
    const struct nan_parse_config config = {
        .options = 1, /* native batch validation before dispatch */
        .allocate = (parse_alloc_fn)0x290e5u,
        .release = (parse_free_fn)0x29105u,
        .digest = allocate_digest,
        .max_registrations = 8, /* native core + component command groups */
        .allocation_context = nan_context,
    };
    int error = create_parse_context(&config, parser_slot(nan_context));
    if (error)
        return error;
    error = nan_commands_register(nan_context, nan_context, core_commands,
                                   sizeof(core_commands) / sizeof(core_commands[0]));
    if (error)
        nan_commands_detach(nan_context);
    return error;
}

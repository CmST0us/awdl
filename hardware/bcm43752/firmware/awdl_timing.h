#ifndef BCM43752_AWDL_TIMING_H
#define BCM43752_AWDL_TIMING_H
#include <stdint.h>
#include "awdl_host_abi.h"

/* Owned by one AWDL session; never stored in the NAN scheduler context.
 * Callbacks and command processing run on the firmware's serialized WLC
 * execution path. The owning session checks generation before calling.
 */
struct awdl_timing {
    uint64_t epoch, open, close;
    uint16_t channels[BCM_AWDL_PLAN_ENTRIES];
    uint16_t primary, secondary, active;
    uint32_t drain_us;
    unsigned planned, on_channel;
    /* Recomputed from native residency, peer window and public plan before
     * admission; zero means no verified residency. Never extends ownership. */
    uint64_t channel_close;
};

/* Inputs contain the payload after the common ABI header. Channel legality
 * and the primary/secondary pair are validated by native CLM at START.
 * Invalid updates preserve the previous owned schedule/window.
 */
int awdl_timing_plan(struct awdl_timing *, const uint8_t *, unsigned);
int awdl_timing_window(struct awdl_timing *, const uint8_t *, unsigned);
void awdl_timing_channel(struct awdl_timing *, uint16_t channel, int opened);
void awdl_timing_close(struct awdl_timing *);
/* Clip a native residency end at the first different public channel.
 * Missing plans, pre-epoch time and expired residency cannot admit TX. */
uint64_t awdl_timing_deadline(const struct awdl_timing *, uint64_t now,
                              uint64_t end);
/* Admit only after a native channel-open event, in the negotiated peer
 * window, with enough time to drain before a different channel/unavailable
 * slot. This never fabricates an MSCH event or advances a radio clock.
 */
int awdl_timing_can_send(const struct awdl_timing *, uint64_t now);
#endif

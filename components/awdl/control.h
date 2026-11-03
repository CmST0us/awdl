#ifndef AWARE_AWDL_CONTROL_H
#define AWARE_AWDL_CONTROL_H

#include <stddef.h>
#include <stdint.h>

struct Control;
/* Synchronous control events; IP payload stays in the native kernel netdev.
 * 0: 802.11 action frame; 1: peer MAC (6) + capabilities (28); 4: peer lost.
 * 5: opening/closing times (two little-endian u64 microseconds; zero closes).
 * 6: channel plan (i64 epoch, 16 channels, preceding/following channel).
 * 7: updated capabilities: VHT IE (12), AWDL HT TLV prefix (8), HE summary (8).
 * Times share the caller's control_tick/control_rx epoch, starting at 1 us.
 * Return zero after applying the event, or a negative errno on failure.
 */
typedef int (*control_emit)(void *, uint32_t, const unsigned char *, size_t);
/* secondary_channel == 0 keeps the primary channel and emits peer windows.
 * Otherwise the backend applies channel plans within the supported pair.
 * tx_drain_us reserves firmware drain time before a channel switch.
 * he is NULL or points to the local 21-byte HE capability.
 */
struct Control *control_new(const unsigned char *mac, unsigned char channel,
                            unsigned char secondary_channel, const char *target,
                            const char *local_name, const unsigned char *he,
                            uint32_t tx_drain_us, control_emit emit, void *context);
uint64_t control_tick(struct Control *, uint64_t now_us);
void control_rx(struct Control *, const unsigned char *, size_t,
                uint64_t now_us);
int control_select(struct Control *, const char *target);
void control_free(struct Control *);

#endif

#ifndef BCM43752_AWDL_NATIVE_H
#define BCM43752_AWDL_NATIVE_H
#include <stdint.h>

/* Hash-locked A2 primitives. Status is separate from the sampled value:
 * a missing clock must not be mistaken for the valid boot timestamp zero. */
int awdl_native_clock(void *nan, uint64_t *microseconds);
int awdl_native_rx_clock(void *nan, const void *rx, uint64_t *microseconds);
int awdl_native_channels(void *nan, uint16_t primary, uint16_t secondary);
#endif

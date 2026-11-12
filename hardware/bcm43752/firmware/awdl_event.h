#ifndef BCM43752_AWDL_EVENT_H
#define BCM43752_AWDL_EVENT_H
#include "awdl_session.h"
void awdl_event_nan_dw(void *nan, uint16_t channel);

/* Copies one event through the existing native NAN event constructor.
 * Return zero means handed to that constructor, not host receipt or ACK. */
int awdl_event_emit(struct awdl_session *, unsigned kind, uint64_t timestamp,
                    uint64_t cookie, int result, uint16_t channel, unsigned flags,
                    const uint8_t *header, const uint8_t *body, unsigned body_bytes);
int awdl_receive_action(void *nan, const uint8_t *header, const uint8_t *body,
                        unsigned length, const void *rx);
#endif

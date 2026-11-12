#ifndef BCM43752_AWDL_SCHEDULE_H
#define BCM43752_AWDL_SCHEDULE_H
#include "awdl_session.h"
int awdl_schedule_install(struct awdl_session *, const uint8_t *, unsigned);
int awdl_schedule_window(struct awdl_session *);
int awdl_schedule_remove(struct awdl_session *);
#endif

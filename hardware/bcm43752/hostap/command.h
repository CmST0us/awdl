/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BCM_NAN_COMMAND_H
#define BCM_NAN_COMMAND_H
#include "utils/includes.h"
#include "utils/common.h"
#include "netlink.h"
#include "../include/nan_host_abi.h"
#include "../include/nan_vendor_abi.h"

/* A command result is not a lifecycle or packet-completion event. The
 * transport result is preserved separately for mutations with unknown fate. */
struct bcm_result { int transport; int32_t native; size_t used; };
struct bcm_result bcm_command(struct bcm_link *, u16 command, u16 operation,
                              const void *, size_t, void *, size_t);
int bcm_result_errno(struct bcm_result);
struct bcm_result bcm_set(struct bcm_link *, u16 command, const void *, size_t);
struct bcm_result bcm_get(struct bcm_link *, u16 command, const void *, size_t,
                          void *, size_t);
void bcm_header(u8 *wire, u16 operation, u16 length);
bool bcm_unicast(const u8 *address);
#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WL_NAN_NDI_H
#define WL_NAN_NDI_H

#include <linux/spinlock.h>
#include <linux/if.h>
#include <linux/workqueue.h>

/* Two native NDI BSSes; the NMI has BSSCFG_NOIF and is firmware-owned.
 * Commands are serialized by cfg->if_sync. DPC only changes these records
 * under lock; it must never wait for RTNL or issue an IOVAR. Records survive
 * command failures and hostap restarts, and expire with the owning wiphy. */
struct wl_nan_ndi_owner {
	u64 awdl_generation; /* Pending/active owner; STOP or empty QUERY releases. */
	u32 flags;
	u32 ifindex;
	u8 ifidx, bssidx, mac[6];
	char name[IFNAMSIZ];
	char firmware_name[IFNAMSIZ];
	s32 ioctl_result, host_result;
};

struct wl_nan_ndi_state {
	spinlock_t lock;
	/* Separate from owner lock: TX completion can hold DHD_GENERAL_LOCK. */
	spinlock_t work_lock;
	struct work_struct work;
	bool initialized, stopping;
	struct wl_nan_ndi_owner owner[2];
};

#endif

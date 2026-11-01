/* SPDX-License-Identifier: GPL-2.0 */
#ifndef U2_BIGTK_H
#define U2_BIGTK_H
#include "u2_bip.h"
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

/* NAN func8: op, key ID, NMI[6], initial IPN[6], ports[2], key[16].
 * op1 installs once per START; op2 copies an atomic public snapshot.
 * STOP removes the key after the native queues have been stopped. */
struct u2_bigtk_snapshot {
    u8 seq, index, nmi[6], ipn[6], pending, active;
    u32 prepared, completed, skipped, error, calculation_us;
    u8 frames[2][84];
};
struct u2_faw_prepared {
    u32 duration[2];
    u16 macid;
    u8 channel[15], record;
};
struct u2_faw_pending {
    struct u2_faw_prepared prepared;
    u32 generation;
};
struct u2_tssi_path {
    u32 alignment[4];
    u32 cck_de, ofdm_de;
    u8 enabled, offset, valid, reserved;
};
struct u2_iqc_channel {
    u8 center, width, page, valid;
    u32 enable[2], tx[2], rx[2];
    struct u2_tssi_path tssi[2];
};
struct u2_bigtk_state {
    struct u2_bigtk_snapshot snapshot;
    u8 active, index, nmi[6], ipn[6], pending, ports[2];
    u8 faw_windows, ending_macid, ending;
    u32 prepared, completed, skipped, error, calculation_us;
    u8 key[16];
    u32 base[2];
    u8 frame[2][84];
    /* Data MACIDs have independent FAW gates; STOP retires all members. */
    struct { u8 count, macid[2], open; } faw, faw_snapshot;
    u32 faw_generation;
    struct u2_faw_pending pending_faw[3];
    struct u2_iqc_channel iqc[2];
    u8 rfk_hold, rfk_wait, rfk_seq;
    u32 channel_generation, channel_deferred;
    u8 channel_switching, channel_ready, channel_center, channel_width;
    u8 channel_open_mask;
    u8 channel_descriptor[15], iqc_cached;
    u32 iqc_skipped;
    u32 tail_low, tail_high, tail_generation, tail_saved, tail_events;
    u8 tail_macid, tail_active, tail_owner;
    u32 tail_arms;
    u8 data_windows[2];
};
_Static_assert(sizeof(struct u2_bigtk_snapshot) == 204, "public snapshot ABI");
_Static_assert(sizeof(struct u2_bigtk_state) == 784, "U2 state layout");
_Static_assert(__builtin_offsetof(struct u2_bigtk_state, key) == 244, "key readback ABI");
_Static_assert(__builtin_offsetof(struct u2_bigtk_state, faw) == 436, "FAW readback ABI");
_Static_assert(__builtin_offsetof(struct u2_bigtk_state, faw_snapshot) == 440, "FAW snapshot ABI");
extern struct u2_bigtk_state u2_bigtk_state;
u32 u2_bigtk_dispatch(u8 *header);
u32 u2_bigtk_prepare(u32 port, u32 mbssid, u32 band);
u32 u2_bigtk_gate(u32 band, u32 port, u32 enable);
void u2_bigtk_done(u32 report);
u32 u2_nan_faw_gate(u32 macid, u32 pause, u32 sleep, u32 reason);
void u2_nan_faw_end(u32 macid);
void u2_nan_window_tail(u32 token);
void u2_nan_window_arm(u32 macid, u32 low, u32 high);
u32 u2_nan_schedule(u32, u32, u32, u32, u32, u32);
u32 u2_nan_faw_begin(u32 token);
u32 u2_nan_faw_channel(void *channel, u32 complete);
u32 u2_nan_iqc_channel(void *channel, u32 complete);
u32 u2_nan_discovery_channel(void *channel, u32 complete);
#endif

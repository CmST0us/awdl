/* SPDX-License-Identifier: GPL-2.0 */
/* U2 native beacon ownership and one NMI transmit BIGTK. */
#include "u2_bigtk.h"

extern u32 big_enter(void);
extern void big_leave(u32 flags);
extern u32 big_native_dispatch(u8 *);
extern u32 big_native_prepare(u32, u32, u32);
extern u32 big_native_gate(u32, u32, u32);
extern u32 big_queue_lock(void);
extern void big_queue_unlock(void);
extern u32 big_time(void);
extern u32 big_stopped(void);
extern u32 big_frame(u32 port, u8 **base, volatile u8 **wd);
extern u32 big_macid_gate(u32 macid, u32 pause, u32 sleep, u32 reason);
extern u32 big_schedule_macid(void);
extern u32 big_faw_macid(void);
extern u32 big_native_faw_channel(void *, u32);
extern void big_native_faw_end(void);
extern u32 big_native_faw_begin(void);
extern u32 big_faw_continue(void);
extern u32 big_timer_schedule(u32, u32, u32, u32, u32, u32);
extern void big_faw_capture(struct u2_faw_prepared *);
extern void big_faw_restore(const struct u2_faw_prepared *);
extern u32 big_data_channel(u8 *);
extern void big_iqc_restore(const struct u2_iqc_channel *, u32);
extern u32 big_rfk_grant(u32 seq);
extern void big_window_trace(u32 marker, u32 first, u32 second);
extern int big_tssi_matches(const struct u2_iqc_channel *);
extern u32 big_aggregate_time(u32 units);
extern unsigned long long big_free_time(void);

#define S u2_bigtk_state
static void copy(u8 *d, const u8 *s, u32 n);
static int channel_owner_bit(u32 macid)
{
    u32 i;
    if (macid == big_schedule_macid()) return 0;
    for (i = 0; i < S.faw.count; i++)
        if (macid == S.faw.macid[i]) return i + 1;
    return -1;
}
static u32 channel_gate(u32 macid, u32 pause, u32 sleep, u32 reason)
{
    int bit = channel_owner_bit(macid);
    if (S.active && bit >= 0) {
        if (pause || sleep) S.channel_open_mask &= ~(1U << bit);
        else {
            S.channel_open_mask |= 1U << bit;
            if (S.channel_switching || !S.channel_ready || S.rfk_hold) {
                pause = sleep = 1;
                S.channel_deferred++;
            }
        }
    }
    return big_macid_gate(macid, pause, sleep, reason);
}
static int data_reserved(u8 *channel)
{
    u32 owner;
    if (!S.active) return 0;
    owner = big_data_channel(channel);
    return owner < 128 && channel_owner_bit(owner) >= 0;
}
u32 u2_nan_iqc_channel(void *channel, u32 complete)
{
    u8 *ch = channel;
    u32 ret, i, primary, owner;
    u8 data[15];
    int owner_bit, same = S.iqc_cached;
    for (i = 0; i < 15; i++) if (S.channel_descriptor[i] != ch[i]) same = 0;
    if (S.rfk_hold) return 0;
    if (S.active) {
        owner = big_data_channel(data);
        if (owner == 0xffff || data[1] != ch[1] || data[2] != ch[2])
            owner = big_schedule_macid();
        owner_bit = channel_owner_bit(owner);
        S.channel_open_mask &= owner_bit < 0 ? 0 : (1U << owner_bit);
        S.channel_switching = 1;
        S.channel_ready = 0;
        S.channel_generation++;
        primary = big_schedule_macid();
        if (primary < 128) big_macid_gate(primary, 1, 1, 36);
        for (i = 0; i < S.faw.count; i++)
            big_macid_gate(S.faw.macid[i], 1, 1, 36);
    }
    ret = big_native_faw_channel(channel, complete);
    /* Without installed dual-channel backups the native path owns calibration. */
    S.channel_ready = !ret && !(S.iqc[0].valid && S.iqc[1].valid);
    if (!ret && S.active && S.iqc[0].valid && S.iqc[1].valid)
        for (i = 0; i < 2; i++)
            if (S.iqc[i].center == ch[1] && S.iqc[i].width == ch[2]) {
                u32 preserve = same && big_tssi_matches(&S.iqc[i]);
                if (preserve) S.iqc_skipped++;
                big_iqc_restore(&S.iqc[i], preserve);
                S.channel_ready = 1;
                break;
            }
    if (S.channel_generation <= 24)
        big_window_trace(252,(same<<24)|(S.iqc_cached<<16)|(S.iqc[0].valid<<8)|S.iqc[1].valid,ret);
    S.iqc_cached = S.active && S.channel_ready && !ret &&
                   S.iqc[0].valid && S.iqc[1].valid;
    for (i = 0; i < 15; i++) S.channel_descriptor[i] = ch[i];
    S.channel_center = ch[1]; S.channel_width = ch[2];
    S.channel_switching = 0;
    if (S.active && S.channel_ready && !S.rfk_hold) {
        primary = big_schedule_macid();
        S.channel_open_mask &= owner_bit < 0 ? 0 : (1U << owner_bit);
        if ((S.channel_open_mask & 1) && primary < 128)
            big_macid_gate(primary, 0, 0, 36);
        for (i = 0; i < S.faw.count; i++)
            if (S.channel_open_mask & (1U << (i + 1)))
                big_macid_gate(S.faw.macid[i], 0, 0, 36);
    }
    return ret;
}
u32 u2_nan_discovery_channel(void *channel, u32 complete)
{
    u8 data[15];
    /* Keep the accepted reservation's channel. Discovery beacons use that
     * current channel so a new peer can synchronize before negotiating NDP. */
    if (data_reserved(data)) return 0;
    return u2_nan_iqc_channel(channel, complete);
}
/* Native preparation is shared, while adjacent owners can both have a begin
 * pending. Each owner retains the descriptor belonging to its timer. */
u32 u2_nan_schedule(u32 unused, u32 timer, u32 low, u32 high,
                    u32 callback, u32 argument)
{
    u32 owner, slot = 0, ret;
    struct u2_faw_pending previous;
    struct u2_faw_prepared prepared;
    (void)unused; (void)timer;
    if (callback == 0xb89ac531) {
        owner = big_faw_macid();
        big_faw_capture(&prepared);
        /* Before a data station exists, local receive availability shares
         * the primary MACID but needs its own pending channel descriptor.
         * The second member slot becomes the data station slot on handoff. */
        slot = prepared.channel[0] > 14 ? 2 : 0;
        if (owner != big_schedule_macid()) {
            for (slot = 0; slot < S.faw.count; slot++)
                if (S.faw.macid[slot] == owner) break;
            if (slot == S.faw.count) return 0;
            slot++;
        }
        copy((u8 *)&previous, (const u8 *)&S.pending_faw[slot], sizeof(previous));
        copy((u8 *)&S.pending_faw[slot].prepared,
             (const u8 *)&prepared, sizeof(prepared));
        S.pending_faw[slot].generation = ++S.faw_generation;
        argument = (S.faw_generation << 2) | slot;
    }
    ret = big_timer_schedule(0, 5, low, high, callback, argument);
    if (!ret && callback == 0xb89ac531)
        copy((u8 *)&S.pending_faw[slot], (const u8 *)&previous, sizeof(previous));
    return ret;
}
u32 u2_nan_faw_begin(u32 token)
{
    u32 flags = big_enter(), ret = 0, slot = token & 3;
    u32 primary = big_schedule_macid();
    u8 channel[15];
    if (slot < 3 && token && primary < 128 &&
        S.pending_faw[slot].generation == (token >> 2)) {
        if ((!slot && S.pending_faw[slot].prepared.macid == primary) ||
            (slot == 2 && S.pending_faw[slot].prepared.macid == primary &&
             big_data_channel(channel) == primary) ||
            (slot && slot <= S.faw.count &&
             S.pending_faw[slot].prepared.macid == S.faw.macid[slot-1])) {
            big_faw_restore(&S.pending_faw[slot].prepared);
            ret = big_native_faw_begin();
        } else if (S.active) {
            /* This timer was consumed after its owner was handed off. The
             * native begin also advances the NAN event chain; skipping the
             * obsolete window must still select and arm the next event. */
            S.pending_faw[slot].generation = 0;
            ret = big_faw_continue();
            big_window_trace(249, token, ret);
        }
    }
    big_leave(flags);
    return ret;
}
/* The native end deadline is in the free-running domain, not DW TSF. */
static void window_restore(void)
{
    if (S.tail_active) big_aggregate_time(S.tail_saved);
    S.tail_active = 0;
    S.tail_generation++;
}
void u2_nan_window_tail(u32 token)
{
    u32 flags = big_enter();
    unsigned long long end = ((unsigned long long)S.tail_high << 32) | S.tail_low;
    unsigned long long now;
    u32 units;
    int bit = channel_owner_bit(S.tail_macid);
    if (!S.active || !S.tail_active || token != S.tail_generation ||
        bit <= 0 || !(S.channel_open_mask & (1U << bit))) goto out;
    now = big_free_time();
    if (now >= end) goto out;
    units = (u32)(end - now) >> 5;
    if (!units) units = 1;
    if (units < S.tail_saved) big_aggregate_time(units);
    S.tail_active = 2;
    S.tail_events++;
out:
    big_leave(flags);
}
void u2_nan_window_arm(u32 macid, u32 low, u32 high)
{
    unsigned long long end = ((unsigned long long)high << 32) | low;
    unsigned long long now;
    if (!S.active || S.tail_owner != macid + 1 || channel_owner_bit(macid) <= 0) return;
    /* Adjacent records share the longest outstanding availability. */
    if (S.tail_active && S.tail_macid == macid &&
        end <= (((unsigned long long)S.tail_high << 32) | S.tail_low)) return;
    window_restore();
    now = big_free_time();
    if (end <= now) return;
    S.tail_low = low; S.tail_high = high; S.tail_macid = macid;
    S.tail_saved = big_aggregate_time(0);
    if (!S.tail_saved) return;
    S.tail_active = 1;
    /* Keep the native aggregation budget until the final 2048 us. The
     * native end callback owns gate closure, including in-flight retries. */
    if (end - now <= 2048) {
        u2_nan_window_tail(S.tail_generation);
        return;
    }
    end -= 2048;
    if (!big_timer_schedule(0,5,(u32)end,(u32)(end >> 32),
            (u32)u2_nan_window_tail,S.tail_generation)) window_restore();
}
static void copy(u8 *d, const u8 *s, u32 n)
{
    while (n--) *d++ = *s++;
}
static int equal(const u8 *a, const u8 *b, u32 n)
{
    while (n--) if (*a++ != *b++) return 0;
    return 1;
}
static void clear(void)
{
    volatile u8 *p = (volatile u8 *)&S;
    u32 n = sizeof(S), i, generation = S.tail_generation + 1;
    window_restore();
    for (i = 0; i < S.faw.count; i++)
        big_macid_gate(S.faw.macid[i], 0, 0, 36);
    while (n--) *p++ = 0;
    S.tail_generation = generation;
}
/* Called at the two native FAW gate writes inside the callback critical
 * section. Membership changes use the same section, including mid-window. */
u32 u2_nan_faw_gate(u32 macid, u32 pause, u32 sleep, u32 reason)
{
    u32 ret;
    if (S.active && S.ending) macid = S.ending_macid;
    if (S.active && macid == big_schedule_macid()) {
        /* Adjacent records can begin before the preceding end callback.
         * Keep their common MACID gate open until the last window ends. */
        if (!sleep && !pause) S.faw_windows++;
        else if (S.faw_windows) S.faw_windows--;
        S.faw.open = !!S.faw_windows;
        if ((sleep || pause) && S.faw.open) return 0;
        sleep = pause = S.rfk_hold || !S.faw.open;
        ret = channel_gate(macid, pause, sleep, reason);
        return ret;
    }
    if (S.active) {
        int bit = channel_owner_bit(macid);
        if (bit > 0) {
            u8 *windows = &S.data_windows[bit - 1];
            if (!pause && !sleep) (*windows)++;
            else {
                if (*windows) (*windows)--;
                if (*windows) return 0;
            }
        }
    }
    if (S.rfk_hold) sleep = pause = 1;
    return channel_gate(macid, pause, sleep, reason);
}
void u2_nan_faw_end(u32 macid)
{
    /* A following window can begin before this callback runs. */
    S.ending_macid = macid;
    S.ending = 1;
    big_native_faw_end();
    if (S.tail_active && S.tail_macid == macid) {
        int bit = channel_owner_bit(macid);
        if (bit <= 0 || !S.data_windows[bit - 1]) window_restore();
    }
    S.ending = S.ending_macid = 0;
    /* Our committed receive availability covers the data channel after the
     * authentication slot. Peer availability only controls its TX gate. */
    if (S.active && macid == big_schedule_macid() && !S.faw_windows) {
        u8 channel[15];
        u32 flags = big_enter();
        if (data_reserved(channel)) u2_nan_iqc_channel(channel, 0);
        big_leave(flags);
    }
}
u32 u2_nan_faw_channel(void *channel, u32 complete)
{
    u32 next = big_faw_macid(), primary = big_schedule_macid(), i;
    u8 *ch = channel;
    /* Stop the previous owner before the radio changes channels. The
     * next owner's native begin callback opens it after the switch. */
    if (S.active) {
        /* U2's switch descriptor carries its RF band at byte 6. NAN's
         * availability record supplies channels and width separately. */
        ch[6] = ch[0] > 14 ? 1 : 0;
        if (primary < 128 && primary != next)
            big_macid_gate(primary, 1, 1, 36);
        for (i = 0; i < S.faw.count; i++)
            if (S.faw.macid[i] != next)
                big_macid_gate(S.faw.macid[i], 1, 1, 36);
    }
    return u2_nan_iqc_channel(channel, complete);
}
static int slot(u32 port)
{
    if (port == S.ports[0]) return 0;
    if (port == S.ports[1]) return 1;
    return -1;
}
static u32 rfk_grant(void)
{
    u32 i, ret;
    window_restore();
    S.iqc_cached = 0;
    S.rfk_hold = 1;
    S.rfk_wait = 0;
    big_native_gate(0, S.ports[0], 0);
    big_native_gate(0, S.ports[1], 0);
    i = big_schedule_macid();
    if (i < 128) big_macid_gate(i, 1, 1, 36);
    for (i = 0; i < S.faw.count; i++)
        big_macid_gate(S.faw.macid[i], 1, 1, 36);
    S.iqc[0].valid = S.iqc[1].valid = 0;
    ret = big_rfk_grant(S.rfk_seq);
    if (ret) S.rfk_hold = 0;
    return ret;
}
static u32 command(u8 *h)
{
    u8 *p = h + 8;
    u32 i, ret = 4, flags = big_enter();
    if (((h[4] | ((u32)h[5] << 8)) & 0x3fff) != 40) goto out;
    if (p[0] == 0 || p[0] == 5) {
        ret = 0;
        goto out;
    } else if (p[0] == 7) {
        S.snapshot.active = S.active;
        S.snapshot.pending = S.pending;
        if (!S.active || S.rfk_wait || S.rfk_hold) goto out;
        S.rfk_seq = h[3];
        S.rfk_wait = 1;
        if (!S.pending) {
            ret = rfk_grant();
            goto out;
        }
    } else if (p[0] == 8) {
        S.rfk_hold = S.rfk_wait = 0;
    } else if (p[0] == 6) {
        struct u2_iqc_channel *iqc;
        if (!S.active || p[1] > 1 || !p[2] || p[3] > 2 || p[4] > 1)
            goto out;
        S.iqc_cached = 0;
        iqc = &S.iqc[p[1]];
        iqc->center = p[2]; iqc->width = p[3]; iqc->page = p[4];
        copy((u8 *)iqc->enable, p + 8, 24);
        iqc->valid = 1;
        iqc->tssi[0].valid = iqc->tssi[1].valid = 0;
    } else if (p[0] == 9) {
        struct u2_iqc_channel *iqc;
        struct u2_tssi_path *tssi;
        if (!S.active || p[1] > 1 || p[4] > 1 || p[5] > 1)
            goto out;
        if (!S.rfk_hold) { ret = 5; goto out; }
        S.iqc_cached = 0;
        iqc = &S.iqc[p[1]];
        if (!iqc->valid || iqc->center != p[2] || iqc->width != p[3]) {
            ret = 6;
            goto out;
        }
        tssi = &iqc->tssi[p[4]];
        copy((u8 *)tssi->alignment, p + 8, 24);
        tssi->enabled = p[5]; tssi->offset = p[6]; tssi->valid = 1;
    } else if (p[0] == 1) {
        if (S.active || (p[1] != 6 && p[1] != 7) || (p[2] & 1) ||
            p[14] > 4 || p[15] > 4 || p[14] == p[15]) goto out;
        /* Quiesce both native beacon ports before publishing the key. */
        big_native_gate(0, p[14], 0);
        big_native_gate(0, p[15], 0);
        if (big_queue_lock()) goto out;
        clear();
        S.index = p[1];
        copy(S.nmi, p + 2, 6);
        copy(S.ipn, p + 8, 6);
        copy(S.ports, p + 14, 2);
        copy(S.key, p + 16, sizeof(S.key));
        S.channel_center = 6;
        S.channel_ready = 1;
        S.active = 1;
        big_queue_unlock();
    } else if (p[0] == 2) {
        if (!S.active || S.index != p[1] || !equal(S.nmi, p + 2, 6)) goto out;
        S.snapshot.seq = h[3];
        S.snapshot.index = S.index;
        for (i = 0; i < 6; i++) {
            S.snapshot.nmi[i] = S.nmi[i];
            S.snapshot.ipn[i] = S.ipn[i];
        }
        S.snapshot.pending = S.pending;
        S.snapshot.active = S.active;
        S.snapshot.prepared = S.prepared;
        S.snapshot.completed = S.completed;
        S.snapshot.skipped = S.skipped;
        S.snapshot.error = S.error;
        S.snapshot.calculation_us = S.calculation_us;
        copy(S.snapshot.frames[0], S.frame[0], sizeof(S.frame));
    } else if (p[0] == 3) {
        u32 primary = big_schedule_macid();
        u8 open_mask = S.channel_open_mask & 1, windows[2] = {0,0};
        if (!S.active || S.index != p[1] || !equal(S.nmi, p + 2, 6) ||
            primary >= 128 || p[8] > 2) goto out;
        for (i = 0; i < p[8]; i++)
            if (p[9+i] >= 128 || p[9+i] == primary ||
                (i && p[9] == p[10])) goto out;
        if (p[11]) {
            for (i = 0; i < p[8]; i++) if (p[9+i] + 1 == p[11]) break;
            if (i == p[8]) goto out;
        }
        if (p[8] == S.faw.count && p[11] == S.tail_owner &&
            (!p[8] || equal(p + 9, S.faw.macid, p[8]))) { ret = 0; goto out; }
        /* Retained members keep their own record's current gate state.
         * Newly registered data MACIDs wait for their first FAW callback. */
        for (i = 0; i < S.faw.count; i++) {
            u32 j;
            for (j = 0; j < p[8]; j++) if (S.faw.macid[i] == p[9+j]) break;
            if (j == p[8]) big_macid_gate(S.faw.macid[i], 0, 0, 36);
        }
        for (i = 0; i < p[8]; i++) {
            u32 j;
            for (j = 0; j < S.faw.count; j++) if (S.faw.macid[j] == p[9+i]) break;
            if (j == S.faw.count) big_macid_gate(p[9+i], 1, 1, 36);
            else {
                windows[i] = S.data_windows[j];
                if (S.channel_open_mask & (1U << (j + 1))) open_mask |= 1U << (i + 1);
            }
        }
        window_restore();
        S.channel_open_mask = open_mask;
        S.tail_owner = p[11];
        S.data_windows[0] = windows[0]; S.data_windows[1] = windows[1];
        S.faw.count = p[8];
        S.faw.macid[0] = S.faw.macid[1] = 0;
        for (i = 0; i < S.faw.count; i++) {
            S.faw.macid[i] = p[9+i];
        }
    } else if (p[0] == 4) {
        if (!S.active || S.index != p[1] || !equal(S.nmi, p + 2, 6)) goto out;
        copy((u8 *)&S.faw_snapshot, (u8 *)&S.faw, sizeof(S.faw));
    } else goto out;
    ret = 0;
out:
    big_leave(flags);
    return ret;
}
u32 u2_bigtk_dispatch(u8 *h)
{
    u32 ret, flags;
    if (h[1] == 8) return command(h);
    if (h[1] == 0 && big_stopped()) {
        flags = big_enter();
        clear();
        big_leave(flags);
    }
    ret = big_native_dispatch(h);
    if (h[1] == 2 && !h[14] && !h[15] && !ret) {
        big_window_trace(250,S.iqc_skipped,S.tail_events);
        flags = big_enter();
        clear();
        big_leave(flags);
    }
    return ret;
}
u32 u2_bigtk_gate(u32 band, u32 port, u32 enable)
{
    if (S.active && !band && slot(port) >= 0 &&
        (S.rfk_hold || S.pending != port + 1))
        enable = 0;
    return big_native_gate(band, port, enable);
}
u32 u2_bigtk_prepare(u32 port, u32 mbssid, u32 band)
{
    u32 ret, flags, i, carry, start;
    int which;
    u8 *base, *frame;
    volatile u8 *wd;
    if (!S.active || band || mbssid || (which = slot(port)) < 0)
        return big_native_prepare(port, mbssid, band);
    flags = big_enter();
    if (S.rfk_hold || S.rfk_wait || S.pending) {
        S.skipped++;
        big_leave(flags);
        return 0;
    }
    big_native_gate(0, port, 0);
    if (S.error == 5) { big_leave(flags); return 0; }
    ret = big_native_prepare(port, mbssid, band);
    if (big_queue_lock()) { S.error = 1; goto out; }
    S.error = big_frame(port, &base, &wd);
    if (S.error) goto unlock;
    frame = S.frame[which];
    if (base == frame) base = (u8 *)S.base[which];
    if (!base || base[0] != 0x80 || !equal(base + 10, S.nmi, 6)) {
        S.error = 4;
        goto unlock;
    }
    S.base[which] = (u32)base;
    /* Reserve before signing; a failed/discarded value is never reused. */
    for (i = 0, carry = 1; i < 6; i++) {
        carry += S.ipn[i];
        S.ipn[i] = carry;
        carry >>= 8;
    }
    if (carry) { S.error = 5; goto unlock; }
    start = big_time();
    copy(frame, base, 63);
    frame[63] = 76;
    frame[64] = 16;
    frame[65] = S.index;
    frame[66] = 0;
    copy(frame + 67, S.ipn, 6);
    {
        struct u2_cmac_key expanded;
        u2_cmac_key(&expanded, S.key);
        u2_beacon_mic(&expanded, frame, 81, frame + 73);
    }
    S.calculation_us = big_time() - start;
    *(volatile u16 *)(wd + 8) = (*(volatile u16 *)(wd + 8) & 0xc000) | 81;
    *(volatile u16 *)(wd + 56) = 81;
    *(volatile u32 *)(wd + 60) = (u32)frame & 0x1fffffff;
    S.pending = port + 1;
    S.prepared++;
unlock:
    big_queue_unlock();
    if (!S.error) big_native_gate(0, port, 1);
out:
    big_leave(flags);
    return ret;
}
void u2_bigtk_done(u32 report)
{
    u32 port = report & 7, flags = big_enter();
    if (S.active && !(report & 0xffff0000) && S.pending == port + 1) {
        big_native_gate(0, port, 0);
        S.pending = 0;
        S.completed++;
        if (S.rfk_wait) rfk_grant();
    }
    big_leave(flags);
}

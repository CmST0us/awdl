/* SPDX-License-Identifier: GPL-2.0 */
#include "u2_bigtk.h"
#include "beacon.h"
struct u2_bigtk_state u2_bigtk_state;
static u8 bases[2][64], descriptors[2][64] __attribute__((aligned(4)));
static u32 enabled[5], locked, clock_us, stopped, queue_error;
static u32 macid_sleep[128], macid_pause[128], dispatch_error;
static struct u2_faw_prepared prepared_faw, executed_faw;
static u32 timer_argument, begin_count, begin_failure, continue_count;
u32 big_faw_continue(void) { continue_count++; return 0; }
static unsigned long long free_clock;
static u32 agg_units=165, tail_token, tail_low, tail_high, tail_failure, iqc_match;
unsigned long long big_free_time(void) { return free_clock; }
u32 big_aggregate_time(u32 units) { u32 old=agg_units;if(units)agg_units=units;return old; }
int big_tssi_matches(const struct u2_iqc_channel *iqc) { (void)iqc;return iqc_match; }
static u32 grant_count, grant_seq, grant_error;
void big_window_trace(u32 marker,u32 skipped,u32 tails) { (void)marker;(void)skipped;(void)tails; }
u32 big_rfk_grant(u32 seq) { grant_count++; grant_seq=seq; return grant_error; }
static struct u2_iqc_channel restored_iqc;
static u32 iqc_calls, iqc_receive_calls;
static u32 receive_owner = 0xffff, receive_calls, receive_primary;
static u32 open_during_switch, restored_while_closed, channel_error;
#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)
#define S u2_bigtk_state
static void cp(u8 *d, const u8 *s, u32 n) { while (n--) *d++ = *s++; }
u32 big_enter(void) { return 0; }
void big_leave(u32 flags) { (void)flags; }
u32 big_stopped(void) { return stopped; }
u32 big_native_dispatch(u8 *h) { (void)h; return dispatch_error; }
u32 big_schedule_macid(void) { return 2; }
u32 big_macid_gate(u32 id, u32 pause, u32 sleep, u32 reason)
{
    if (id >= 128 || reason != 36) return 1;
    macid_sleep[id] = sleep; macid_pause[id] = pause;
    return 0;
}
static u32 active_macid;
u32 big_faw_macid(void) { return active_macid; }
void big_faw_capture(struct u2_faw_prepared *p) { *p=prepared_faw; }
void big_faw_restore(const struct u2_faw_prepared *p) { prepared_faw=*p; }
u32 big_native_faw_begin(void) { executed_faw=prepared_faw; begin_count++; return 0; }
u32 big_timer_schedule(u32 band, u32 timer, u32 low, u32 high,
                       u32 callback, u32 argument)
{
    if (callback == (u32)u2_nan_window_tail) {
        tail_token=argument;tail_low=low;tail_high=high;return !tail_failure;
    }
    if (band || timer!=5 || low!=123 || high!=456 || !callback) return 1;
    if (begin_failure) return 0;
    timer_argument=argument;
    return 1;
}
u32 big_native_faw_channel(void *ch, u32 band) {
    (void)ch; (void)band;
    if (channel_error) return channel_error;
    receive_calls++; receive_primary = ((u8 *)ch)[0];
    if (open_during_switch) {
        u2_nan_faw_gate(3,0,0,36);
        if (!macid_pause[3] || !macid_sleep[3]) return 10;
    }
    for (u32 id=2; id<=4; id++) if (id != active_macid &&
        (id == 2 || (S.faw.count && S.faw.macid[0] == id) ||
         (S.faw.count > 1 && S.faw.macid[1] == id)))
        if (!macid_sleep[id] || !macid_pause[id]) return 9;
    return 0;
}
void big_iqc_restore(const struct u2_iqc_channel *iqc, u32 preserve_tssi) {
    (void)preserve_tssi;
    if (open_during_switch)
        restored_while_closed = macid_pause[3] && macid_sleep[3] && S.channel_switching;
    restored_iqc = *iqc; iqc_calls++; iqc_receive_calls=receive_calls;
}
u32 big_data_channel(u8 *ch) {
    for (u32 i=0;i<15;i++) ch[i]=0;
    ch[0]=161; ch[1]=155; ch[2]=2; ch[6]=1;
    return receive_owner;
}
static u32 native_end_macid;
void big_native_faw_end(void) { u2_nan_faw_gate(native_end_macid, 1, 1, 36); }
u32 big_native_prepare(u32 p, u32 m, u32 b) { (void)p; (void)m; (void)b; return 7; }
u32 big_native_gate(u32 b, u32 p, u32 e) { (void)b; enabled[p] = e; return 1; }
u32 big_queue_lock(void) { if (queue_error) return 1; locked++; return 0; }
void big_queue_unlock(void) { locked--; }
u32 big_time(void) { return ++clock_us; }
u32 big_frame(u32 p, u8 **base, volatile u8 **wd)
{
    unsigned i = p == 2;
    *base = S.prepared ? S.frame[i] : bases[i];
    if (!S.base[i]) *base = bases[i];
    *wd = descriptors[i];
    return 0;
}
int test_bigtk(void)
{
    u8 h[40] = {0}, old[81];
    u8 channel[15] = {0};
    u32 i;
    h[1] = 8; h[3] = 19; h[4] = 40;
    CHECK(!u2_bigtk_dispatch(h) && !S.active && !S.index);
    h[8] = 5;
    CHECK(!u2_bigtk_dispatch(h) && !S.active && !S.faw.count);
    h[8] = 1; h[9] = 6;
    cp(h + 10, beacon + 10, 6);
    h[16] = 255; h[17] = 255; h[18] = 255;
    h[22] = 0; h[23] = 2;
    cp(h + 24, key_bytes, 16);
    cp(bases[0], beacon, 63); cp(bases[1], beacon, 63);
    CHECK(!u2_bigtk_dispatch(h) && S.active && !locked);
    h[8] = 5;
    CHECK(!u2_bigtk_dispatch(h) && S.active && S.ipn[0] == 255);
    h[8] = 0;
    CHECK(!u2_bigtk_dispatch(h) && S.active && S.index == 6 && S.ipn[0] == 255);
    h[8] = 1;
    CHECK(u2_bigtk_dispatch(h) == 4 && S.ipn[0] == 255);
    h[8] = 3; h[16] = 1; h[17] = 3; h[19] = 0;
    CHECK(!u2_bigtk_dispatch(h) && S.faw.count == 1);
    CHECK(macid_sleep[3] && macid_pause[3] && !S.faw.open);
    /* Local receive availability and authentication precede peer creation. */
    S.faw.count=0; receive_owner=2; active_macid=2;
    prepared_faw=(struct u2_faw_prepared){.macid=2,.record=1,
        .duration={16384,0},.channel={6,6,0}};
    CHECK(u2_nan_schedule(0,5,123,456,0xb89ac531,0)==1);
    u32 local_auth=timer_argument;
    prepared_faw=(struct u2_faw_prepared){.macid=2,.record=2,
        .duration={491520,0},.channel={161,155,2}};
    CHECK(u2_nan_schedule(0,5,123,456,0xb89ac531,0)==1);
    u32 local_data=timer_argument;
    CHECK(local_auth!=local_data);
    /* START installs availability before the local BIGTK owner. */
    S.active=0;
    CHECK(!u2_nan_faw_begin(local_auth) && executed_faw.channel[0]==6);
    S.active=1;
    CHECK(!u2_nan_faw_begin(local_data) && executed_faw.channel[0]==161);
    u32 local_calls=receive_calls;
    CHECK(!u2_nan_discovery_channel(channel,0) && receive_calls==local_calls);
    receive_owner=3;
    u32 local_begins=begin_count;
    CHECK(!u2_nan_faw_begin(local_data) && begin_count==local_begins && continue_count==1);
    CHECK(!u2_nan_faw_begin(local_data) && continue_count==1);
    receive_owner=0xffff; S.faw.count=1; begin_count=0;
    /* Native limits remain intact until the end budget is actually smaller. */
    free_clock=0xffff0000ULL;
    u2_nan_window_arm(2,0x1000,1);CHECK(!S.tail_active && agg_units==165);
    u2_nan_window_arm(3,0x1000,1);CHECK(!S.tail_active && agg_units==165);
    h[19]=4;CHECK(!u2_bigtk_dispatch(h) && S.tail_owner==4);
    CHECK(!u2_nan_faw_gate(3,0,0,36));
    u2_nan_window_arm(3,0x1000,1);
    CHECK(S.tail_active==1 && agg_units==165 && tail_low==0x800 && tail_high==1);
    u32 saved_tail=tail_token;
    CHECK(!u2_bigtk_dispatch(h) && S.tail_generation==saved_tail && S.tail_active);
    free_clock=0x100000800ULL;u2_nan_window_tail(tail_token);
    CHECK(S.tail_active==2 && agg_units==64 && !macid_pause[3]);
    /* A new window begins before the old one's end; the old end cannot close it. */
    CHECK(!u2_nan_faw_gate(3,0,0,36) && S.data_windows[0]==2);
    u2_nan_window_arm(3,0x10000,1);
    CHECK(agg_units==165);
    u32 next_tail=tail_token;
    u2_nan_faw_end(3);CHECK(S.tail_active && !macid_pause[3] && S.data_windows[0]==1);
    u2_nan_window_tail(saved_tail);CHECK(S.tail_generation==next_tail && agg_units==165);
    u2_nan_window_arm(3,0x8000,1);CHECK(S.tail_generation==next_tail);
    u2_nan_faw_end(3);CHECK(!S.tail_active && agg_units==165 && macid_pause[3]);
    /* Short windows apply their actual remaining time immediately. */
    CHECK(!u2_nan_faw_gate(3,0,0,36));
    u2_nan_window_arm(3,0xc00,1);CHECK(S.tail_active==2 && agg_units==32);
    u2_nan_faw_end(3);CHECK(!S.tail_active && agg_units==165);
    tail_failure=1;u2_nan_window_arm(3,0x10000,1);
    CHECK(!S.tail_active && agg_units==165);tail_failure=0;
    /* A pending authentication begin must retain its descriptor when the
     * native selector prepares the immediately following data window. */
    active_macid=2;
    prepared_faw=(struct u2_faw_prepared){.macid=2,.record=1,
        .duration={16384,0},.channel={6,6,0}};
    CHECK(u2_nan_schedule(0,5,123,456,0xb89ac531,0)==1);
    u32 auth_token=timer_argument;
    active_macid=3;
    receive_owner=3;
    prepared_faw=(struct u2_faw_prepared){.macid=3,.record=2,
        .duration={98304,0},.channel={161,155,2}};
    CHECK(u2_nan_schedule(0,5,123,456,0xb89ac531,0)==1);
    u32 data_token=timer_argument;
    begin_failure=1;prepared_faw.duration[0]=42;
    CHECK(!u2_nan_schedule(0,5,123,456,0xb89ac531,0));
    CHECK(S.pending_faw[1].generation==(data_token>>2) &&
          S.pending_faw[1].prepared.duration[0]==98304);
    begin_failure=0;
    CHECK(!u2_nan_faw_begin(auth_token) && begin_count==1);
    CHECK(executed_faw.macid==2 && executed_faw.record==1 &&
          executed_faw.channel[0]==6 && executed_faw.duration[0]==16384);
    CHECK(!u2_nan_faw_begin(data_token) && begin_count==2 && continue_count==1);
    CHECK(executed_faw.macid==3 && executed_faw.record==2 &&
          executed_faw.channel[0]==161 && executed_faw.duration[0]==98304);
    prepared_faw.duration[0]=327680;
    CHECK(u2_nan_schedule(0,5,123,456,0xb89ac531,0)==1);
    CHECK(!u2_nan_faw_begin(data_token) && begin_count==2 && continue_count==1);
    CHECK(!u2_nan_faw_begin(timer_argument) && begin_count==3 &&
          executed_faw.duration[0]==327680);
    active_macid=127;
    CHECK(!u2_nan_schedule(0,5,123,456,0xb89ac531,0));
    active_macid=3;
    S.faw.count=0;
    CHECK(!u2_nan_faw_begin(timer_argument) && begin_count==3 && continue_count==2);
    CHECK(!u2_nan_faw_begin(timer_argument) && continue_count==2);
    active_macid=2; prepared_faw.macid=2; prepared_faw.channel[0]=161;
    CHECK(u2_nan_schedule(0,5,123,456,0xb89ac531,0)==1);
    S.active=0;
    CHECK(!u2_nan_faw_begin(timer_argument) && continue_count==2);
    S.active=1;
    S.faw.count=1;
    CHECK(!u2_nan_faw_gate(2, 0, 0, 36));
    CHECK(S.faw.open && !macid_sleep[2] && macid_sleep[3] && macid_pause[3]);
    CHECK(!u2_nan_faw_gate(3, 0, 0, 36) && !macid_sleep[3]);
    h[16] = 2; h[18] = 4;
    CHECK(!u2_bigtk_dispatch(h) && S.faw.count == 2 && !macid_sleep[3] && macid_sleep[4]);
    h[18] = 3;
    CHECK(u2_bigtk_dispatch(h) == 4 && S.faw.macid[1] == 4);
    h[18] = 2;
    CHECK(u2_bigtk_dispatch(h) == 4 && S.faw.macid[1] == 4);
    CHECK(!u2_nan_faw_gate(2, 1, 1, 36));
    CHECK(!S.faw.open && !macid_pause[3] && macid_pause[4]);
    /* Native timer ordering at a shared boundary: next start, old end. */
    CHECK(!u2_nan_faw_gate(2, 0, 0, 36));
    CHECK(!u2_nan_faw_gate(2, 0, 0, 36) && S.faw_windows == 2);
    CHECK(!u2_nan_faw_gate(2, 1, 1, 36));
    CHECK(S.faw_windows == 1 && S.faw.open && !macid_sleep[2] &&
          !macid_pause[3] && macid_pause[4]);
    CHECK(!u2_nan_faw_gate(2, 1, 1, 36));
    CHECK(!S.faw_windows && !S.faw.open && macid_sleep[2] && !macid_pause[3]);
    CHECK(!u2_nan_faw_gate(3, 1, 0, 36) && macid_pause[3] && !macid_sleep[3]);
    CHECK(!u2_nan_faw_gate(3, 0, 1, 36) && !macid_pause[3] && macid_sleep[3]);
    CHECK(!u2_nan_faw_gate(3, 1, 1, 36) && macid_pause[3]);
    h[8] = 4;
    CHECK(!u2_bigtk_dispatch(h) && S.faw_snapshot.count == 2 &&
          S.faw_snapshot.macid[1] == 4 && !S.faw_snapshot.open);
    h[8] = 3; h[16] = 1;
    CHECK(!u2_bigtk_dispatch(h) && !macid_pause[4] && macid_pause[3]);
    CHECK(!u2_nan_faw_gate(5, 0, 0, 36) && !S.faw.open && macid_pause[3]);
    CHECK(!u2_nan_faw_gate(2, 0, 0, 36));
    CHECK(!u2_nan_faw_gate(3, 0, 0, 36));
    active_macid=3;
    channel[0] = 161; channel[1] = 155; channel[2] = 2;
    CHECK(!u2_nan_faw_channel(channel,0) && macid_pause[2] && !macid_pause[3]);
    CHECK(channel[0] == 161 && channel[1] == 155 && channel[2] == 2 && channel[6] == 1);
    native_end_macid = 3;
    u2_nan_faw_end(2);
    CHECK(macid_pause[2] && !macid_pause[3] && !S.faw_windows);
    active_macid=2;
    channel[0] = channel[1] = 6; channel[2] = 0;
    CHECK(!u2_nan_faw_channel(channel,0) && macid_pause[3]);
    CHECK(channel[6] == 0);
    u2_nan_faw_end(3);
    CHECK(macid_pause[3] && !S.ending && !S.ending_macid);
    receive_owner=3;
    CHECK(!u2_nan_faw_gate(2,0,0,36));
    u32 before_receive=receive_calls;
    u2_nan_faw_end(2);
    CHECK(receive_calls==before_receive+1 && receive_primary==161 &&
          macid_pause[2] && macid_pause[3]);
    /* Removed records and nonmember records cannot retain radio residence. */
    receive_owner=0xffff;
    u2_nan_faw_end(2);
    CHECK(receive_calls==before_receive+1);
    receive_owner=4;
    u2_nan_faw_end(2);
    CHECK(receive_calls==before_receive+1);
    receive_owner=3;
    before_receive=receive_calls;
    CHECK(!u2_nan_discovery_channel(channel,0) && receive_calls==before_receive);
    receive_owner=0xffff;
    CHECK(!u2_nan_discovery_channel(channel,0) && receive_calls==before_receive+1);
    {
        u8 command[40] = {0};
        u32 calls = iqc_calls;
        command[1]=8; command[4]=40; command[8]=7;
        CHECK(!u2_bigtk_dispatch(command) && S.rfk_hold);
        CHECK(S.snapshot.active == 1 && S.snapshot.pending == 0);
        before_receive=receive_calls;
        CHECK(!u2_nan_iqc_channel(channel,0) && receive_calls==before_receive);
        CHECK(!u2_nan_faw_gate(3,0,0,36) && macid_pause[3]);
        CHECK(!u2_bigtk_prepare(0,0,0) && !S.pending && !enabled[0]);
        command[8]=6; command[9]=0; command[10]=155;
        command[11]=2; command[12]=1;
        for (i=0;i<24;i++) command[16+i]=i+1;
        CHECK(!u2_bigtk_dispatch(command));
        channel[0]=161;channel[1]=155;channel[2]=2;
        CHECK(!u2_nan_iqc_channel(channel,0) && iqc_calls==calls);
        command[9]=1;command[10]=6;command[11]=0;command[12]=0;
        for (i=0;i<24;i++) command[16+i]=i+31;
        CHECK(!u2_bigtk_dispatch(command));
        command[8]=9; command[9]=0; command[10]=155; command[11]=2;
        command[12]=0; command[13]=1; command[14]=0x35;
        for (i=0;i<16;i++) command[16+i]=i+61;
        command[32]=0xf1;command[33]=3;command[34]=command[35]=0;
        command[36]=5;command[37]=command[38]=command[39]=0;
        CHECK(!u2_bigtk_dispatch(command) && S.iqc[0].tssi[0].valid &&
              !S.iqc[0].tssi[1].valid && S.iqc[0].tssi[0].offset==0x35 &&
              S.iqc[0].tssi[0].cck_de==0x3f1 && S.iqc[0].tssi[0].ofdm_de==5);
        command[12]=1;
        CHECK(!u2_bigtk_dispatch(command) && S.iqc[0].tssi[1].valid);
        command[10]=6;
        CHECK(u2_bigtk_dispatch(command)==6);
        command[10]=155; command[12]=2;
        CHECK(u2_bigtk_dispatch(command)==4);
        command[8]=8;
        CHECK(!u2_bigtk_dispatch(command) && !S.rfk_hold);
        command[8]=9; command[12]=0;
        CHECK(u2_bigtk_dispatch(command)==5);
        command[8]=6;
        open_during_switch=1;
        receive_owner=3;
        CHECK(!u2_nan_iqc_channel(channel,0) && iqc_calls==++calls &&
              iqc_receive_calls==receive_calls && restored_iqc.page==1 &&
              restored_iqc.tx[0]==0x0c0b0a09 && restored_iqc.rx[1]==0x18171615 &&
              restored_iqc.tssi[1].alignment[0]==0x403f3e3d &&
              restored_iqc.tssi[1].cck_de==0x3f1 && restored_iqc.tssi[1].ofdm_de==5);
        CHECK(restored_while_closed && !S.channel_switching && S.channel_ready &&
              !macid_pause[3] && S.channel_deferred);
        open_during_switch=0;
        channel_error=5;
        CHECK(u2_nan_iqc_channel(channel,0)==5 && !S.channel_ready &&
              macid_pause[3] && iqc_calls==calls && !S.iqc_cached);
        channel_error=0;
        CHECK(!u2_nan_iqc_channel(channel,0) && S.channel_ready && iqc_calls==++calls);
        iqc_match=1;
        CHECK(!u2_nan_iqc_channel(channel,0) && iqc_calls==++calls && S.iqc_skipped==1);
        iqc_match=0;
        CHECK(!u2_nan_iqc_channel(channel,0) && iqc_calls==++calls);

        channel[0]=channel[1]=6;channel[2]=0;
        CHECK(!u2_nan_iqc_channel(channel,0) && iqc_calls==++calls &&
              iqc_receive_calls==receive_calls && restored_iqc.page==0 &&
              restored_iqc.enable[0]==0x2221201f);
        channel[1]=155; channel[2]=1;
        CHECK(!u2_nan_iqc_channel(channel,0) && iqc_calls==calls);
        CHECK(!S.channel_ready && macid_pause[3]);
        command[10]=6; command[11]=0; command[12]=0;
        command[9]=2;
        CHECK(u2_bigtk_dispatch(command)==4);
        command[8]=7;
        CHECK(!u2_bigtk_dispatch(command) && !S.iqc[0].valid && !S.iqc[1].valid);
        channel[1]=6; channel[2]=0;
        CHECK(!u2_nan_iqc_channel(channel,0) && iqc_calls==calls);
        command[8]=8;
        CHECK(!u2_bigtk_dispatch(command) && !S.rfk_hold);
    }
    receive_owner=0xffff;
    u2_bigtk_prepare(0, 0, 0);
    CHECK(S.pending == 1 && S.prepared == 1 && enabled[0] && !locked);
    {
        u8 command[40] = {0};
        command[1]=8; command[4]=40; command[8]=7;
        CHECK(!u2_bigtk_dispatch(command) && S.rfk_wait && !S.rfk_hold && enabled[0]);
        command[8]=8; CHECK(!u2_bigtk_dispatch(command) && !S.rfk_wait);
        command[8]=7;
        CHECK(S.snapshot.active == 1 && S.snapshot.pending == 1);
        S.active = 0;
        CHECK(u2_bigtk_dispatch(command)==4);
        CHECK(S.snapshot.active == 0 && S.snapshot.pending == 1);
        S.active = 1;
    }
    CHECK(!S.ipn[0] && !S.ipn[1] && !S.ipn[2] && S.ipn[3] == 1);
    CHECK(S.frame[0][63] == 76 && S.frame[0][65] == 6);
    for (i=0;i<8;i++) CHECK(S.frame[0][73+i] == expected_mic[i]);
    CHECK(*(u16 *)(descriptors[0]+8) == 81 && *(u16 *)(descriptors[0]+56) == 81);
    cp(old, S.frame[0], 81);
    bases[0][59] ^= 4;
    u2_bigtk_prepare(2, 0, 0);
    u2_bigtk_gate(0, 2, 1);
    CHECK(S.skipped == 2 && !enabled[2] && S.prepared == 1);
    for (i=0;i<81;i++) CHECK(old[i] == S.frame[0][i]);
    u2_bigtk_done(2);
    CHECK(S.pending == 1);
    u2_bigtk_done(0);
    CHECK(!S.pending && S.completed == 1 && !enabled[0]);
    u2_bigtk_gate(0, 0, 1);
    CHECK(!enabled[0]);
    receive_owner=3;
    before_receive=receive_calls;
    CHECK(!u2_nan_discovery_channel(channel,0) && receive_calls==before_receive);
    u2_bigtk_prepare(2, 0, 0);
    CHECK(receive_calls==before_receive);
    receive_owner=0xffff;
    CHECK(S.pending == 3 && S.ipn[0] == 1 && S.prepared == 2 && enabled[2]);
    h[8] = 2;
    CHECK(!u2_bigtk_dispatch(h) && S.snapshot.seq == 19);
    CHECK(S.snapshot.ipn[0] == 1 && S.snapshot.pending == 3 && S.snapshot.completed == 1);
    u2_bigtk_done(0x102);
    CHECK(!S.pending && S.completed == 2 && !enabled[2]);
    queue_error = 1;
    u2_bigtk_prepare(0, 0, 0);
    CHECK(S.error == 1 && !S.pending && !enabled[0] && S.ipn[0] == 1);
    queue_error = 0;
    u2_bigtk_prepare(0, 0, 0);
    CHECK(!S.error && S.pending == 1 && S.frame[0][59] == bases[0][59]);
    CHECK(S.snapshot.ipn[0] == 1 && S.ipn[0] == 2);
    u2_bigtk_done(0);
    for (i=0;i<6;i++) S.ipn[i] = 255;
    u2_bigtk_prepare(0, 0, 0);
    CHECK(S.error == 5 && !S.pending && !enabled[0]);
    u2_bigtk_prepare(0, 0, 0);
    CHECK(S.error == 5 && !S.pending && !S.ipn[0]);
    S.iqc[0].valid = S.iqc[1].valid = 1;
    S.rfk_hold = 1;
    h[1] = 2; h[14] = 0; h[15] = 0;
    dispatch_error = 4;
    CHECK(u2_bigtk_dispatch(h) == 4 && S.faw.count == 1 && macid_pause[3] && S.iqc[0].valid && S.iqc[1].valid);
    dispatch_error = 0;
    CHECK(!u2_bigtk_dispatch(h));
    CHECK(!macid_sleep[3] && !macid_pause[3] && !macid_pause[4]);
    for (i=0;i<sizeof(S);i++)
        if (i<__builtin_offsetof(struct u2_bigtk_state,tail_generation) ||
            i>=__builtin_offsetof(struct u2_bigtk_state,tail_generation)+4)
            CHECK(!((u8 *)&S)[i]);
    u2_nan_window_tail(saved_tail);CHECK(!S.tail_active && agg_units==165);
    {
        u8 command[40]={0};
        u32 grants=grant_count;
        command[1]=8;command[3]=91;command[4]=40;command[8]=7;
        S.active=1;S.ports[0]=0;S.ports[1]=2;S.pending=3;enabled[2]=1;
        CHECK(!u2_bigtk_dispatch(command) && S.rfk_wait && !S.rfk_hold);
        CHECK(grant_count==grants && enabled[2]);
        u2_bigtk_done(0);
        CHECK(S.rfk_wait && S.pending==3 && grant_count==grants);
        u2_bigtk_done(0x102);
        CHECK(!S.rfk_wait && S.rfk_hold && !S.pending && !enabled[2]);
        CHECK(grant_count==grants+1 && grant_seq==91);
        command[8]=8;CHECK(!u2_bigtk_dispatch(command));
        S.pending=1;enabled[0]=1;command[8]=7;
        CHECK(!u2_bigtk_dispatch(command));
        command[8]=8;CHECK(!u2_bigtk_dispatch(command));
        u2_bigtk_done(0);
        CHECK(!S.rfk_hold && grant_count==grants+1);
        grant_error=4;command[8]=7;
        CHECK(u2_bigtk_dispatch(command)==4 && !S.rfk_hold && !S.rfk_wait);
        grant_error=0;S.pending=3;command[8]=7;
        CHECK(!u2_bigtk_dispatch(command));
        h[1]=2;CHECK(!u2_bigtk_dispatch(h));
        u2_bigtk_done(2);
        CHECK(!S.rfk_hold && !S.rfk_wait && grant_count==grants+2);
    }
    return 0;
}

/* SPDX-License-Identifier: GPL-2.0 */
#include "u2_bigtk.h"
#define R32(a) (*(volatile u32 *)(a))
#define R8(a) (*(volatile u8 *)(a))
extern int big_queued(u32 band, u32 pktid);
unsigned long long big_free_time(void);
void big_window_trace(u32 marker,u32 first,u32 second);
u32 big_faw_macid(void) { return R8(0xb8e16aa8); }
u32 big_data_channel(u8 *channel)
{
    u32 valid = R32(0xb8e16ae0) >> 16, i, j;
    for (i = 0; i < 5; i++) {
        u32 record = 0xb8e16ae4 + 108*i;
        if (!(valid & (1U << i)) || R8(record) != 2 || R8(record+3) <= 14)
            continue;
        for (j = 0; j < 15; j++) channel[j] = 0;
        channel[0] = R8(record+3);
        channel[1] = R8(record+1);
        channel[2] = R8(record+2);
        channel[6] = 1;
        return *(volatile u16 *)(record+104);
    }
    return 0xffff;
}
void big_faw_capture(struct u2_faw_prepared *p)
{
    u32 i;
    p->duration[0] = R32(0xb8e16aa0);
    p->duration[1] = R32(0xb8e16aa4);
    p->macid = *(volatile u16 *)0xb8e16aa8;
    for (i = 0; i < 15; i++) p->channel[i] = R8(0xb8e16aac+i);
    p->record = R8(0xb8e16ad4);
}
void big_faw_restore(const struct u2_faw_prepared *p)
{
    u32 i;
    R32(0xb8e16aa0) = p->duration[0];
    R32(0xb8e16aa4) = p->duration[1];
    *(volatile u16 *)0xb8e16aa8 = p->macid;
    for (i = 0; i < 15; i++) R8(0xb8e16aac+i) = p->channel[i];
    R8(0xb8e16ad4) = p->record;
}
u32 big_timer_schedule(u32 band, u32 timer, u32 low, u32 high,
                       u32 callback, u32 argument)
{
    u32 (*schedule)(u32, u32, u32, u32, u32, u32) =
        (void *)R32(0xb8e09bc4);
    return schedule(band, timer, low, high, callback, argument);
}

u32 u2_nan_faw_schedule_end(u32 unused, u32 timer, u32 low, u32 high)
{
    u32 (*schedule)(u32, u32, u32, u32, void (*)(u32), u32) =
        (void *)R32(0xb8e09bc4);
    (void)unused;
    u32 macid = big_faw_macid();
    u32 ret = schedule(0, timer, low, high, u2_nan_faw_end, macid);
    if (++u2_bigtk_state.tail_arms <= 4)
        big_window_trace(251,(macid<<24)|(ret&0xffffff),low-(u32)big_free_time());
    if (ret) u2_nan_window_arm(macid, low, high);
    return ret;
}

u32 big_faw_continue(void)
{
    u32 (*select_next)(void) = (void *)0xb89ab69d;
    u32 (*arm_next)(u32) = (void *)0xb89a7d39;
    u32 ret = select_next();
    if (ret == R32(0xb8e1454c)) ret = arm_next(0);
    return ret;
}

u32 big_schedule_macid(void)
{
    if ((R8(0xb8e16e2d) & 7) != 7 ||
        !*(volatile u16 *)0xb8e16ae2) return 0xffff;
    return R8(0xb8e16dfe);
}

u32 big_stopped(void)
{
    return !R32(0xb8e16e08) && !R32(0xb8e16e0c);
}
u32 big_time(void)
{
    u32 (*read)(u32) = (void *)R32(0xb8e094d4);
    return read(0xc438);
}
u32 big_frame(u32 port, u8 **base, volatile u8 **wd)
{
    u32 c = R32(0xb8e0a814), b, q, pid, mask, ref, p, i, length;
    if (!c) return 2;
    b = R32(c + 24);
    if (!b) return 2;
    q = port ? port + 3 : 0;
    pid = R8(b + 241 + 2*q) | (R8(b + 242 + 2*q) << 8);
    if (pid >= 0xfff || big_queued(0, pid) != 1) return 2;
    *wd = (volatile u8 *)(R32(0xb8e14564) | (pid << 15));
    length = *(volatile u16 *)(*wd + 8) & 0x3fff;
    if (((*wd)[24] >> 4 & 7) != port) return 3;
    mask = R32(0xb8e14568);
    ref = *(volatile u32 *)(*wd + 60);
    for (i = 0; i < 4; i++) {
        p = R32(b + 32 + 16*port + 4*i);
        if (p && (p & mask) == ref && length == 63) {
            *base = (u8 *)p;
            return 0;
        }
    }
    for (i = 0; i < 2; i++) {
        p = (u32)u2_bigtk_state.frame[i];
        if (port == u2_bigtk_state.ports[i] && (p & mask) == ref && length == 81) {
            *base = (u8 *)p;
            return 0;
        }
    }
    return 3;
}
void *memset(void *destination, int value, unsigned n)
{
    u8 *p = destination;
    while (n--) *p++ = value;
    return destination;
}

void big_iqc_restore(const struct u2_iqc_channel *iqc, u32 preserve_tssi)
{
    void (*write_bb)(u32, u32, u32) = (void *)0xb8999e69;
    u32 path;
    for (path = 0; path < 2; path++) {
        u32 offset = path << 8;
        write_bb(0x8104 + offset, 1, iqc->page);
        write_bb(0x8138 + offset, 0xffffffff, iqc->tx[path]);
        write_bb(0x813c + offset, 0xffffffff, iqc->rx[path]);
        write_bb(0x8124 + offset, 0xffffffff, iqc->enable[path]);
    }
    if (preserve_tssi || !iqc->tssi[0].valid || !iqc->tssi[1].valid) return;
    for (path = 0; path < 2; path++) {
        const struct u2_tssi_path *tssi = &iqc->tssi[path];
        u32 offset = path << 13;
        if (!tssi->enabled) continue;
        write_bb(0x5630 + offset, 0xffffffff, tssi->alignment[0]);
        write_bb(0x5634 + offset, 0xffffffff, tssi->alignment[1]);
        write_bb(0x563c + offset, 0xffffffff, tssi->alignment[2]);
        write_bb(0x5640 + offset, 0xffffffff, tssi->alignment[3]);
        write_bb(0x5820 + offset, 0x80000000, 0);
        write_bb(0x5820 + offset, 0x80000000, 1);
        write_bb(0x5818 + offset, 0xff, tssi->offset);
        write_bb(0x5818 + offset, 0x10000000, 0);
        write_bb(0x5818 + offset, 0x10000000, 1);
        /* Native efuse DE uses one value for both CCK forms and all OFDM widths. */
        write_bb(0x5858 + offset, 0x003ff000, tssi->cck_de);
        write_bb(0x5860 + offset, 0x003ff000, tssi->cck_de);
        write_bb(0x5838 + offset, 0x003ff000, tssi->ofdm_de);
        write_bb(0x5840 + offset, 0x003ff000, tssi->ofdm_de);
        write_bb(0x5848 + offset, 0x003ff000, tssi->ofdm_de);
        write_bb(0x5850 + offset, 0x003ff000, tssi->ofdm_de);
        write_bb(0x5828 + offset, 0x003ff000, tssi->ofdm_de);
        write_bb(0x5830 + offset, 0x003ff000, tssi->ofdm_de);

    }
}

/* NAN C2H func9: request sequence, status, reserved[2]. The native sender
 * owns allocated payloads; it copies the descriptor before returning. */
u32 big_rfk_grant(u32 seq)
{
    u32 (*allocate)(u32, u32, u8 **, u32) = (void *)R32(0xb8e09dc8);
    u32 (*send)(const u8 *, u8 *) = (void *)0xb8985e25;
    u8 descriptor[5] = {9, 13, 1, 12, 0};
    u8 *payload = 0;
    u32 ret = allocate(0, 4, &payload, 0);
    if (ret || !payload) return 4;
    payload[0] = seq;
    payload[1] = payload[2] = payload[3] = 0;
    return send(descriptor, payload);
}

void big_window_trace(u32 marker, u32 skipped, u32 tail_events)
{
    void (*log)(u32,u32,u32,u32,u32,u32,u32,u32) = (void *)R32(0xb8e098f0);
    log(R32(0xb8e14b10),R32(0xb8e14b14),2,857,3,marker,skipped,tail_events);
}
unsigned long long big_free_time(void)
{
    unsigned long long (*read)(u32) = (void *)R32(0xb8e09bf4);
    return read(0);
}
u32 big_aggregate_time(u32 units)
{
    u32 (*read)(u32) = (void *)R32(0xb8e094d4);
    void (*write)(u32,u32) = (void *)R32(0xb8e094c8);
    u32 value = read(0xc610);
    if (units) write(0xc610,(value & 0x00ffffff) | (units << 24));
    return value >> 24;
}
int big_tssi_matches(const struct u2_iqc_channel *iqc)
{
    /* Native BB access adds the base at 0xb8e1474c before the bus read. */
    u32 (*read)(u32, u32) = (void *)0xb8999f41;
    u32 path, i;
    const u16 align[4] = {0x5630,0x5634,0x563c,0x5640};
    const u16 de[8] = {0x5858,0x5860,0x5838,0x5840,0x5848,0x5850,0x5828,0x5830};
    if (!iqc->tssi[0].valid || !iqc->tssi[1].valid) return 0;
    for (path = 0; path < 2; path++) {
        u32 offset;
        const struct u2_tssi_path *tssi = &iqc->tssi[path];
        if (!tssi->valid || !tssi->enabled) continue;
        offset = path << 13;
        if (!(read(0x5820+offset, 0xffffffff)&0x80000000) ||
            !(read(0x5818+offset, 0xffffffff)&0x10000000)) return 0;
        for (i=0;i<4;i++) if (read(align[i]+offset, 0xffffffff)!=tssi->alignment[i]) return 0;
        for (i=0;i<8;i++) if (((read(de[i]+offset, 0xffffffff)>>12)&0x3ff) !=
            (i<2 ? tssi->cck_de : tssi->ofdm_de)) return 0;
        /* Preserve the live TSSI offset and convergence pulses on a warm visit. */
    }
    return 1;
}

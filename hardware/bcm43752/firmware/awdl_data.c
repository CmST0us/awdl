#include "native_calls.h"
#include "awdl_peer.h"
#include "awdl_frame.h"
#include "awdl_tx.h"

#define PTR(b, o) (*(void **)((uint8_t *)(b) + (o)))
#define U16(b, o) (*(uint16_t *)((uint8_t *)(b) + (o)))
#define U32(b, o) (*(uint32_t *)((uint8_t *)(b) + (o)))

static void copy(void *to, const void *from, unsigned bytes)
{
    ((void *(*)(void *, const void *, unsigned))native_0x151a21)(to, from, bytes);
}

static struct awdl_session *session(void *wlc)
{
    void *wrapper = PTR(wlc, 0x520);
    return awdl_session_get(wrapper ? PTR(wrapper, 4) : 0);
}

/* Native A-MSDU admission reads PCIe metadata from the packet head. Our
 * writable local header is not a TXFRAG; its original PCIe owner remains
 * in PKTNEXT. Resolve reads only, without copying ownership fields into
 * local storage or changing the allocator/freeing flags. */
unsigned awdl_data_fragment_field(void *packet, unsigned offset)
{
    void *original = packet, *scb = PTR(packet, 0x2c);
    void *bss = scb ? PTR(scb, 0x0c) : 0;
    struct awdl_session *s = bss ? session(PTR(bss, 0)) : 0;
    if (s && s->bss == bss) {
        void **ids = *(void ***)0x1701d0;
        while (packet && !(U32(packet, 0x10) & 0x200u))
            packet = ids[U16(packet, 0x18)];
    }
    return U16(packet ? packet : original, offset);
}

/* Tail hooks replace four LDRH instructions in 0x1f06b8. LR holds the
 * first flow ID, not a return address; preserve it across subsequent
 * reads. R12 is dead at these sites. The native function's five-register
 * push leaves SP four bytes off AAPCS alignment: saving 20 more bytes
 * aligns the C call. Flags are overwritten before use at these three sites. */
#define FRAGMENT_FIELD_GATE(name, packet_reg, offset, result_slot, resume) \
NATIVE_ENTRY void name(void) \
{ \
    __asm__ volatile( \
        "push {r0-r3,lr}\n" \
        "mov r0," packet_reg "\nmovs r1,#" offset "\n" \
        "bl awdl_data_fragment_field\n" \
        "str r0,[sp,#" result_slot "]\n" \
        "pop {r0-r3,lr}\nldr pc,=" resume "\n"); \
}

FRAGMENT_FIELD_GATE(awdl_data_first_flow, "r6", "66", "16", "0x1f06c9")
FRAGMENT_FIELD_GATE(awdl_data_next_flow, "r1", "66", "12", "0x1f070b")
FRAGMENT_FIELD_GATE(awdl_data_next_sequence, "r1", "90", "12", "0x1f0735")
#undef FRAGMENT_FIELD_GATE

NATIVE_ENTRY void awdl_data_first_sequence(void)
{
    /* Unlike the other reads, this LDRH sits between CMP r5,#0 and IT EQ.
     * Preserve APSR as well as registers; the 28-byte save aligns SP. */
    __asm__ volatile(
        "push {r0-r3,r12,lr}\n"
        "mrs r12,apsr\npush {r12}\n"
        "mov r0,r6\nmovs r1,#90\nbl awdl_data_fragment_field\n"
        "str r0,[sp,#16]\npop {r12}\nmsr APSR_nzcvq,r12\n"
        "pop {r0-r3,r12,lr}\nldr pc,=0x1f0721\n");
}

int awdl_data_dma(void *dma, void *packet, unsigned commit)
{
    void *scb = PTR(packet, 0x2c);
    void *bss = scb ? PTR(scb, 0x0c) : 0;
    struct awdl_session *s = bss ? session(PTR(bss, 0)) : 0;
    if (s && s->bss == bss) {
        int error = awdl_tx_stamp(s, packet);
        if (error)
            return error; /* Native caller retains and requeues the packet. */
    }
    /* Native 0x1ed804 selects its DMA walker from the head's flags only.
     * A replacement local header can precede the PCIe TX fragment without
     * carrying TXFRAG (0x200); the local-only walker then omits the payload.
     * Select the native fragment walker for our owned chain. Do not mark
     * a local allocation TXFRAG: that flag also controls native freeing.
     * Header-split chains (0x200000) retain their original DMA callback.
     * DHD submits one host DMA range per MSDU; PKTLINK batches were already
     * detached by awdl_data_sendpkt, independently of this PKTNEXT chain.
     */
    if (s && s->bss == bss && !(U32(packet, 0x10) & 0x200200u)) {
        void **ids = *(void ***)0x1701d0;
        for (void *part = packet; part; part = ids[U16(part, 0x18)]) {
            if (U32(part, 0x10) & 0x200u) {
                /* Preserve the dispatcher's shared DMA register selector. */
                if (((uint8_t *)dma)[0x36])
                    ((void (*)(void *, unsigned))native_0x186d3d)(dma, 0);
                return ((int (*)(void *, void *, unsigned))native_0x1ed659)(
                    dma, packet, commit);
            }
        }
    }
    return ((int (*)(void *, void *, unsigned))native_0x1ed805)(dma, packet, commit);
}

NATIVE_ENTRY static unsigned original_sendpkt(void *, void *, void *)
{
    __asm__ volatile(
        "ldr r3,=0x181ae4\n"
        "push {r4-r11,lr}\n"
        "ldr pc,=0x1f8457\n");
}

unsigned awdl_data_sendpkt(void *wlc, void *packet, void *wlcif)
{
    struct awdl_session *s = session(wlc);
    if (!s || ((void *(*)(void *, void *))native_0x1bd385)(wlc, wlcif) != s->bss)
        return original_sendpkt(wlc, packet, wlcif);
    /* The native chained fast path copies an eight-byte LLC from the
     * previous packet (0x1f26f2), without running encapsulation again.
     * Submit each AWDL MSDU through preparation for its own sequence and
     * 16-byte header. D11/SCB aggregation and native completion stay intact.
     * Detach PKTLINK before preparation can replace the head; otherwise
     * the original fragment retains a second MSDU link when it is freed. */
    unsigned result = 0;
    while (packet) {
        void **ids = *(void ***)0x1701d0;
        void *next = ids[U16(packet, 0x1a)];
        U16(packet, 0x1a) = 0;
        U32(packet, 0x10) &= ~0x1000u;
        result |= original_sendpkt(wlc, packet, wlcif);
        packet = next;
    }
    return result;
}

unsigned awdl_data_headroom(void *wlc, void *scb)
{
    struct awdl_session *s = session(wlc);
    return s && scb && PTR(scb, 0x0c) == s->bss ? 104 : 96;
}

NATIVE_ENTRY void awdl_data_headroom_gate(void)
{
    /* Replace CMP/BLS at 0x1f774e. Preserve the native caller's registers
     * and compare its measured headroom against the owned BSS requirement. */
    __asm__ volatile(
        "push {r0-r3,r12,lr}\n"
        "mov r0,r6\nmov r1,r10\nbl awdl_data_headroom\n"
        "ldr r1,[sp,#12]\ncmp r1,r0\n"
        "pop {r0-r3,r12,lr}\n"
        "blo 1f\nldr pc,=0x1f7753\n"
        "1: ldr pc,=0x1f775d\n");
}

/* Checked caller: A2 RAM 0x1f78c2, in native packet preparation 0x1f7718.
 * It has already made the packet header writable, copied tags when a new
 * head was required, and ensured headroom. The owned AWDL BSS requires
 * 104 rather than 96 bytes so its longer header leaves the same room for
 * native encryption/802.11/D11 headers. */
void awdl_data_encapsulate(void *wlc, void *osh, const uint8_t *ethernet, void *packet)
{
    struct awdl_session *s = session(wlc);
    /* 0x1f8880 calls packet preparation before installing the SCB tag.
     * In particular a replacement head can still have a null tag here.
     * Resolve through the same owned BSS/address/authorization gate as
     * sendpkt, rather than mistaking an untagged AWDL MSDU for plain LLC. */
    if (!s || !awdl_peer_tx_owner(s, s->bss, ethernet)) {
        ((void (*)(void *, void *, const void *, void *))native_0x1ee06f)(
            wlc, osh, ethernet, packet);
        return;
    }
    /* The old header may alias the packet being prepended. Save its
     * addresses and EtherType before replacing any bytes. Payload and
     * fragment-chain ownership stay with the original native packet. */
    uint8_t ethernet_copy[14];
    copy(ethernet_copy, ethernet, sizeof(ethernet_copy));
    uint8_t *data = (uint8_t *)PTR(packet, 8) - BCM_AWDL_MSDU_HEADER_SIZE;
    PTR(packet, 8) = data;
    U16(packet, 12) += BCM_AWDL_MSDU_HEADER_SIZE;
    copy(data, ethernet_copy, 12);
    unsigned bytes = ((unsigned (*)(void *, void *))native_0x1ec101)(osh, packet) - 14;
    data[12] = (uint8_t)(bytes >> 8);
    data[13] = (uint8_t)bytes;
    bcm_awdl_msdu_encode(data + 14, s->data_sequence++, ethernet_copy + 12);
}

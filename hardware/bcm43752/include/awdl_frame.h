#ifndef BCM43752_AWDL_FRAME_H
#define BCM43752_AWDL_FRAME_H

/* Native AWDL MSDU framing, shared by firmware and DHD. The caller owns
 * packet headroom, length changes and checksums. These helpers never change
 * an Ethernet/NAN packet in place or authorize a station. The complete
 * 16-byte header replaces ordinary RFC1042 SNAP; do not prepend both.
 * Layout matches the BE200 native data path: Apple SNAP (10 bytes),
 * little-endian sequence (2), zero padding (2), network EtherType (2).
 */
#define BCM_AWDL_MSDU_HEADER_SIZE 16u
#define BCM_AWDL_BSSID_BYTES { 0x00, 0x25, 0x00, 0xff, 0x94, 0x73 }

static inline void bcm_awdl_msdu_encode(unsigned char *out,
                                        unsigned short sequence,
                                        const unsigned char protocol[2])
{
    static const unsigned char snap[10] = {
        0xaa, 0xaa, 0x03, 0x00, 0x17, 0xf2, 0x08, 0x00, 0x03, 0x04
    };
    /* Permit protocol to alias the destination, as in an skb head push. */
    unsigned char high = protocol[0], low = protocol[1];
    unsigned i;
    for (i = 0; i < sizeof(snap); ++i)
        out[i] = snap[i];
    out[10] = (unsigned char)sequence;
    out[11] = (unsigned char)(sequence >> 8);
    out[12] = out[13] = 0;
    out[14] = high;
    out[15] = low;
}

/* Return zero for a truncated or non-AWDL MSDU. On success the caller
 * removes exactly HEADER_SIZE bytes and restores the Ethernet protocol.
 * Sequence/padding are not authentication or replay protection; AWDL's
 * upper-layer TLS remains responsible for AirDrop transport security.
 */
static inline int bcm_awdl_msdu_decode(const unsigned char *in,
                                       unsigned length,
                                       unsigned char protocol[2])
{
    static const unsigned char snap[10] = {
        0xaa, 0xaa, 0x03, 0x00, 0x17, 0xf2, 0x08, 0x00, 0x03, 0x04
    };
    unsigned i;
    if (length < BCM_AWDL_MSDU_HEADER_SIZE)
        return 0;
    for (i = 0; i < sizeof(snap); ++i)
        if (in[i] != snap[i])
            return 0;
    protocol[0] = in[14];
    protocol[1] = in[15];
    return 1;
}

#endif

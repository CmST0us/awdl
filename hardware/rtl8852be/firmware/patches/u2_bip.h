/* SPDX-License-Identifier: GPL-2.0 */
#ifndef U2_BIP_H
#define U2_BIP_H
struct u2_cmac_key {
    unsigned char round[176];
    unsigned char subkey[2][16];
};
void u2_cmac_key(struct u2_cmac_key *, const unsigned char key[16]);
void u2_cmac(const struct u2_cmac_key *, const unsigned char *, unsigned length,
             unsigned char mac[16]);
/* frame includes the 18-byte MMIE with an eight-byte MIC at its end. */
void u2_beacon_mic(const struct u2_cmac_key *, const unsigned char *frame,
                   unsigned length, unsigned char mic[8]);
#endif

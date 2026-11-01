#ifndef WA_PHY_H
#define WA_PHY_H
#include "utils/includes.h"
#include "utils/common.h"
struct wa_caps;
struct wa_nan_ndi_config;
int wa_ndp_phy(const struct wa_caps *, const u8 *, size_t, struct wa_nan_ndi_config *);
#endif

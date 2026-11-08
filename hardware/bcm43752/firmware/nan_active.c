/* Active A2 NAN lifecycle. Keep allocation and scheduling failures observable,
 * retain ownership until actual cancellation, and publish completed state. */
#include <stddef.h>
#include <stdint.h>
#include "nan_mac.h"
#include "nan_data.h"
#include "nan_peer.h"
#include "nan_initialize.h"
#include "nan_link.h"
#include "nan_dam.h"
#include "nan_host.h"

#define FIELD(type, base, offset) (*(type *)((uint8_t *)(base) + (offset)))
#define PTR(base, offset) FIELD(void *, base, offset)
#define U32(base, offset) FIELD(uint32_t, base, offset)
#define U16(base, offset) FIELD(uint16_t, base, offset)
#define U8(base, offset) FIELD(uint8_t, base, offset)

/* Device-capability encoding calls this even without firmware-owned NCS-SK.
 * Hostap installs ordinary pairwise keys; absence of the optional firmware
 * security context cannot advertise extended key IDs or dereference NULL. */
unsigned nan_security_ext_iv(void *security)
{
    return security && PTR(security, 8) ? U8(PTR(security, 8), 6) : 0;
}

void nan_watchdog(void *wrapper)
{
    if (!wrapper || !PTR(wrapper, 0) || !PTR(wrapper, 4))
        return;
    void *wlc = PTR(wrapper, 0);
    void *nan = PTR(wrapper, 4);
    if (PTR(nan, 8) != wlc || !PTR(wlc, 0) || !PTR(PTR(wlc, 0), 4) ||
        !U8(PTR(PTR(wlc, 0), 4), 0x30) || !PTR(nan, 0x10))
        return;
    void *common = PTR(nan, 0x10);
    void *mac = PTR(nan, 0x14);
    if (!U8(common, 0x20) || U8(common, 0x30) || !mac ||
        !PTR(mac, 0x38) || !U8(PTR(mac, 0x38), 0x4a))
        return;
    /* Native MAC tick advances election warmup, discovery scan countdowns
     * and template updates. Do not replace DW completion with this clock.
     * Native peer aging reaches our ownership-checked release hook, so an
     * expired peer with a station, TX or NDP owner remains allocated.
     * Autonomous DAM/NDP/ranging/security watchdogs are deliberately absent:
     * their negotiation/termination policy belongs to the host backend. */
    ((void (*)(void *, void *))0x30069u)(mac, wlc);
    ((void (*)(void *, void *))0x37a5du)(nan, wlc);
}

int nan_mac_enable_active(void *wlc, void *nan)
{
    if (!wlc || !nan || PTR(nan, 8) != wlc || !PTR(nan, 0x14) ||
        !PTR(nan, 0x10) || !PTR(wlc, 0) || !PTR(PTR(wlc, 0), 4))
        return -2;
    void *mac = PTR(nan, 0x14);
    void *runtime = PTR(mac, 0x38);
    void *config = PTR(mac, 0x20);
    void *bands = PTR(mac, 8);
    if (!runtime || !config || !PTR(mac, 0x34) || !bands ||
        !PTR(bands, 0) || !PTR(bands, 4) || !PTR(PTR(bands, 0), 0) ||
        !U8(PTR(nan, 0x10), 0x20))
        return -25;
    if (U8(runtime, 0x4a)) {
        ((int (*)(const char *, ...))0xa815u)("NAN MAC already active: pub=%u\n",
            U8(PTR(PTR(wlc, 0), 4), 0x30));
        return U8(PTR(PTR(wlc, 0), 4), 0x30) ? 0 : -16;
    }
    /* A failed cancellation retains timers, schedules and in-flight TX for
     * STOP. Beacon templates alone are valid before enable: preparing the
     * NMI emits native BSS notification 0x80, whose 0x2a6f4 callback builds
     * both templates. The cluster builder updates those owned templates. */
    if (PTR(mac, 0xe4) || PTR(mac, 0x10c) || U32(mac, 0xb4)) {
        ((int (*)(const char *, ...))0xa815u)("NAN MAC pending owners: %p %p %u\n",
            PTR(mac, 0xe4), PTR(mac, 0x10c), U32(mac, 0xb4));
        return -16;
    }
    for (unsigned band = 0; band < 2; ++band) {
        void *context = ((void **)bands)[band];
        if (PTR(context, 0x0c) || PTR(context, 0x10) || PTR(context, 0x14)) {
            ((int (*)(const char *, ...))0xa815u)("NAN MAC band %u pending: %p %p %p\n",
                band, PTR(context, 0x0c), PTR(context, 0x10), PTR(context, 0x14));
            return -16;
        }
    }
    void *bss = PTR(PTR(bands, 0), 0);
    int error = -27;
    const char *stage = "timers";
    PTR(mac, 0x10c) = ((void *(*)(void *))0x1f41du)(nan);
    if (!PTR(mac, 0x10c))
        return error;
    /* Native callback/name literals: A2 0x2efd0/0x2efcc. Allocate both
     * timers before cluster, packet or schedule work can become active. */
    PTR(mac, 0xe4) = ((void *(*)(void *, uintptr_t, void *, uintptr_t))0x1f445u)(
        nan, 0x29909u, mac, 0x15764au);
    if (!PTR(mac, 0xe4))
        goto failed;
    U8(PTR(mac, 0x34), 0x30) = 0;
    stage = "cluster";
    error = ((int (*)(void *, void *, void *, void *))0x2aa9du)(
        mac, bss, runtime, config);
    U32(runtime, 0x4c) = error;
    if (error)
        goto failed;
    /* The original cluster builder does not propagate packet allocation
     * failure. Initial enable requires both real beacon templates. */
    if (!PTR(mac, 0x3c) || !PTR(mac, 0x40)) {
        error = -27;
        goto failed;
    }
    U32(runtime, 4) = 2;
    ((void (*)(void *))0x2abb9u)(mac);
    U8(runtime, 0x4a) = 1;
    /* 0x2c388 returns its logger's result on DW registration failure.
     * Its band=2 loop also overwrites a first-band failure with the second
     * band's result. Preserve each native registration's actual error. */
    unsigned bands_used = U8(config, 0x50) ? 2 : 1;
    stage = "DW";
    for (unsigned band = 0; band < bands_used; ++band) {
        error = ((int (*)(void *, unsigned))0x2e08du)(mac, band);
        if (error)
            break;
    }
    if (!error && U32(runtime, 0x10) - 3u < 2u) {
        stage = "scan";
        for (unsigned band = 0; band < bands_used; ++band) {
            error = ((int (*)(void *, unsigned))0x2e039u)(mac, band);
            if (error)
                break;
        }
    }
    U32(runtime, 0x4c) = error;
    if (error) {
        ++U32(PTR(mac, 0x70), 0x50);
        goto failed;
    }
    ((void (*)(void *, uint32_t, uint32_t, void *, void *, unsigned))0x30261u)(
        mac, U32(mac, 0x5c), U32(mac, 0x58), (uint8_t *)mac + 0xbc,
        (uint8_t *)mac + 0xb8, U16(runtime, 0x54));
    ((void (*)(void *, unsigned))0xbefe9u)(PTR(wlc, 0x100), 0);
    ((void (*)(void *, unsigned))0x22559u)(nan, 1);
    stage = "band enable";
    error = ((int (*)(void *, unsigned))0x2fc5du)(mac, 0);
    if (!error && U8(config, 0x50))
        error = ((int (*)(void *, unsigned))0x2fc5du)(mac, 1);
    if (error)
        goto failed;
    return 0;
failed: {
    ((int (*)(const char *, ...))0xa815u)("NAN MAC %s: %d\n", stage, error);
    int cleanup = nan_mac_disable_active(wlc, nan);
    if (cleanup)
        ((int (*)(const char *, ...))0xa815u)("NAN MAC cleanup: %d\n", cleanup);
    return cleanup ? cleanup : error;
}
}

int nan_modules_disable(void *wlc, void *nan)
{
    if (!nan || !wlc || PTR(nan, 8) != wlc || !PTR(nan, 0x14) ||
        !PTR(nan, 0x18) || !PTR(nan, 0x5c))
        return -2;
    void *discovery = PTR(nan, 0x18);
    /* Autonomous discovery/FSM sessions are not registered by this backend.
     * Keep any unexpected owner instead of discarding its pending work. */
    if (U32(PTR(nan, 0x14), 0xb4) || PTR(discovery, 8) ||
        PTR(discovery, 0x0c) || U8(discovery, 0x10) ||
        U8(discovery, 0x50) || U8(discovery, 0x58))
        return -16;
    int error = nan_host_stop(nan);
    if (!error)
        error = nan_data_check_idle(nan);
    if (!error)
        error = nan_peer_disable(wlc, nan);
    if (!error)
        error = nan_link_check_idle(nan);
    if (!error)
        error = nan_dam_disable(wlc, nan);
    if (!error)
        error = ((int (*)(void *, void *))0x26079u)(wlc, nan);
    if (!error)
        error = nan_mac_disable_active(wlc, nan);
    return error;
}

int nan_modules_enable(void *wlc, void *nan)
{
    int error = nan_mac_enable_active(wlc, nan);
    if (error)
        return error;
    error = ((int (*)(void *, void *))0x21bb9u)(wlc, nan);
    if (error) {
        ((int (*)(const char *, ...))0xa815u)("NAN DAM enable: %d\n", error);
        int cleanup = nan_modules_disable(wlc, nan);
        return cleanup ? cleanup : error;
    }
    return 0;
}

int nan_common_enable(void *nan, unsigned band)
{
    if (band == 1)
        return 0;
    if (band || !nan || !PTR(nan, 8) || !PTR(nan, 0x14))
        return -2;
    void *wlc = PTR(nan, 8);
    void *mac = PTR(nan, 0x14);
    if (!PTR(wlc, 0) || !PTR(PTR(wlc, 0), 4) || !PTR(mac, 8) ||
        !PTR(PTR(mac, 8), 0) || !PTR(PTR(PTR(mac, 8), 0), 0))
        return -25;
    if (U8(PTR(PTR(wlc, 0), 4), 0x30) || PTR(mac, NAN_MAC_EVENT_BSS))
        return -16;
    void *bss = PTR(PTR(PTR(mac, 8), 0), 0);
    if (U8(bss, 5) || (PTR(mac, NAN_MAC_BSS_NODE) && U8(PTR(mac, NAN_MAC_BSS_NODE), 1)))
        return -16;
    if ((U32(PTR(nan, 0x10), 0x34) & 1u) &&
        (!PTR(bss, 0x0c) || !PTR(PTR(bss, 0x0c), 8)))
        return -26;
    uint8_t power = U8(wlc, 0x133);
    U8(wlc, 0x133) = 1;
    ((void (*)(void *))0x8c3bdu)(wlc);
    int error = -4;
    if (U8(PTR(wlc, 0), 0x1c)) {
        error = ((int (*)(void *, void *, unsigned, unsigned))0x1fd59u)(
            nan, bss, band, U32(mac, 0xd4));
        if (!error)
            error = nan_modules_enable(wlc, nan);
        if (!error) {
            error = nan_bss_events(nan, 1);
            if (error)
                ((int (*)(const char *, ...))0xa815u)("NAN BSS events: %d\n", error);
        }
        if (!error) {
            U32(mac, NAN_MAC_HOST_STOPPING) = 0;
            U8(PTR(PTR(wlc, 0), 4), 0x30) = 1;
            ((void (*)(void *, unsigned))0x1e53du)(nan, band);
            ((void (*)(void *, uint32_t))0x1d821u)(nan, U32(PTR(nan, 0x10), 0x34));
            ((void (*)(void *, void *, unsigned, unsigned))0x2bd6du)(mac, bss, 0x9e, 1);
        } else {
            ((int (*)(const char *, ...))0xa815u)("NAN enable failed: %d\n", error);
            int cleanup = nan_common_disable(nan, band);
            if (cleanup)
                error = cleanup;
        }
    }
    U8(wlc, 0x133) = power;
    ((void (*)(void *))0x8c3bdu)(wlc);
    return error;
}

int nan_wlc_down(void *wrapper)
{
    if (!wrapper || !PTR(wrapper, 0) || !PTR(wrapper, 4))
        return 0;
    void *nan = PTR(wrapper, 4);
    if (PTR(nan, 8) != PTR(wrapper, 0) || !PTR(nan, 0x14))
        return -2;
    void *mac = PTR(nan, 0x14);
    /* Also drain a failed enable that has not committed common active. */
    if (!PTR(mac, NAN_MAC_BSS_MANAGER))
        return PTR(mac, NAN_MAC_EVENT_BSS) ? -30 : 0;
    return nan_common_disable(nan, 0);
}

int nan_common_disable(void *nan, unsigned band)
{
    /* A2 has one WLC. The original second-band common callback is empty;
     * module teardown processes both band schedules on the primary call. */
    if (band == 1)
        return 0;
    if (band || !nan || !PTR(nan, 8) || !PTR(nan, 0x14))
        return -2;
    void *wlc = PTR(nan, 8);
    void *mac = PTR(nan, 0x14);
    if (!PTR(wlc, 0) || !PTR(PTR(wlc, 0), 4) || !PTR(mac, 8) ||
        !PTR(PTR(mac, 8), 0) || !PTR(PTR(PTR(mac, 8), 0), 0))
        return -25;
    void *bss = PTR(PTR(PTR(mac, 8), 0), 0);
    unsigned active = U8(PTR(PTR(wlc, 0), 4), 0x30);
    int error = nan_host_stop(nan);
    if (!error)
        error = nan_modules_disable(wlc, nan);
    if (!error)
        error = ((int (*)(void *, void *))0x22769u)(PTR(nan, 0x30), wlc);
    if (!error)
        error = ((int (*)(void *, void *))0x2e87du)(mac, bss);
    if (!error)
        error = nan_bss_events(nan, 0);
    if (error)
        return error;
    /* Native code clears PUBCMN before these fallible BSS operations, making
     * the next STOP look complete. Commit only after actual BSS stop. */
    if (PTR(nan, 0x84))
        ((void (*)(void *))0x1f779u)(nan);
    U8(PTR(PTR(wlc, 0), 4), 0x30) = 0;
    ((void (*)(void *, unsigned))0x1e53du)(nan, band);
    if (active)
        ((void (*)(void *, void *, unsigned, unsigned))0x2bd6du)(mac, bss, 0x9e, 0x0b);
    return 0;
}

int nan_mac_disable_active(void *wlc, void *nan)
{
    if (!wlc || !nan || PTR(nan, 8) != wlc || !PTR(nan, 0x14))
        return -2;
    void *mac = PTR(nan, 0x14);
    void *runtime = PTR(mac, 0x38);
    void *bands = PTR(mac, 8);
    if (!runtime || !bands || !PTR(bands, 0) || !PTR(bands, 4))
        return -25;
    int error = nan_host_stop(nan);
    if (!error)
        error = nan_data_check_idle(nan);
    if (!error)
        error = nan_peer_check_idle(nan);
    if (error)
        return error;
    /* TX completion owns the last packet reference. Keep the radio and
     * callbacks usable while the host drains outstanding management frames. */
    if (U32(mac, 0xb4))
        return -16;
    int active = U8(runtime, 0x4a);
    int resources = active || PTR(mac, 0xe4) || PTR(mac, 0x10c) ||
                    PTR(mac, 0x3c) || PTR(mac, 0x40);
    for (unsigned band = 0; band < 2; ++band) {
        void *context = ((void **)bands)[band];
        if (PTR(context, 0x0c) || PTR(context, 0x10) || PTR(context, 0x14))
            resources = 1;
    }
    if (!resources)
        return 0;
    /* The original combined loop returns only its final error. Process each
     * band separately and retain the first failure. Call-site hooks preserve
     * the actual handles until MSCH membership confirms removal. */
    for (unsigned band = 0; band < 2; ++band) {
        void *context = ((void **)bands)[band];
        if (!PTR(context, 0)) {
            if (PTR(context, 0x0c) || PTR(context, 0x10) || PTR(context, 0x14))
                return -30;
            continue;
        }
        error = ((int (*)(void *, unsigned))0x2e115u)(mac, band);
        if (!error)
            error = ((int (*)(void *, unsigned))0x2e085u)(mac, band);
        if (!error)
            error = ((int (*)(void *, unsigned))0x2e1c9u)(mac, band);
        if (error)
            return error;
        if (PTR(context, 0x0c) || PTR(context, 0x10) || PTR(context, 0x14))
            return -16;
    }
    if (PTR(mac, 0xe4)) {
        /* Native wl_del_timer returns false while callback ownership remains.
         * wl_free_timer subsequently frees the timer object, not just a tag. */
        if (!PTR(PTR(mac, 0xe4), 0x44))
            return -30;
        if (!((int (*)(void *, void *))0x1ef85u)(nan, PTR(mac, 0xe4)))
            return -16;
        ((void (*)(void *, void *))0x1f1c5u)(nan, PTR(mac, 0xe4));
        PTR(mac, 0xe4) = 0;
    }
    if (PTR(mac, 0x10c)) {
        /* 0x1f434 only cancels HRT. 0x1f43c cancels and frees the allocation. */
        if (!PTR(PTR(mac, 0x10c), 4))
            return -30;
        ((void (*)(void *, void *))0x1f43du)(nan, PTR(mac, 0x10c));
        PTR(mac, 0x10c) = 0;
    }
    U8(mac, 0x94) = 0;
    U8(mac, 0x110) = 0;
    ((void (*)(void *))0x2ccddU)(mac); /* Native capability-list reset. */
    ((void (*)(void *))0x2a785U)(mac); /* Native owned beacon packets. */
    ((void (*)(void *))0x2a925U)(mac); /* Clears active only after real cleanup. */
    return 0;
}

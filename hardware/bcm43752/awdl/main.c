/* SPDX-License-Identifier: BSD-3-Clause */
#include "../hostap/ndi.h"
#include "../hostap/phy.h"
#include "controller.h"
#include "network.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

struct application {
    struct bcm_awdl_session session;
    struct bcm_awdl_controller controller;
    struct bcm_awdl_network network;
    struct bcm_ndi ndi;
    unsigned revision;
    int stopping, error;
};
static volatile sig_atomic_t interrupted;
static void interrupt(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}
static uint64_t read_le64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= (uint64_t)p[i] << (i * 8);
    return value;
}
static void received(void *context, const struct bcm_event *event)
{
    struct application *a = context;
    if (bcm_ndi_changed(&a->ndi, event))
        ++a->revision;
    if (event->reason == BCM_AWDL_EVENT_REASON &&
        event->length >= BCM_AWDL_EVENT_HEADER_SIZE &&
        read_le64(event->data + 8) == a->session.generation) {
        ++a->revision;
        if (!a->stopping && a->controller.control) {
            int error = bcm_awdl_controller_event(&a->controller, event);
            if (error)
                a->error = error;
        }
    }
}
static int events_until(struct application *a, unsigned revision, uint64_t deadline)
{
    while (a->revision == revision) {
        uint64_t now = bcm_awdl_monotonic_us();
        if (!now || now >= deadline)
            return -ETIMEDOUT;
        struct pollfd fd = {.fd = bcm_link_fd(a->session.link), .events = POLLIN};
        int result = poll(&fd, 1, (int)((deadline - now + 999) / 1000));
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return result ? -errno : -ETIMEDOUT;
        if (fd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -EIO;
        int error = bcm_link_receive(a->session.link);
        if (error)
            return error;
    }
    return 0;
}
static int ndi_wait(struct application *a, int deleting, uint64_t deadline)
{
    for (;;) {
        unsigned revision = a->revision;
        int error = bcm_ndi_command(&a->ndi, a->session.link, BCM_NAN_NDI_QUERY);
        if (error)
            return error;
        if (deleting ? bcm_ndi_gone(&a->ndi) : bcm_ndi_created(&a->ndi))
            return 0;
        if (a->ndi.native_result || a->ndi.host_result)
            return -EIO;
        error = events_until(a, revision, deadline);
        if (error)
            return error;
    }
}
static int cleanup(struct application *a)
{
    a->stopping = 1;
    uint64_t deadline = bcm_awdl_monotonic_us() + UINT64_C(15000000);
    struct bcm_awdl_status status;
    int error = bcm_awdl_query(&a->session, &status);
    if (error)
        return error;
    if (status.generation && status.generation != a->session.generation)
        return -ESTALE;
    /* Close admission, then let DHD complete real PCIe ring teardown. */
    if (status.generation) {
        error = bcm_awdl_window(&a->session, 0, 0);
        if (error && !(status.flags & BCM_AWDL_STOPPING))
            return error;
    }
    if (a->ndi.attempted) {
        error = bcm_ndi_command(&a->ndi, a->session.link, BCM_NAN_NDI_QUERY);
        if (error == -ENOENT && !status.generation) {
            /* CREATE was rejected before the driver allocated an owner. */
            a->ndi.attempted = false;
        } else if (error) {
            return error;
        }
    }
    if (a->ndi.attempted && !bcm_ndi_gone(&a->ndi)) {
        error = bcm_ndi_command(&a->ndi, a->session.link, BCM_NAN_NDI_QUIESCE);
        if (error)
            return error;
        if (!(a->ndi.flags & BCM_NAN_NDI_QUIESCED) || a->ndi.host_result)
            return -EBUSY;
    }
    while (status.generation) {
        unsigned revision = a->revision;
        int stopped = bcm_awdl_stop(&a->session);
        error = bcm_awdl_query(&a->session, &status);
        if (error)
            return error;
        if (!status.generation)
            break;
        if (status.generation != a->session.generation)
            return -ESTALE;
        if (stopped && stopped != -EBUSY)
            return stopped;
        error = events_until(a, revision, deadline);
        if (error)
            return error;
    }
    if (status.flags || status.queued || status.submitted)
        return -EBUSY;
    if (a->ndi.attempted && !bcm_ndi_gone(&a->ndi)) {
        error = bcm_ndi_command(&a->ndi, a->session.link, BCM_NAN_NDI_DELETE);
        if (!error)
            error = ndi_wait(a, 1, deadline);
        if (error)
            return error;
    }
    if (if_nametoindex("awdl0"))
        return -EBUSY;
    a->session.generation = 0;
    return 0;
}
static struct bcm_link *open_phy(unsigned phy, struct application *a)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/ieee80211/phy%u/device/net", phy);
    DIR *directory = opendir(path);
    if (!directory)
        return NULL;
    struct bcm_link *link = NULL;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (entry->d_name[0] == '.')
            continue;
        struct bcm_link *candidate = bcm_link_open(entry->d_name, received, a);
        if (!candidate)
            continue;
        if (bcm_link_phy(candidate) == phy) {
            link = candidate;
            break;
        }
        bcm_link_close(candidate);
    }
    closedir(directory);
    if (!link)
        errno = ENODEV;
    return link;
}
static int current_mac(uint8_t mac[6])
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    struct ifreq request = {0};
    memcpy(request.ifr_name, "awdl0", 6);
    int error = ioctl(fd, SIOCGIFHWADDR, &request) ? -errno : 0;
    if (!error)
        memcpy(mac, request.ifr_hwaddr.sa_data, 6);
    close(fd);
    return error;
}
static int recover(struct application *a, const struct bcm_awdl_status *status)
{
    uint8_t mac[6];
    if (status->generation)
        memcpy(mac, status->local, 6);
    else if (if_nametoindex("awdl0")) {
        int error = current_mac(mac);
        if (error)
            return error;
    } else {
        return 0;
    }
    int error = bcm_ndi_prepare(&a->ndi, mac, bcm_link_address(a->session.link), "awdl0");
    if (error)
        return error;
    a->ndi.attempted = true;
    a->session.generation = status->generation;
    /* Query ownership before touching a pre-existing interface. */
    error = bcm_ndi_command(&a->ndi, a->session.link, BCM_NAN_NDI_QUERY);
    return error ? error : cleanup(a);
}
int main(int argc, char **argv)
{
    if ((argc != 3 && argc != 5) || strncmp(argv[1], "phy", 3))
        return 2;
    char *end;
    errno = 0;
    unsigned long phy_number = strtoul(argv[1] + 3, &end, 10);
    if (errno || *end || end == argv[1] + 3 || phy_number > UINT32_MAX)
        return 2;
    int stop_only = !strcmp(argv[2], "stop");
    if (!stop_only && (argc != 5 || (strcmp(argv[2], "149") && strcmp(argv[2], "161"))))
        return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, interrupt);
    signal(SIGTERM, interrupt);
    signal(SIGPIPE, SIG_IGN);
    char lock_path[128];
    snprintf(lock_path, sizeof(lock_path), "/run/lock/wirelessd-bcm-awdl-phy%lu", phy_number);
    int lock = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
        if (lock >= 0) close(lock);
        return 1;
    }
    struct application a = {0};
    struct bcm_phy phy = {0};
    int error = 0, released = 0;
    a.session.link = open_phy((unsigned)phy_number, &a);
    if (!a.session.link) {
        error = -errno;
        goto done;
    }
    a.network.link = a.session.link;
    struct bcm_awdl_status status;
    error = bcm_awdl_clock_sample(&a.session, &status);
    if (error)
        goto done;
    if (stop_only) {
        released = recover(&a, &status);
        error = released;
        printf("{\"event\":\"stopped\",\"rc\":%d,\"cleanup\":%d}\n", error, released);
        goto done;
    }
    if (status.generation || if_nametoindex("awdl0")) {
        error = -EBUSY;
        goto done;
    }
    error = bcm_awdl_network_policy(&a.network);
    if (!error) error = bcm_phy_read(a.session.link, &phy);
    if (!error && (!phy.present[1] || !phy.ht[1] || !phy.vht[1]))
        error = -EOPNOTSUPP;
    if (error)
        goto done;
    if (getrandom(&a.session.generation, 8, 0) != 8 || !a.session.generation) {
        error = -EIO;
        goto done;
    }
    /* Release/acquire selects the actual AirDrop sender. Keep its discovered
     * IPv6 endpoint stable across that transition so SYN retries still reach
     * us. Ordinary NAN's default NDI uses NMI xor 1; AWDL uses xor 2.
     * Native NDI creation still rejects an explicitly configured collision. */
    memcpy(a.network.local, bcm_link_address(a.session.link), 6);
    a.network.local[0] = (a.network.local[0] | 2) & ~1u;
    a.network.local[5] ^= 2;
    error = bcm_ndi_prepare(&a.ndi, a.network.local, bcm_link_address(a.session.link), "awdl0");
    if (!error) error = bcm_ndi_command(&a.ndi, a.session.link, BCM_NAN_NDI_CREATE);
    if (!error) error = ndi_wait(&a, 0, bcm_awdl_monotonic_us() + UINT64_C(15000000));
    if (error)
        goto stop;
    unsigned primary = (unsigned)atoi(argv[2]);
    unsigned secondary = primary == 149 ? 161 : 149;
    /* Guard includes clock uncertainty once; excessive command latency is
     * a failure, not a reason to advertise a fictitious accurate schedule. */
    if (a.session.uncertainty_us >= 65536 - a.network.drain_us) {
        error = -ETIMEDOUT;
        goto stop;
    }
    unsigned drain = a.network.drain_us + (unsigned)a.session.uncertainty_us;
    error = bcm_awdl_start(&a.session, a.network.local, primary, secondary, drain);
    if (!error) error = bcm_awdl_network_up();
    if (error)
        goto stop;
    uint8_t he[21], *he_pointer = NULL;
    const struct he_capabilities *cap = &phy.band[1].he_capab[IEEE80211_MODE_INFRA];
    if (cap->he_supported) {
        memcpy(he, cap->mac_cap, 6);
        memcpy(he + 6, cap->phy_cap, 11);
        memcpy(he + 17, cap->mcs, 4);
        he_pointer = he;
    }
    error = bcm_awdl_controller_open(&a.controller, &a.session, a.network.local,
        primary, secondary, argv[3], argv[4], he_pointer, phy.band[1].vht_capab,
        drain, bcm_awdl_network_ready, &a.network);
    if (error)
        goto stop;
    printf("{\"event\":\"started\"}\n");
    while (!interrupted) {
        uint64_t next = bcm_awdl_controller_tick(&a.controller);
        if (a.error || a.controller.error) {
            error = a.error ? a.error : a.controller.error;
            break;
        }
        uint64_t now = bcm_awdl_monotonic_us();
        uint64_t wait = next > now ? (next - now + 999) / 1000 : 0;
        struct pollfd f[2] = {{.fd = bcm_link_fd(a.session.link), .events = POLLIN},
                             {.fd = STDIN_FILENO, .events = POLLIN}};
        int result = poll(f, 2, wait > INT_MAX ? INT_MAX : (int)wait);
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0) { error = -errno; break; }
        if (f[0].revents & (POLLERR | POLLHUP | POLLNVAL)) { error = -EIO; break; }
        if (f[0].revents & POLLIN) {
            error = bcm_link_receive(a.session.link);
            if (error) break;
        }
        if (f[1].revents & (POLLIN | POLLHUP)) {
            char input[256];
            if (!fgets(input, sizeof(input), stdin)) break;
            input[strcspn(input, "\r\n")] = 0;
            if (!strcmp(input, "STOP")) break;
            if (!strncmp(input, "SELECT ", 7)) {
                error = bcm_awdl_controller_select(&a.controller, input + 7);
                if (error) break;
            }
        }
    }
stop:
    do {
        released = cleanup(&a);
        printf("{\"event\":\"stopped\",\"rc\":%d,\"cleanup\":%d}\n", error, released);
        if (!released)
            break;
        /* Keep the process, transport and retained generation available for
         * an explicit STOP retry. Dead parent recovery uses the same API. */
        char input[128];
        if (!fgets(input, sizeof(input), stdin))
            break;
        input[strcspn(input, "\r\n")] = 0;
        if (strcmp(input, "STOP"))
            break;
    } while (1);
    if (released)
        error = released;
done:
    if (error)
        fprintf(stderr, "BCM AWDL: %s (%d)\n", strerror(-error), error);
    bcm_awdl_controller_close(&a.controller);
    bcm_phy_clear(&phy);
    if (a.session.link) bcm_link_close(a.session.link);
    close(lock);
    return error ? 1 : 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#define _POSIX_C_SOURCE 200809L
#include "network.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int execute(char *const argv[])
{
    pid_t child = fork();
    if (child < 0)
        return -errno;
    if (!child) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int status;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            return -errno;
    return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -EIO;
}
static unsigned positive(const char *text)
{
    if (!text || !*text)
        return 0;
    for (const char *p = text; *p; ++p)
        if (!isdigit((unsigned char)*p))
            return 0;
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    return errno || *end || value > INT_MAX ? 0 : (unsigned)value;
}
int bcm_awdl_network_policy(struct bcm_awdl_network *n)
{
    n->congestion = getenv("AWARE_AWDL_TCP_CONGESTION");
    n->window = getenv("AWARE_AWDL_INITIAL_WINDOW");
    n->reordering = getenv("AWARE_AWDL_REORDERING");
    n->queue = getenv("AWARE_AWDL_TX_QUEUE");
    unsigned drain = positive(getenv("AWARE_AWDL_TX_DRAIN_TU"));
    if (!n->congestion || !*n->congestion || strlen(n->congestion) >= 16 ||
        !positive(n->window) || !positive(n->reordering) || !positive(n->queue) ||
        drain < 1 || drain > 60)
        return -EINVAL;
    for (const char *p = n->congestion; *p; ++p)
        if (!isalnum((unsigned char)*p) && *p != '_')
            return -EINVAL;
    n->drain_us = drain * 1024;
    return 0;
}
int bcm_awdl_network_up(void)
{
    int error = execute((char *[]){"ip", "link", "set", "awdl0", "addrgenmode", "none", NULL});
    if (!error)
        error = execute((char *[]){"ip", "link", "set", "awdl0", "mtu", "1500", "up", NULL});
    return error;
}
static void linklocal(const uint8_t mac[6], uint8_t address[16], char text[INET6_ADDRSTRLEN])
{
    memset(address, 0, 16);
    address[0] = 0xfe;
    address[1] = 0x80;
    memcpy(address + 8, mac, 3);
    address[8] ^= 2;
    address[11] = 0xff;
    address[12] = 0xfe;
    memcpy(address + 13, mac + 3, 3);
    inet_ntop(AF_INET6, address, text, INET6_ADDRSTRLEN);
}
int bcm_awdl_network_ready(void *context, const uint8_t peer[6])
{
    struct bcm_awdl_network *n = context;
    uint8_t source[16], destination[16];
    char local_text[INET6_ADDRSTRLEN], peer_text[INET6_ADDRSTRLEN], mac[18], prefix[INET6_ADDRSTRLEN + 4];
    linklocal(n->local, source, local_text);
    linklocal(peer, destination, peer_text);
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
    snprintf(prefix, sizeof(prefix), "%s/64", local_text);
    int error = execute((char *[]){"ip", "-6", "addr", "replace", prefix, "dev", "awdl0", "nodad", NULL});
    if (!error)
        error = execute((char *[]){"ip", "-6", "neigh", "replace", peer_text, "lladdr", mac,
                                   "nud", "permanent", "dev", "awdl0", NULL});
    if (!error)
        error = execute((char *[]){"ip", "-6", "route", "replace", peer_text, "dev", "awdl0",
                                   "rto_min", "1100ms", "congctl", (char *)n->congestion,
                                   "initcwnd", (char *)n->window, "reordering", (char *)n->reordering, NULL});
    if (!error)
        error = bcm_link_clear_tcp_metrics(n->link, source, destination);
    if (!error)
        error = execute((char *[]){"ip", "link", "set", "awdl0", "txqueuelen", (char *)n->queue, NULL});
    if (!error)
        error = execute((char *[]){"tc", "qdisc", "replace", "dev", "awdl0", "root", "fq", NULL});
    if (!error)
        printf("{\"event\":\"netdev-ready\",\"interface\":\"awdl0\",\"local\":\"%s\",\"peer\":\"%s\",\"mac\":\"%s\"}\n",
               local_text, peer_text, mac);
    return error;
}

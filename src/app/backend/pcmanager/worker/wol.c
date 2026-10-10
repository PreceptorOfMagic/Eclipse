#include "worker.h"
#include "app.h"
#include "logging.h"
#include "../pclist.h"
#include "../priv.h"

#include "wol.h"

#include <errno.h>
#include <stdio.h>
#include <SDL.h>

#include <arpa/inet.h>
#ifndef _WIN32
#include <ifaddrs.h>
#endif
#ifndef _WIN32
#include <net/if.h>   /* IFF_UP / IFF_LOOPBACK, used only in the getifaddrs loop */
#endif
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Adaptive post-wake poll. Rationale for each value is at the poll loop in
 * worker_wol(); in short, cheap probes so they can be frequent, and a budget that
 * grows once the host proves it is on the network.
 *
 * THE BUDGET IS SIZED FOR A COLD BOOT, NOT A RESUME, because these hosts do both.
 * An S3 resume answers in about 6.5 s (measured on both machines). But a PC that
 * dropped out of S3 into a hard power-off -- which PC1 and PC2 each do several times
 * a month, unclean, on their own -- does not resume at all: the magic packet POWERS IT
 * ON, and it has to POST and boot Windows before Apollo exists to answer. Measured end
 * to end on PC2, 2026-08-12:
 *
 *     13:44:14  magic packet sent
 *     13:45:00  Windows boots            (+46 s -- BIOS POST plus boot)
 *     13:45:34  Apollo process starts    (+80 s)
 *     13:45:39  answering serverinfo     (+85 s)
 *
 * The budget then was 45 s, so the poll gave up at 46010 ms -- within seconds of the
 * machine finishing its boot -- and logged `woke=no` for a host that woke perfectly.
 * That is the failure this file already warns about one comment down: a false "did not
 * wake" is worse than a slow one, because it sends the next investigation after the
 * wrong cause. 120 s clears the measured 85 s with room for a slower disk or a Windows
 * update on boot. */
#define WOL_CONNECT_SECS 1
#define WOL_POLL_STEP_MS 250
#define WOL_POLL_MAX_MS 1000
#define WOL_COLD_MS 120000
#define WOL_WARM_MS 180000

/*
 * SEND THE MAGIC PACKET EVERY WAY, OUT EVERY INTERFACE.
 *
 * wol_broadcast() (third_party/commons, a submodule, so not ours to edit) opens one
 * socket bound to INADDR_ANY and sends ONE datagram to 255.255.255.255:9. A limited
 * broadcast is not routed -- the kernel picks a single interface from the routing
 * table and it leaves there and nowhere else.
 *
 * THAT IS A REAL GAP ON THIS CLIENT, because the TV has TWO NICs and both sit on the
 * SAME /24: the wired UE300 (eth1) and wlan0. Whichever one owns the default route
 * gets the packet and the other never sees it, decided by routing state we do not
 * control and which changes when a cable is moved or Wi-Fi re-associates.
 *
 * MEASURED 2026-08-10, and it is why this is a robustness fix and not the PC1 fix:
 * all three forms below were sent from the TV and ALL THREE ARRIVED at PC1 with the
 * correct MAC, so the packet was never the reason that machine would not wake. This
 * removes a way for the wake to fail silently on a future move, nothing more. Do not
 * let it be cited as the cause of anything.
 *
 * Three forms because switches treat them differently: the limited broadcast, the
 * interface's own subnet-directed broadcast (survives some switch configurations that
 * drop 255.255.255.255), and a unicast to the host's last known address (works only
 * while the neighbour entry is still cached, which it often is right after suspend).
 * Ports 9 and 7 are both conventional; a compliant NIC pattern-matches the payload and
 * ignores the port, so this costs two datagrams and removes an assumption.
 */
static void wol_sendto(intptr_t fd, const void *packet, size_t len, uint32_t addr, uint16_t port,
                       unsigned *sent) {
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = addr;
    dst.sin_port = htons(port);
    if (sendto(fd, packet, len, 0, (struct sockaddr *) &dst, sizeof(dst)) == (ssize_t) len) {
        (*sent)++;
    }
}

/** @return number of datagrams that left successfully. */
static unsigned wol_send_everywhere(const char *macstr, const char *unicast) {
    unsigned int v[6];
    if (sscanf(macstr, "%x:%x:%x:%x:%x:%x%*c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return 0;
    }
    unsigned char packet[102];
    memset(packet, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        for (int b = 0; b < 6; b++) {
            packet[6 + i * 6 + b] = (unsigned char) v[b];
        }
    }

    unsigned sent = 0;
#ifdef _WIN32
    /* WINDOWS HAS NO getifaddrs, AND THIS IS A REAL REDUCTION -- said plainly rather than
     * left to look equivalent. The loop below exists because a limited broadcast is not
     * routed: the kernel picks ONE interface, so the packet is sent out each one in turn,
     * bound to that interface's address. The Win32 equivalent is GetAdaptersAddresses, and
     * until that is written this sends a single unbound limited broadcast and lets the
     * routing table choose. That is enough on a one-NIC machine and NOT enough on a
     * multi-homed one, which is exactly the case the loop was added for. */
    {
        intptr_t fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd >= 0) {
            int on = 1;
            setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
            for (int pi = 0; pi < 2; ++pi) {
                const uint16_t port = pi == 0 ? 9 : 7;
                wol_sendto(fd, packet, sizeof(packet), htonl(INADDR_BROADCAST), port, &sent);
            }
            close(fd);
        }
    }
#else
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) == 0) {
        for (struct ifaddrs *p = ifa; p != NULL; p = p->ifa_next) {
            if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_INET) {
                continue;
            }
            if ((p->ifa_flags & IFF_UP) == 0 || (p->ifa_flags & IFF_LOOPBACK) != 0) {
                continue;
            }
            const uint32_t self = ((struct sockaddr_in *) p->ifa_addr)->sin_addr.s_addr;
            intptr_t fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) {
                continue;
            }
            int on = 1;
            setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
            /* Bind to THIS interface's address so the limited broadcast leaves here
             * rather than wherever the routing table would have sent it. */
            struct sockaddr_in src;
            memset(&src, 0, sizeof(src));
            src.sin_family = AF_INET;
            src.sin_addr.s_addr = self;
            src.sin_port = 0;
            if (bind(fd, (struct sockaddr *) &src, sizeof(src)) == 0) {
                for (int pi = 0; pi < 2; ++pi) {
                    const uint16_t port = pi == 0 ? 9 : 7;
                    wol_sendto(fd, packet, sizeof(packet), htonl(INADDR_BROADCAST), port, &sent);
                    if (p->ifa_netmask != NULL) {
                        const uint32_t mask = ((struct sockaddr_in *) p->ifa_netmask)->sin_addr.s_addr;
                        wol_sendto(fd, packet, sizeof(packet), self | ~mask, port, &sent);
                    }
                }
            }
            close(fd);
        }
        freeifaddrs(ifa);
    }
#endif /* _WIN32 */

    /* Unicast last, on its own socket: it needs no broadcast permission and reaches a
     * host whose neighbour entry is still cached. */
    if (unicast != NULL && unicast[0] != '\0') {
        struct in_addr ua;
        if (inet_aton(unicast, &ua) != 0) {
            intptr_t fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd >= 0) {
                wol_sendto(fd, packet, sizeof(packet), ua.s_addr, 9, &sent);
                wol_sendto(fd, packet, sizeof(packet), ua.s_addr, 7, &sent);
                close(fd);
            }
        }
    }
    return sent;
}

int worker_wol(worker_context_t *context) {
    /* TAKE A COPY, UNDER THE LOCK -- THE POLL BELOW USED TO READ A FREED SERVER_DATA.
     *
     * pclist_find_by_uuid() locks and unlocks INTERNALLY, so the node it returns is
     * already unprotected by the time we have it. This function then kept that
     * SERVER_DATA* and dereferenced it for up to 15 s inside the poll loop.
     *
     * WHAT FREES IT -- AND WHY A SUCCESSFUL WAKE IS THE DANGEROUS ONE. The co-op
     * dialog re-queries every not-yet-online host every 2.5 s (coop.dialog.c
     * wake_tick_cb -> pcmanager_request_update). While the host is still asleep that
     * update fails and upserts with server=NULL, which frees nothing. The moment the
     * host ANSWERS -- i.e. the wake worked -- update.c builds a fresh SERVER_DATA and
     * upserts it, and pclist_node_apply() calls serverdata_free() on the very pointer
     * this function is still reading. The executor runs 2*min(3,ncpu) threads
     * (backend_root.c), so the wake and the update genuinely overlap.
     *
     * The diag log measured on the TV 2026-08-08 says exactly that: two wakes to
     * 192.0.2.5 that never came back logged their "woke=no" 15 s later and were
     * harmless, while the wake to 192.0.2.16 that DID come back logged
     * "[WOL@173518] host=192.0.2.16 ... rc=0" and then no "woke=" line at all --
     * the next entry is tick 4298, a fresh app run. That is the crash to the TV home
     * screen the user reported. So this is NOT "two wakes race each other": ONE
     * co-op wake is enough, and it faults precisely when it succeeds.
     *
     * The single-PC wake button (apps.controller.c) survived it because nothing
     * re-queries the host every 2.5 s there, so nothing upserts inside the window.
     *
     * The lock is recursive (SDL_CreateMutex), so wrapping the find is safe -- and
     * note the old NULL branch unlocked a mutex this function had never taken.
     * Nothing below this block touches the list, so two of these can run in parallel. */
    char mac[64] = {0};
    char address[128] = {0};
    uint16_t port = 0;
    pcmanager_lock(context->manager);
    const pclist_t *node = pcmanager_node(context->manager, &context->uuid);
    if (node == NULL) {
        pcmanager_unlock(context->manager);
        return ENOENT;
    }
    const SERVER_DATA *server = node->server;
    if (server != NULL) {
        if (server->mac != NULL) {
            snprintf(mac, sizeof(mac), "%s", server->mac);
        }
        if (server->serverInfo.address != NULL) {
            snprintf(address, sizeof(address), "%s", server->serverInfo.address);
        }
        port = server->extPort;
    }
    pcmanager_unlock(context->manager);
    if (mac[0] == '\0' || address[0] == '\0') {
        return ENOENT;
    }

    /* LOG THE SEND, AND LOG IT WHERE IT CAN ACTUALLY BE READ. commons_log output is
     * not retrievable from the TV (same reason session_worker.c writes its quit trace
     * here), so a failed wake used to leave no record of whether a packet went out.
     *
     * wol_broadcast() returns -1 WITHOUT SENDING if the MAC will not parse, and the
     * poll below then waits the full 15 s exactly as if it had sent -- so "no usable
     * MAC" and "the PC ignored it" looked identical. Keep the return value. */
    int wolrc = wol_broadcast(mac);
    /* Then again, deliberately, out every interface and in every form -- see
     * wol_send_everywhere(). The original call is kept rather than replaced so a
     * failure in the new path can never make the wake worse than it was. */
    const unsigned extra = wol_send_everywhere(mac, address);
    FILE *wd = fopen(AURORA_DIAG_LOG, "a");
    if (wd != NULL) {
        fprintf(wd, "[WOL@%u] host=%s mac=%s rc=%d extra=%u%s\n", (unsigned) SDL_GetTicks(),
                address, mac, wolrc, extra,
                (wolrc != 0 && extra == 0) ? "  <- NOTHING SENT" : "");
        fclose(wd);
    }

    /* THE WAIT ADAPTS TO THE HOST -- A FIXED WALL GAVE UP EXACTLY WHEN PC1 ARRIVED.
     *
     * The budget was a flat 15 s with a 3 s sleep between probes, on a client whose
     * CONNECT timeout is 5 s (app_gs_client_new -> gs_set_timeout(hnd, 5)). A sleeping
     * host therefore got TWO probes -- t=0..5 and t=8..13 -- and the loop quit at t=16.
     * Two unattended runs on 2026-08-11 measured PC1 still dead at +12 s and answering
     * at +19 s after the magic packet, so it was abandoned at the moment it came back
     * and a machine that woke perfectly logged "woke=no". That false signal is worse
     * than a slow wake: it is what sends the next investigation after the wrong cause.
     *
     * A bigger constant would only move the wall, and every machine wakes at its own
     * speed, so nothing here is a guess about how long a PC takes:
     *
     *  - Probes are made CHEAP (1 s connect) so they can be FREQUENT. A host that is up
     *    answers a LAN connect in milliseconds, so the short poll is what makes the wake
     *    feel immediate instead of waiting out the next 3 s tick.
     *  - Silence only means "not yet", so probing continues to WOL_COLD_MS.
     *  - ECONNREFUSED means the TCP stack ANSWERED: the machine is demonstrably awake
     *    and only Apollo is still starting. That earns the longer WOL_WARM_MS budget.
     *    The old code instead broke out on ECONNREFUSED and then reported "woke=no"
     *    for a host that had plainly woken. */
    GS_CLIENT gs = app_gs_client_new(context->app);
    gs_set_timeout(gs, WOL_CONNECT_SECS);
    const Uint32 started = SDL_GetTicks();
    Uint32 budget = WOL_COLD_MS, poll = WOL_POLL_STEP_MS;
    bool answered = false;
    int ret = -1;
    /* Unsigned elapsed rather than an absolute deadline, so a SDL_GetTicks() wrap
     * cannot turn the budget into an instant timeout. */
    while (SDL_GetTicks() - started < budget) {
        PSERVER_DATA tmpserver = serverdata_new();
        ret = gs_get_status(gs, tmpserver, strdup(address), port, false);
        serverdata_free(tmpserver);
        if (ret == 0) {
            break;
        }
        if (errno == ECONNREFUSED && !answered) {
            answered = true;
            budget = WOL_WARM_MS;
            commons_log_info("WoL", "%s answered TCP after %u ms -- awake, awaiting Umbra",
                             address, (unsigned) (SDL_GetTicks() - started));
        }
        SDL_Delay(poll);
        if (poll < WOL_POLL_MAX_MS) {
            poll += WOL_POLL_STEP_MS;
        }
    }
    const unsigned elapsed = (unsigned) (SDL_GetTicks() - started);
    gs_destroy(gs);
    /* And whether it actually came back, so a wake that was sent but ignored is
     * distinguishable from one that was never sent at all. `after=` is the MEASURED
     * wake time for this host and `tcp=` says whether the stack answered before Apollo
     * did -- together they turn "it felt slow" into a number, and they are what a
     * per-host learned budget would be built from if the adaptive wait ever needs it. */
    wd = fopen(AURORA_DIAG_LOG, "a");
    if (wd != NULL) {
        fprintf(wd, "[WOL@%u] host=%s woke=%s after=%ums tcp=%s (poll ret=%d)\n",
                (unsigned) SDL_GetTicks(), address, ret == 0 ? "YES" : "no", elapsed,
                answered ? "yes" : "no", ret);
        fclose(wd);
    }
    return ret;
}

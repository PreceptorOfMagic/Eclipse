#include "coop_pi_pads.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/** Must match XLINK_PI_QUERY_PORT in xlink/pi/xlink_pi_query.py. */
#ifndef COOP_PI_PORT
#define COOP_PI_PORT 48212
#endif

#define QUERY_MSG "XLINK_PI_QUERY_V1"
#define REPLY_PREFIX "XLINK_PI_V1 "
#define ROUTE_ACK "XLINK_PI_ROUTED_V1"

/*
 * NON-BLOCKING AT CREATION, NOT PER CALL.
 *
 * The reads below ask for MSG_DONTWAIT, which winsock has no per-call equivalent for:
 * coop_win_compat.h maps it to 0 and warns, in as many words, that a caller relying on it
 * "against a blocking socket ... will hang". This was that caller. Measured on Windows
 * 2026-09-23 with a gdb backtrace of the frozen app: the co-op dialog's rescan timer parked
 * the LVGL MAIN THREAD in recvfrom() waiting for a Pi that never answers
 * (coop_pi_scan_poll <- rescan_timer_cb <- lv_timer_handler <- app_run_loop), and because the
 * main thread is also what drains the app bus, the discovery worker -- holding the throttle
 * lock inside app_bus_post_sync() -- could never complete either. The whole app wedged, and
 * on screen that looks like the co-op dialog freezing on its GPU question.
 *
 * SO_RCVTIMEO did not save it: the scan opened its socket with a zero timeout, and zero means
 * WAIT FOREVER, not "do not wait". The socket itself is therefore non-blocking, like every
 * other co-op socket (coop_ipc_set_nonblocking). Routing waits with poll() against its own
 * monotonic deadline; scanning never waits at all.
 */
static bool set_nonblocking(intptr_t fd) {
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket((SOCKET) fd, FIONBIO, &on) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static intptr_t open_bcast_socket(void) {
    intptr_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    if (!set_nonblocking(fd)) {
        close(fd);
        return -1;
    }
    return fd;
}

/** Pull "pad=<name>;host=<ip>" out of a reply. Returns false when pad= is empty,
 *  which is the ordinary "a Pi is there but no controller is attached" answer. */
static bool parse_reply(const char *body, char *name, size_t name_cap) {
    const char *p = strstr(body, "pad=");
    if (p == NULL) {
        return false;
    }
    p += 4;
    const char *end = strchr(p, ';');
    size_t len = end != NULL ? (size_t) (end - p) : strlen(p);
    if (len == 0) {
        return false;
    }
    if (len > name_cap - 1) {
        len = name_cap - 1;
    }
    memcpy(name, p, len);
    name[len] = '\0';
    return true;
}

/** Turn "192.0.2.7" into "192.0.2.255". Returns false for anything that is not a
 *  plain dotted quad -- a hostname gives us no subnet to derive. */
static bool directed_broadcast(const char *addr, struct in_addr *out) {
    struct in_addr a;
    if (addr == NULL || inet_pton(AF_INET, addr, &a) != 1) {
        return false;
    }
    /* Assume /24. The alternative is asking the OS for the prefix of whichever
     * interface would route there, which webOS does not make available to this app;
     * a /24 covers every network this is deployed on and a wrong guess merely means
     * this extra packet goes nowhere, with the global broadcast still in play. */
    a.s_addr |= htonl(0x000000FFu);
    *out = a;
    return true;
}

static void scan_send(intptr_t fd, const char *subnet_hint) {
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(COOP_PI_PORT);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(fd, QUERY_MSG, strlen(QUERY_MSG), 0, (struct sockaddr *) &dst, sizeof(dst));
    /* And again to the hint's own subnet, so a multi-homed TV reaches the network the
     * PCs are actually on rather than whichever one won the default route. */
    struct in_addr directed;
    if (directed_broadcast(subnet_hint, &directed)) {
        dst.sin_addr = directed;
        sendto(fd, QUERY_MSG, strlen(QUERY_MSG), 0, (struct sockaddr *) &dst, sizeof(dst));
    }
}

bool coop_pi_scan_begin(coop_pi_scan_t *scan, const char *subnet_hint) {
    if (scan == NULL) {
        return false;
    }
    memset(scan, 0, sizeof(*scan));
#ifndef ECLIPSE_PI_HUB
    /* NOT IN RELEASE BUILDS. The Pi controller hub is this project's own rig, not something
     * Eclipse ships, so a release build sends no query: no socket, no broadcast, an empty pad
     * list -- and with no hub pad listed, no route is ever attempted. -DECLIPSE_PI_HUB=ON
     * builds it back in. fd is set explicitly because memset leaves 0, a valid descriptor. */
    (void) subnet_hint;
    scan->fd = -1;
    return false;
#else
    /* The non-blocking socket -- not the MSG_DONTWAIT on each read -- guarantees the UI
     * thread is never parked waiting for a Pi. The flag alone is a no-op on Windows. */
    scan->fd = open_bcast_socket();
    if (scan->fd < 0) {
        return false;
    }
    scan_send(scan->fd, subnet_hint);
    return true;
#endif
}

void coop_pi_scan_probe(coop_pi_scan_t *scan, const char *subnet_hint) {
    if (scan != NULL && scan->fd >= 0) {
        scan_send(scan->fd, subnet_hint);
    }
}

bool coop_pi_scan_poll(coop_pi_scan_t *scan) {
    if (scan == NULL || scan->fd < 0) {
        return false;
    }
    bool changed = false;
    for (;;) {
        char buf[512];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(scan->fd, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                             (struct sockaddr *) &from, &from_len);
        if (n <= 0) {
            break;                          /* nothing waiting: the usual case */
        }
        buf[n] = '\0';
        if (strncmp(buf, REPLY_PREFIX, strlen(REPLY_PREFIX)) != 0) {
            continue;
        }
        char from_addr[COOP_PI_ADDR_MAX];
        inet_ntop(AF_INET, &from.sin_addr, from_addr, sizeof(from_addr));
        char name[COOP_PI_NAME_MAX];
        if (!parse_reply(buf + strlen(REPLY_PREFIX), name, sizeof(name))) {
            continue;                       /* a Pi with no controller attached */
        }
        /* DEDUPE ON PI **AND** NAME. The same Pi answers both broadcasts, and every
         * probe repeats the whole set; but a Pi can report SEVERAL pads, one datagram
         * each, so deduping on the Pi alone would throw all but the first away. That
         * mistake is invisible in the logs and shows up as a missing controller. */
        bool seen = false;
        for (size_t k = 0; k < scan->count; ++k) {
            if (strcmp(scan->pads[k].pi_addr, from_addr) == 0 &&
                strcmp(scan->pads[k].name, name) == 0) {
                seen = true;
                break;
            }
        }
        if (seen || scan->count >= sizeof(scan->pads) / sizeof(scan->pads[0])) {
            continue;
        }
        coop_pi_pad_t *slot = &scan->pads[scan->count];
        memset(slot, 0, sizeof(*slot));
        memcpy(slot->name, name, sizeof(slot->name));
        memcpy(slot->pi_addr, from_addr, sizeof(slot->pi_addr));
        scan->count++;
        changed = true;
    }
    return changed;
}

void coop_pi_scan_end(coop_pi_scan_t *scan) {
    if (scan != NULL && scan->fd >= 0) {
        close(scan->fd);
        scan->fd = -1;
    }
}

bool coop_pi_pads_route(const char *pi_addr, const char *pad_name,
                        const char *host_ip, int timeout_ms) {
#ifndef ECLIPSE_PI_HUB
    (void) pi_addr; (void) pad_name; (void) host_ip; (void) timeout_ms;
    return false;                           /* unreachable without a hub pad; see scan_begin */
#endif
    if (pi_addr == NULL || pi_addr[0] == '\0') {
        return false;
    }
    intptr_t fd = open_bcast_socket();
    if (fd < 0) {
        return false;
    }
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(COOP_PI_PORT);
    if (inet_pton(AF_INET, pi_addr, &dst.sin_addr) != 1) {
        close(fd);
        return false;
    }
    char msg[256];
    /* "-" clears the pin. The Pi validates the address itself before writing it -- it
     * is the file that decides where a controller's input lands, so it does not take
     * our word for the format. The pad name makes this pin THAT controller only, which
     * is what lets two Pi pads drive two different PCs. */
    const char *want = host_ip != NULL && host_ip[0] != '\0' ? host_ip : "-";
    int msg_length;
    if (pad_name != NULL && pad_name[0] != '\0') {
        msg_length = snprintf(msg, sizeof(msg), "XLINK_PI_ROUTE_V1 %s %s", want, pad_name);
    } else {
        msg_length = snprintf(msg, sizeof(msg), "XLINK_PI_ROUTE_V1 %s", want);
    }
    if (msg_length < 0 || (size_t) msg_length >= sizeof(msg) ||
        sendto(fd, msg, (size_t) msg_length, 0,
               (struct sockaddr *) &dst, sizeof(dst)) != msg_length) {
        close(fd);
        return false;
    }

    char expected[128];
    const int expected_length = snprintf(expected, sizeof(expected), "%s %s", ROUTE_ACK, want);
    if (expected_length < 0 || (size_t) expected_length >= sizeof(expected)) {
        close(fd);
        return false;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        close(fd);
        return false;
    }
    const int64_t start_ms = (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
    const int64_t deadline_ms = start_ms + (timeout_ms > 0 ? timeout_ms : 0);
    bool acknowledged = false;

    for (;;) {
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            break;
        }
        const int64_t now_ms = (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
        int64_t remaining = deadline_ms - now_ms;
        if (remaining < 0) {
            break;
        }
        if (remaining > INT_MAX) {
            remaining = INT_MAX;
        }

        struct pollfd readable = {
                .fd = fd,
                .events = POLLIN,
        };
        const int ready = poll(&readable, 1, (int) remaining);
        if (ready == 0) {
            break;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if ((readable.revents & POLLIN) == 0) {
            break;
        }

        char buf[256];
        struct sockaddr_in from;
        socklen_t from_length = sizeof(from);
        const ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                                   (struct sockaddr *) &from, &from_length);
        if (n < 0) {
#ifdef _WIN32
            const int error = WSAGetLastError();
            if (error == WSAEINTR || error == WSAEWOULDBLOCK) {
                continue;
            }
#else
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
#endif
            break;
        }

        /* Ignore discovery replies, stale ACKs for another destination, and packets
         * from any other socket.  The Pi replies from the same address/port that
         * received the route request and echoes the route it actually applied. */
        if (from_length >= (socklen_t) sizeof(from) && from.sin_family == AF_INET &&
            from.sin_port == dst.sin_port && from.sin_addr.s_addr == dst.sin_addr.s_addr &&
            n == expected_length && memcmp(buf, expected, (size_t) expected_length) == 0) {
            acknowledged = true;
            break;
        }
    }

    close(fd);
    return acknowledged;
}

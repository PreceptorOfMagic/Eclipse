/* Pi routing ACK validation and deadline regression test.
 *
 * Build + run on POSIX:
 * The hub is compiled in only with ECLIPSE_PI_HUB (release builds leave it out):
 *   gcc -std=gnu11 -Wall -Wextra -Werror -pthread -DECLIPSE_PI_HUB=1 \
 *       -Isrc/app/stream/input tests/coop/test_coop_pi_pads.c \
 *       src/app/stream/input/coop_pi_pads.c -o /tmp/test_coop_pi_pads \
 *       && /tmp/test_coop_pi_pads
 */
#include "coop_pi_pads.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define TEST_PI_PORT 48212
#define TEST_WANT "127.0.0.42"
#define ROUTE_ACK "XLINK_PI_ROUTED_V1"

typedef enum {
    SERVER_VALID_AFTER_NOISE,
    SERVER_SILENT,
} server_mode_t;

typedef struct {
    int fd;
    server_mode_t mode;
} server_args_t;

static int failures;

static int64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }
    return (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void check(bool condition, const char *description) {
    printf("  [%s] %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) {
        failures++;
    }
}

static int open_server(void) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    const struct timeval timeout = {.tv_sec = 1};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(TEST_PI_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (const struct sockaddr *) &address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void *serve_one_route(void *opaque) {
    server_args_t *args = opaque;
    char request[256];
    struct sockaddr_in client;
    socklen_t client_length = sizeof(client);
    const ssize_t request_length = recvfrom(args->fd, request, sizeof(request), 0,
                                            (struct sockaddr *) &client, &client_length);
    if (request_length <= 0) {
        close(args->fd);
        return NULL;
    }

    if (args->mode == SERVER_VALID_AFTER_NOISE) {
        /* A byte-prefix match used to accept both of these false positives: a valid
         * looking packet from another socket, and the Pi reporting another route. */
        const int rogue = socket(AF_INET, SOCK_DGRAM, 0);
        const char exact[] = ROUTE_ACK " " TEST_WANT;
        if (rogue >= 0) {
            sendto(rogue, exact, sizeof(exact) - 1, 0,
                   (const struct sockaddr *) &client, client_length);
            close(rogue);
        }
        const char wrong_route[] = ROUTE_ACK " 127.0.0.99";
        sendto(args->fd, wrong_route, sizeof(wrong_route) - 1, 0,
               (const struct sockaddr *) &client, client_length);

        const struct timespec delay = {.tv_nsec = 30 * 1000 * 1000};
        nanosleep(&delay, NULL);
        sendto(args->fd, exact, sizeof(exact) - 1, 0,
               (const struct sockaddr *) &client, client_length);
    } else {
        const struct timespec delay = {.tv_nsec = 140 * 1000 * 1000};
        nanosleep(&delay, NULL);
    }

    close(args->fd);
    return NULL;
}

static bool start_server(server_mode_t mode, pthread_t *thread, server_args_t *args) {
    args->fd = open_server();
    args->mode = mode;
    return args->fd >= 0 && pthread_create(thread, NULL, serve_one_route, args) == 0;
}

int main(void) {
#ifndef ECLIPSE_PI_HUB
    /* RELEASE BUILD: the hub is compiled out, so nothing may be opened or sent. */
    coop_pi_scan_t scan;
    check(!coop_pi_scan_begin(&scan, "192.0.2.20"), "release build: the hub scan does not start");
    check(scan.fd == -1 && scan.count == 0, "release build: no socket is opened and no pad is listed");
    coop_pi_scan_probe(&scan, NULL);
    check(!coop_pi_scan_poll(&scan), "release build: probe and poll do nothing");
    coop_pi_scan_end(&scan);
    check(!coop_pi_pads_route("127.0.0.1", "Test Pad", TEST_WANT, 50), "release build: routing is refused");
    printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
#endif
    pthread_t thread;
    server_args_t args;
    if (!start_server(SERVER_VALID_AFTER_NOISE, &thread, &args)) {
        fprintf(stderr, "Could not bind the loopback Pi test server: %s\n", strerror(errno));
        return 1;
    }
    const int64_t valid_start = monotonic_ms();
    const bool routed = coop_pi_pads_route("127.0.0.1", "Test Pad", TEST_WANT, 250);
    const int64_t valid_elapsed = monotonic_ms() - valid_start;
    pthread_join(thread, NULL);
    check(routed, "ignores wrong-source/wrong-route packets and accepts the exact ACK");
    check(valid_elapsed >= 20 && valid_elapsed < 200,
          "keeps waiting for the exact ACK within the caller's deadline");

    if (!start_server(SERVER_SILENT, &thread, &args)) {
        fprintf(stderr, "Could not restart the loopback Pi test server: %s\n", strerror(errno));
        return 1;
    }
    const int64_t timeout_start = monotonic_ms();
    const bool timed_out = !coop_pi_pads_route("127.0.0.1", "Test Pad", TEST_WANT, 70);
    const int64_t timeout_elapsed = monotonic_ms() - timeout_start;
    pthread_join(thread, NULL);
    check(timed_out, "reports failure when the Pi does not acknowledge");
    check(timeout_elapsed >= 55 && timeout_elapsed < 200,
          "honors timeout_ms instead of doing one immediate nonblocking recv");

    printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

/* test_coop_msg_channel — packet and framed-stream small-message transport.
 *
 * Build + run:
 *   gcc -std=c11 -Wall -Wextra -Werror -pthread \
 *       -Isrc/app/stream tests/coop/test_coop_msg_channel.c \
 *       src/app/stream/coop_msg_channel.c -o /tmp/test_coop_msg_channel && \
 *       /tmp/test_coop_msg_channel
 */
#include "coop_msg_channel.h"

#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures = 0;

static void check(bool condition, const char *what) {
    printf("  [%s] %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) {
        failures++;
    }
}

static int wait_message(coop_msg_channel_t *sender, coop_msg_channel_t *receiver,
                        void *out, size_t capacity, size_t *length_out) {
    for (int i = 0; i < 20000; ++i) {
        int received = coop_msg_channel_receive(receiver, out, capacity, length_out);
        if (received != COOP_MSG_RECV_IDLE) {
            return received;
        }
        int flushed = coop_msg_channel_flush(sender);
        if (flushed == COOP_MSG_ERROR) {
            return COOP_MSG_RECV_ERROR;
        }
        struct pollfd pfd = {.fd = receiver->fd, .events = POLLIN, .revents = 0};
        (void) poll(&pfd, 1, 1);
    }
    return COOP_MSG_RECV_IDLE;
}

static void test_seqpacket(void) {
    int sockets[2] = {-1, -1};
    check(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) == 0,
          "SOCK_SEQPACKET pair created");
    if (sockets[0] < 0) {
        return;
    }

    coop_msg_channel_t sender = COOP_MSG_CHANNEL_INITIALIZER;
    coop_msg_channel_t receiver = COOP_MSG_CHANNEL_INITIALIZER;
    check(coop_msg_set_nonblocking(sockets[0]) && coop_msg_set_nonblocking(sockets[1]),
          "packet sockets set non-blocking");
    check(coop_msg_channel_attach(&sender, sockets[0], 64) &&
          coop_msg_channel_attach(&receiver, sockets[1], 64),
          "packet channels attached");

    const unsigned char expected[] = {0x10, 0x20, 0x30, 0x40};
    unsigned char actual[64] = {0};
    size_t actual_length = 0;
    check(coop_msg_channel_send(&sender, expected, sizeof(expected)) == COOP_MSG_ACCEPTED,
          "packet accepted atomically");
    check(wait_message(&sender, &receiver, actual, sizeof(actual), &actual_length) ==
          COOP_MSG_RECV_MESSAGE,
          "packet received");
    check(actual_length == sizeof(expected) &&
          memcmp(actual, expected, sizeof(expected)) == 0,
          "packet boundary and payload preserved");

    coop_msg_channel_detach(&sender);
    coop_msg_channel_detach(&receiver);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_stream(void) {
    enum { BIG = 1024 * 1024 };
    int sockets[2] = {-1, -1};
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
          "SOCK_STREAM pair created");
    if (sockets[0] < 0) {
        return;
    }

    int small_buffer = 4096;
    (void) setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF,
                      &small_buffer, sizeof(small_buffer));
    check(coop_msg_set_nonblocking(sockets[0]) && coop_msg_set_nonblocking(sockets[1]),
          "stream sockets set non-blocking");

    coop_msg_channel_t sender = COOP_MSG_CHANNEL_INITIALIZER;
    coop_msg_channel_t receiver = COOP_MSG_CHANNEL_INITIALIZER;
    check(coop_msg_channel_attach(&sender, sockets[0], BIG) &&
          coop_msg_channel_attach(&receiver, sockets[1], BIG),
          "stream channels attached");

    unsigned char *expected = malloc(BIG);
    unsigned char *actual = malloc(BIG);
    if (expected == NULL || actual == NULL) {
        fprintf(stderr, "allocation failed\n");
        exit(2);
    }
    for (size_t i = 0; i < BIG; ++i) {
        expected[i] = (unsigned char) (i * 29u + 7u);
    }

    check(coop_msg_channel_send(&sender, expected, BIG) == COOP_MSG_ACCEPTED,
          "large stream message accepted without blocking");
    check(coop_msg_channel_has_pending(&sender),
          "short stream write retained for a later flush");
    const uint32_t newer = 0xA1B2C3D4u;
    check(coop_msg_channel_send(&sender, &newer, sizeof(newer)) == COOP_MSG_WOULD_BLOCK,
          "new message dropped while an older frame is pending");

    size_t actual_length = 0;
    check(wait_message(&sender, &receiver, actual, BIG, &actual_length) ==
          COOP_MSG_RECV_MESSAGE,
          "partial stream reads assemble one complete frame");
    check(actual_length == BIG && memcmp(actual, expected, BIG) == 0,
          "framed stream payload survives partial I/O byte-for-byte");
    check(!coop_msg_channel_has_pending(&sender),
          "pending stream write fully drained");

    memset(actual, 0, sizeof(newer));
    check(coop_msg_channel_send(&sender, &newer, sizeof(newer)) == COOP_MSG_ACCEPTED,
          "next stream message accepted after drain");
    check(wait_message(&sender, &receiver, actual, BIG, &actual_length) ==
          COOP_MSG_RECV_MESSAGE,
          "next framed message received without desynchronisation");
    check(actual_length == sizeof(newer) && memcmp(actual, &newer, sizeof(newer)) == 0,
          "next framed message has the correct boundary");

    free(expected);
    free(actual);
    coop_msg_channel_detach(&sender);
    coop_msg_channel_detach(&receiver);
    close(sockets[0]);
    close(sockets[1]);
}

int main(void) {
    test_seqpacket();
    test_stream();
    printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

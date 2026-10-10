/* test_coop_ipc — access-unit transport between the co-op processes.
 *
 * Runs both ends over a real unix socket in one process (fork), so it needs
 * neither the TV nor a second PC.
 *
 * Covers the properties the video path depends on:
 *   - a unit survives the round trip byte-for-byte, with its metadata;
 *   - large units (a 4K IDR is megabytes) survive short writes;
 *   - a non-blocking receiver returns 0 rather than stalling when nothing has
 *     arrived -- the video thread must never block on a late pane;
 *   - a partially-arrived unit is still completed rather than desynchronising;
 *   - a dead peer reports -1 instead of hanging.
 *
 * Build + run:
 *   gcc -O1 -g -o /tmp/test_coop_ipc tests/coop/test_coop_ipc.c \
 *       src/app/stream/video/coop_ipc.c -Isrc/app/stream/video && /tmp/test_coop_ipc
 */
#include "coop_ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;

static void check(int cond, const char *what) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) { failures++; }
}

#define SOCK "/tmp/test_coop_ipc.sock"
#define BIG (3u * 1024u * 1024u)   /* 4K IDR scale */

static uint64_t test_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

static void test_stalled_write_deadline(void) {
    int pair[2] = {-1, -1};
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
          "create a local stream for the stalled-write test");
    if (pair[0] < 0) { return; }
    int sndbuf = 4096;
    (void) setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    check(coop_ipc_set_nonblocking(pair[0]), "stalled writer is non-blocking");

    unsigned char *payload = malloc(BIG);
    check(payload != NULL, "allocate stalled-write payload");
    if (payload != NULL) {
        memset(payload, 0x5a, BIG);
        errno = 0;
        const uint64_t started = test_now_ms();
        const bool sent = coop_ipc_send_au(pair[0], 99, false, payload, BIG, NULL);
        const uint64_t elapsed = test_now_ms() - started;
        printf("  [INFO] stalled send returned after %llu ms\n",
               (unsigned long long) elapsed);
        check(!sent && errno == ETIMEDOUT,
              "a peer that never drains fails the send with ETIMEDOUT");
        check(elapsed >= COOP_IPC_IO_DEADLINE_MS &&
              elapsed < (uint64_t) COOP_IPC_IO_DEADLINE_MS * 3u,
              "stalled send obeys the absolute whole-message deadline");
        free(payload);
    }
    close(pair[0]);
    close(pair[1]);
}

static void test_stalled_read_deadline(void) {
    int pair[2] = {-1, -1};
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
          "create a local stream for the stalled-read test");
    if (pair[0] < 0) { return; }
    check(coop_ipc_set_nonblocking(pair[1]), "stalled reader is non-blocking");

    const coop_ipc_hdr_t hdr = {
            .magic = COOP_IPC_MAGIC,
            .length = 32,
            .frame = 100,
    };
    check(write(pair[0], &hdr, sizeof(hdr) / 2u) == (ssize_t) (sizeof(hdr) / 2u),
          "write only half of a framed header");
    unsigned char payload[64];
    errno = 0;
    const uint64_t started = test_now_ms();
    const long received = coop_ipc_recv_au(pair[1], payload, sizeof(payload), NULL, NULL);
    const uint64_t elapsed = test_now_ms() - started;
    printf("  [INFO] stalled read returned after %llu ms\n",
           (unsigned long long) elapsed);
    check(received == -1 && errno == ETIMEDOUT &&
          coop_ipc_last_errno == ETIMEDOUT &&
          strcmp(coop_ipc_last_stage, "message-timeout") == 0,
          "a peer that stalls mid-frame fails the read with a named timeout");
    check(elapsed >= COOP_IPC_IO_DEADLINE_MS &&
          elapsed < (uint64_t) COOP_IPC_IO_DEADLINE_MS * 3u,
          "stalled read obeys the same absolute whole-message deadline");
    close(pair[0]);
    close(pair[1]);
}

static void test_bad_magic_diagnostic(void) {
    int pair[2] = {-1, -1};
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
          "create a local stream for the framing test");
    if (pair[0] < 0) { return; }
    check(coop_ipc_set_nonblocking(pair[1]), "framing-test reader is non-blocking");
    coop_ipc_hdr_t hdr = {.magic = COOP_IPC_MAGIC ^ 1u, .length = 0};
    check(write(pair[0], &hdr, sizeof(hdr)) == (ssize_t) sizeof(hdr),
          "write a complete header with the wrong magic");
    unsigned char payload[1];
    errno = 0;
    const long received = coop_ipc_recv_au(pair[1], payload, sizeof(payload), NULL, NULL);
    check(received == -2 && errno == EBADMSG && coop_ipc_last_errno == EBADMSG &&
          strcmp(coop_ipc_last_stage, "bad-magic") == 0,
          "framing mismatch is distinct and names its failure stage");
    close(pair[0]);
    close(pair[1]);
}

int main(void) {
    unlink(SOCK);
    int lfd = coop_ipc_listen(SOCK);
    if (lfd < 0) {
        fprintf(stderr, "listen failed\n");
        return 2;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* secondary: send a small unit, then a large one */
        int fd = coop_ipc_connect(SOCK);
        if (fd < 0) { _exit(3); }
        unsigned char small[64];
        for (size_t i = 0; i < sizeof(small); ++i) { small[i] = (unsigned char) i; }
        const coop_ipc_meta_t meta = {.pts_ms = 103, .recv_ms = 107, .reasm_ms = 109,
                                      .host_10us = 23, .flags = COOP_IPC_F_PTS_ZERO};
        if (!coop_ipc_send_au(fd, 42, true, small, sizeof(small), &meta)) { _exit(4); }

        unsigned char *big = malloc(BIG);
        for (size_t i = 0; i < BIG; ++i) { big[i] = (unsigned char) (i * 7); }
        if (!coop_ipc_send_au(fd, 43, false, big, BIG, NULL)) { _exit(5); }
        free(big);
        unsigned char ctl[8] = {0};
        uint32_t ctl_frame = 0;
        long ctl_len = coop_ipc_recv_au(fd, ctl, sizeof(ctl), &ctl_frame, NULL);
        if (ctl_len != 1 || ctl_frame != COOP_IPC_CTL_FRAME ||
            ctl[0] != COOP_IPC_CTL_SHUTDOWN) { _exit(6); }
        close(fd);
        _exit(0);
    }

    int fd = coop_ipc_accept(lfd);
    check(fd >= 0, "coordinator accepts the secondary");
    check(coop_ipc_set_nonblocking(fd), "receiver set non-blocking");

    unsigned char *buf = malloc(BIG + 1024);
    uint32_t frame = 0;
    bool idr = false;
    coop_ipc_hdr_t header;

    /* first unit, retrying while it is still in flight */
    long n = 0;
    for (int i = 0; i < 20000 && n == 0; ++i) {
        n = coop_ipc_recv_au_meta(fd, buf, BIG + 1024, &frame, &idr, &header);
        if (n == 0) { usleep(200); }
    }
    check(n == 64, "small unit arrives with the right length");
    check(frame == 42 && idr, "frame number and IDR flag survive");
    check(header.pts_ms == 103 && header.recv_ms == 107 && header.reasm_ms == 109 &&
          header.host_10us == 23 && header.flags == COOP_IPC_F_PTS_ZERO,
          "capture timing and provenance survive the CP3S transport");
    int ok = 1;
    for (size_t i = 0; i < 64; ++i) { if (buf[i] != (unsigned char) i) { ok = 0; break; } }
    check(ok, "small unit payload is byte-for-byte identical");

    /* large unit: exercises short writes and mid-message completion */
    n = 0;
    for (int i = 0; i < 40000 && n == 0; ++i) {
        n = coop_ipc_recv_au_meta(fd, buf, BIG + 1024, &frame, &idr, &header);
        if (n == 0) { usleep(200); }
    }
    check(n == (long) BIG, "large unit arrives complete despite short writes");
    check(frame == 43 && !idr, "metadata correct on the large unit");
    check(header.pts_ms == 0 && header.recv_ms == 0 && header.reasm_ms == 0 &&
          header.host_10us == 0 && header.flags == 0, "omitted metadata does not reuse an older picture");
    ok = 1;
    for (size_t i = 0; i < BIG; ++i) {
        if (buf[i] != (unsigned char) (i * 7)) { ok = 0; break; }
    }
    check(ok, "large unit payload is byte-for-byte identical");

    check(coop_ipc_send_ctl(fd, COOP_IPC_CTL_SHUTDOWN),
          "coordinator sends an orderly-shutdown control frame");

    /* nothing further pending: must report 0, not block */
    n = coop_ipc_recv_au(fd, buf, BIG + 1024, NULL, NULL);
    check(n <= 0, "idle receiver returns without blocking");

    int status = 0;
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "sender completes successfully");
    /* peer has exited: must report -1 rather than hang */
    for (int i = 0; i < 100 && n == 0; ++i) {
        n = coop_ipc_recv_au(fd, buf, BIG + 1024, NULL, NULL);
        if (n == 0) { usleep(1000); }
    }
    check(n == -1, "dead peer reported as -1");

    close(fd);
    close(lfd);
    unlink(SOCK);
    free(buf);
    test_stalled_write_deadline();
    test_stalled_read_deadline();
    test_bad_magic_diagnostic();
    printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

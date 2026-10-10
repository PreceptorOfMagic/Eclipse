/* test_coop_coord — coordinator side of the two-process split.
 *
 * Runs a real secondary in a forked child over a real socket, so it needs
 * neither the TV nor a second PC.
 *
 * Covers the properties the video path depends on:
 *   - the video path never blocks: taking a pane when none has arrived returns
 *     0 immediately (this is the failure that broke streaming twice);
 *   - a pane survives the trip byte-for-byte with its metadata;
 *   - a pane is consumed once -- taking it twice must not re-deliver a stale
 *     frame, which would pair old pane 2 with current pane 1;
 *   - a short burst is queued in prediction order without unnecessary eviction;
 *   - a secondary that dies leaves the coordinator usable rather than wedged.
 *
 * Build + run:
 *   gcc -O1 -g -o /tmp/test_coop_coord tests/coop/test_coop_coord.c \
 *       src/app/stream/video/coop_coord.c src/app/stream/video/coop_ipc.c \
 *       -Isrc/app/stream/video -lpthread && /tmp/test_coop_coord
 */
#include "coop_coord.h"
#include "coop_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;

static void check(int cond, const char *what) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) { failures++; }
}

#define SOCK "/tmp/test_coop_coord.sock"

int main(void) {
    unlink(SOCK);
    check(coop_coord_start(SOCK), "coordinator starts");

    unsigned char buf[4096];
    uint32_t frame = 0;
    bool idr = false;
    unsigned long age = 0;

    /* Nothing connected yet: must return immediately, not block. */
    size_t n = coop_coord_take_latest(buf, sizeof(buf), &frame, &idr, &age);
    check(n == 0, "take with no secondary returns 0 without blocking");
    check(!coop_coord_connected(), "reports not connected");

    pid_t pid = fork();
    if (pid == 0) {
        int fd = coop_ipc_connect(SOCK);
        if (fd < 0) { _exit(3); }
        unsigned char pane[512];
        for (size_t i = 0; i < sizeof(pane); ++i) { pane[i] = (unsigned char) (i ^ 0x5a); }
        coop_ipc_send_au(fd, 7, true, pane, sizeof(pane), NULL);
        usleep(200000);
        /* A burst smaller than the queue depth must remain in prediction order. */
        for (uint32_t f = 100; f < 110; ++f) {
            coop_ipc_send_au(fd, f, false, pane, 256, NULL);
            usleep(2000);
        }
        usleep(300000);
        close(fd);
        _exit(0);
    }

    for (int i = 0; i < 400 && !coop_coord_connected(); ++i) { usleep(5000); }
    check(coop_coord_connected(), "secondary connects");

    n = 0;
    for (int i = 0; i < 400 && n == 0; ++i) {
        n = coop_coord_take_latest(buf, sizeof(buf), &frame, &idr, &age);
        if (n == 0) { usleep(5000); }
    }
    check(n == 512, "pane arrives with the right length");
    check(frame == 7 && idr, "frame number and IDR flag survive");
    int ok = 1;
    for (size_t i = 0; i < 512; ++i) {
        if (buf[i] != (unsigned char) (i ^ 0x5a)) { ok = 0; break; }
    }
    check(ok, "pane payload is byte-for-byte identical");

    /* Consumed once: an immediate re-take must not re-deliver it. */
    size_t again = coop_coord_take_latest(buf, sizeof(buf), NULL, NULL, NULL);
    check(again == 0, "a consumed pane is not re-delivered (no stale pairing)");

    /* Burst: pop oldest-first so the peer's prediction chain remains intact. */
    usleep(400000);
    uint32_t last = 0;
    unsigned drained = 0;
    bool ordered = true;
    for (uint32_t expected = 100; expected < 110; ++expected) {
        n = coop_coord_take_latest(buf, sizeof(buf), &last, NULL, NULL);
        if (n != 256 || last != expected) {
            ordered = false;
            break;
        }
        drained++;
    }
    check(drained == 10, "burst leaves every pane available");
    check(ordered && last == 109, "burst is consumed oldest-first through frame 109");
    check(coop_coord_replaced() == 0, "burst inside queue depth is not evicted");

    int status = 0;
    waitpid(pid, &status, 0);
    usleep(300000);
    n = coop_coord_take_latest(buf, sizeof(buf), NULL, NULL, NULL);
    check(n == 0, "dead secondary leaves the coordinator usable, not wedged");

    coop_coord_stop();
    check(access(SOCK, F_OK) != 0, "stop removes its video IPC socket file");
    check(coop_coord_start(SOCK), "same socket path can restart after stop");
    coop_coord_stop();
    check(access(SOCK, F_OK) != 0, "restarted coordinator also removes its socket");
    coop_coord_stop();
    unlink(SOCK);
    printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

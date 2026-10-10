/* Focused lifecycle gate for the video coordinator.
 *
 * The client drains the two coordinator control frames, then deliberately writes half a
 * header and stops.  stop() must wake that partial read without closing a descriptor under
 * the worker.  Repetition plus ThreadSanitizer covers run/connected/request publication.
 *
 * Build + run:
 *   gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread \
 *       -DAURORA_DIAG_LOG='"/tmp/test-coop-coord-concurrency.log"' \
 *       -Isrc/app/stream/video tests/coop/test_coop_coord_concurrency.c \
 *       src/app/stream/video/coop_coord.c src/app/stream/video/coop_ipc.c \
 *       -o /tmp/test_coop_coord_concurrency && \
 *       timeout 20s /tmp/test_coop_coord_concurrency
 */
#include "coop_coord.h"
#include "coop_ipc.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SOCK "/tmp/test_coop_coord_concurrency.sock"

static int failures = 0;

static void check(bool condition, const char *description) {
    printf("  [%s] %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) { failures++; }
}

static uint64_t test_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

static bool wait_bool(const atomic_bool *value) {
    const uint64_t deadline = test_now_ms() + COOP_IPC_IO_DEADLINE_MS * 3u;
    while (!atomic_load(value) && test_now_ms() < deadline) { usleep(1000); }
    return atomic_load(value);
}

static bool read_exact(intptr_t fd, unsigned char *buf, size_t len, uint64_t deadline) {
    size_t got = 0;
    while (got < len && test_now_ms() < deadline) {
        const ssize_t n = read(fd, buf + got, len - got);
        if (n > 0) {
            got += (size_t) n;
        } else if (n == 0) {
            return false;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            usleep(1000);
        } else {
            return false;
        }
    }
    return got == len;
}

typedef struct {
    atomic_bool controls_read;
    atomic_bool send_partial;
    atomic_bool partial_written;
    atomic_bool saw_close;
    uint8_t controls[2];
} client_state_t;

static void *client_main(void *opaque) {
    client_state_t *state = opaque;
    intptr_t fd = coop_ipc_connect(SOCK);
    if (fd < 0 || !coop_ipc_set_nonblocking(fd)) {
        if (fd >= 0) { close(fd); }
        return NULL;
    }

    bool controls_ok = true;
    const uint64_t deadline = test_now_ms() + COOP_IPC_IO_DEADLINE_MS * 3u;
    for (unsigned i = 0; i < 2 && controls_ok; ++i) {
        coop_ipc_hdr_t hdr;
        unsigned char code = 0;
        controls_ok = read_exact(fd, (unsigned char *) &hdr, sizeof(hdr), deadline) &&
                      hdr.magic == COOP_IPC_MAGIC && hdr.frame == COOP_IPC_CTL_FRAME &&
                      hdr.length == 1 && read_exact(fd, &code, 1, deadline);
        state->controls[i] = code;
    }
    if (!controls_ok) {
        close(fd);
        return NULL;
    }
    atomic_store(&state->controls_read, true);
    while (!atomic_load(&state->send_partial) && test_now_ms() < deadline) { usleep(1000); }
    if (!atomic_load(&state->send_partial)) {
        close(fd);
        return NULL;
    }

    const coop_ipc_hdr_t header = {
            .magic = COOP_IPC_MAGIC,
            .length = 64,
            .frame = 777,
    };
    if (write(fd, &header, sizeof(header) / 2u) != (ssize_t) (sizeof(header) / 2u)) {
        close(fd);
        return NULL;
    }
    atomic_store(&state->partial_written, true);

    unsigned char byte;
    const uint64_t close_deadline = test_now_ms() + COOP_IPC_IO_DEADLINE_MS * 3u;
    while (test_now_ms() < close_deadline) {
        const ssize_t n = read(fd, &byte, 1);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            atomic_store(&state->saw_close, true);
            break;
        }
        usleep(1000);
    }
    close(fd);
    return NULL;
}

int main(void) {
    unlink(SOCK);
    const bool idle_started = coop_coord_start(SOCK);
    const uint64_t idle_stop_started = test_now_ms();
    if (idle_started) { coop_coord_stop(); }
    const bool idle_stopped_promptly = idle_started &&
            test_now_ms() - idle_stop_started < COOP_IPC_IO_DEADLINE_MS;

    bool all_started = true;
    bool all_connected = true;
    bool all_controls = true;
    bool all_partial = true;
    bool all_stopped_promptly = true;
    bool all_clients_closed = true;

    for (unsigned iteration = 0; iteration < 8; ++iteration) {
        unlink(SOCK);
        if (!coop_coord_start(SOCK)) {
            all_started = false;
            break;
        }
        client_state_t state = {0};
        pthread_t client;
        if (pthread_create(&client, NULL, client_main, &state) != 0) {
            all_started = false;
            coop_coord_stop();
            break;
        }

        const uint64_t connect_deadline = test_now_ms() + COOP_IPC_IO_DEADLINE_MS * 3u;
        while (!coop_coord_connected() && test_now_ms() < connect_deadline) { usleep(1000); }
        if (!coop_coord_connected()) { all_connected = false; }

        coop_coord_request_peer_idr_keep();
        coop_coord_request_peer_shutdown();
        if (!wait_bool(&state.controls_read) ||
            state.controls[0] != COOP_IPC_CTL_REQUEST_IDR ||
            state.controls[1] != COOP_IPC_CTL_SHUTDOWN) {
            all_controls = false;
        }
        atomic_store(&state.send_partial, true);
        if (!wait_bool(&state.partial_written)) { all_partial = false; }

        const uint64_t stop_started = test_now_ms();
        coop_coord_stop();
        if (test_now_ms() - stop_started >= COOP_IPC_IO_DEADLINE_MS) {
            all_stopped_promptly = false;
        }
        pthread_join(client, NULL);
        if (!atomic_load(&state.saw_close)) { all_clients_closed = false; }
    }
    unlink(SOCK);

    check(idle_started, "coordinator starts without a waiting peer");
    check(idle_stopped_promptly, "non-blocking accept lets an idle coordinator stop promptly");
    check(all_started, "eight coordinator lifecycle iterations start cleanly");
    check(all_connected, "each worker publishes the accepted peer");
    check(all_controls, "atomic requests preserve IDR/shutdown framing and ordering");
    check(all_partial, "each worker enters a partial framed read before teardown");
    check(all_stopped_promptly, "stop wakes and joins before the message deadline");
    check(all_clients_closed, "each client observes the worker-owned peer close");
    printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

/* A co-op secondary that refuses to start says why over the control channel and exits at once. Its reason must
 * reach the user even when it exits before the coordinator's worker has accepted the connection or read the
 * frame: the bytes are still in the socket. Before 2026-10-15 the start-up wait saw the child gone first and
 * reported "did not begin streaming in time" whatever the secondary had said (the Android self-test's
 * co-op handshake, peer exit 5 after 43 ms, 2026-10-15).
 *
 * The secondary here is this program re-executed with --coop-secondary, as the app execs itself: it connects,
 * sends COOP_IPC_CTL_FAIL_HOST and exits 5, exactly as coop_secondary.c does for a host that is not paired. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"

#include "stream/video/coop_coord.h"
#include "stream/video/coop_ipc.h"

static char self_exe[PATH_MAX];
static char socket_path[128];

void setUp(void) {
}

void tearDown(void) {
}

static int fake_secondary(const char *sock) {
    const intptr_t fd = coop_ipc_connect(sock);
    if (fd < 0) {
        return 3;
    }
    coop_ipc_send_ctl(fd, COOP_IPC_CTL_FAIL_HOST);
    return 5;   /* no close: the process ends as the real one does, its socket closed by the kernel */
}

static void testRefusalThatExitsAtOnceIsStillReported(void) {
    for (int i = 0; i < 20; ++i) {
        unlink(socket_path);
        TEST_ASSERT_TRUE(coop_coord_start(socket_path));
        TEST_ASSERT_GREATER_THAN_INT(0, coop_coord_spawn_secondary(self_exe, socket_path));
        const bool ready = coop_coord_wait_secondary_ready(5000);
        const char *why = coop_coord_start_error_message();
        coop_coord_reap_secondary();
        coop_coord_stop();
        char what[256];
        snprintf(what, sizeof(what), "attempt %d said: %s", i + 1, why);
        TEST_ASSERT_FALSE_MESSAGE(ready, what);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(why, "no longer paired"), what);
    }
}

/* A child that dies without saying anything still reads as the connection failing, promptly. */
static void testSilentDeathStillReadsAsConnectionFailure(void) {
    unlink(socket_path);
    TEST_ASSERT_TRUE(coop_coord_start(socket_path));
    TEST_ASSERT_GREATER_THAN_INT(0, coop_coord_spawn_secondary("/bin/false", socket_path));
    const bool ready = coop_coord_wait_secondary_ready(5000);
    const char *why = coop_coord_start_error_message();
    coop_coord_reap_secondary();
    coop_coord_stop();
    TEST_ASSERT_FALSE(ready);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(why, "did not begin streaming in time"), why);
}

int main(int argc, char *argv[]) {
    if (argc == 3 && strcmp(argv[1], "--coop-secondary") == 0) {
        return fake_secondary(argv[2]);
    }
    const ssize_t n = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
    if (n <= 0) {
        return 2;
    }
    self_exe[n] = '\0';
    snprintf(socket_path, sizeof(socket_path), "/tmp/test_coop_start_error.%d.sock", (int) getpid());
    UNITY_BEGIN();
    RUN_TEST(testRefusalThatExitsAtOnceIsStillReported);
    RUN_TEST(testSilentDeathStillReadsAsConnectionFailure);
    unlink(socket_path);
    return UNITY_END();
}

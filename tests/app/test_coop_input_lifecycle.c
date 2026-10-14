#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <pthread.h>

#include "unity.h"

#include "app.h"
#include "app_launch.h"
#include "stream/coop_msg_channel.h"
#include "stream/input/coop_input.h"

static CONFIGURATION config;
static intptr_t peer_fd = -1;
static coop_msg_channel_t peer_channel = COOP_MSG_CHANNEL_INITIALIZER;

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_MONOTONIC, &ts));
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

static void close_peer(void) {
    if (peer_fd >= 0) {
        coop_msg_channel_detach(&peer_channel);
        close(peer_fd);
        peer_fd = -1;
    }
}

static void connect_peer(void) {
    coop_input_coord_start(NULL);
    TEST_ASSERT_TRUE(coop_input_coord_active());

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
#ifdef ECLIPSE_RUNTIME_ACTIVE
    /* The coordinator listens where the runtime-files layer puts its sockets (the app's private runtime
     * directory, platform/posix/runtime_files.c), not at the literal /tmp name: connect where it does. */
    TEST_ASSERT_EQUAL_INT(0, eclipse_runtime_socket_path(COOP_INPUT_SOCKET, addr.sun_path, sizeof(addr.sun_path)));
#else
    TEST_ASSERT_LESS_THAN_size_t(sizeof(addr.sun_path), strlen(COOP_INPUT_SOCKET));
    strcpy(addr.sun_path, COOP_INPUT_SOCKET);
#endif

    peer_fd = coop_msg_socket();
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(0, peer_fd);
    TEST_ASSERT_EQUAL_INT(0, connect(peer_fd, (struct sockaddr *) &addr, sizeof(addr)));
    TEST_ASSERT_TRUE(coop_msg_set_nonblocking(peer_fd));
    TEST_ASSERT_TRUE(coop_msg_channel_attach(&peer_channel, peer_fd,
                                              sizeof(coop_input_msg_t)));

    const uint64_t deadline = monotonic_ms() + 1000;
    while (!coop_input_peer_attached() && monotonic_ms() < deadline) {
        coop_input_coord_pump();
        usleep(1000);
    }
    TEST_ASSERT_TRUE(coop_input_peer_attached());
}

void setUp(void) {
    memset(&config, 0, sizeof(config));
    config.coop_pad_pane[0] = COOP_PAD_SECONDARY;
    app_configuration = &config;
    app_launch_set_coop_requested(true);
    coop_input_coord_stop();
    close_peer();
}

void tearDown(void) {
    coop_input_secondary_stop();
    coop_input_coord_stop();
    close_peer();
    app_launch_set_coop_requested(false);
    app_configuration = NULL;
}

static bool receive_one(coop_input_msg_t *out) {
    size_t length = 0;
    const int result = coop_msg_channel_receive(&peer_channel, out, sizeof(*out), &length);
    return result == COOP_MSG_RECV_MESSAGE && length == sizeof(*out);
}

void testBackpressureKeepsFinalStateAndOrderedEdges(void) {
    connect_peer();

    int receive_buffer = 1024;
    (void) setsockopt(peer_fd, SOL_SOCKET, SO_RCVBUF,
                      (const char *) &receive_buffer, sizeof(receive_buffer));

    /* Stop reading until the kernel and framed channel back up. Repeated
     * controller packets are complete state, so only their newest value needs
     * to survive in the bounded application queue. */
    for (unsigned i = 0; i < 200000; ++i) {
        coop_input_send_controller(0, 1, 1, 0, 0,
                                   (short) i, 0, 0, 0);
    }
    TEST_ASSERT_GREATER_THAN_UINT(0, coop_input_dropped());

    enum { EDGE_COUNT = 32 };
    for (int button = 1; button <= EDGE_COUNT; ++button) {
        coop_input_send_mouse_button(button & 1, button);
    }
    /* Controller button changes are edges even though their packets contain full
     * state. Neither side of this tap may coalesce across the touch transition. */
    coop_input_send_controller(0, 1, 2, 0, 0, 0, 0, 0, 0);
    coop_input_send_touchpad(0, 1, 7, 29, 0.25f, 0.75f, 0.5f);
    coop_input_send_controller(0, 1, 0, 0, 0, 0, 0, 0, 0);

    bool saw_release = false;
    int expected_button = 1;
    int controller_edge_phase = 0;
    const uint64_t deadline = monotonic_ms() + 5000;
    while ((!saw_release || expected_button <= EDGE_COUNT) && monotonic_ms() < deadline) {
        coop_input_msg_t message;
        while (receive_one(&message)) {
            TEST_ASSERT_EQUAL_HEX32(COOP_INPUT_MAGIC, message.magic);
            if (message.type == COOP_INPUT_MSG_MOUSE_BUTTON) {
                TEST_ASSERT_EQUAL_INT(expected_button, message.buttons_lo);
                TEST_ASSERT_EQUAL_INT(expected_button & 1, message.sub);
                expected_button++;
            } else if (message.type == COOP_INPUT_MSG_CONTROLLER &&
                       message.buttons_lo == 2) {
                TEST_ASSERT_EQUAL_INT(EDGE_COUNT + 1, expected_button);
                TEST_ASSERT_EQUAL_INT(0, controller_edge_phase);
                controller_edge_phase = 1;
            } else if (message.type == COOP_INPUT_MSG_TOUCHPAD &&
                       message.pointer_id == 29) {
                TEST_ASSERT_EQUAL_INT(1, controller_edge_phase);
                controller_edge_phase = 2;
            } else if (message.type == COOP_INPUT_MSG_CONTROLLER &&
                       message.buttons_lo == 0) {
                TEST_ASSERT_EQUAL_INT(2, controller_edge_phase);
                controller_edge_phase = 3;
                saw_release = true;
            }
        }
        coop_input_coord_pump();
        usleep(100);
    }

    TEST_ASSERT_TRUE(saw_release);
    TEST_ASSERT_EQUAL_INT(EDGE_COUNT + 1, expected_button);
    TEST_ASSERT_EQUAL_INT(3, controller_edge_phase);
}

static atomic_bool coordinator_spam_run;

static void *coordinator_spam(void *unused) {
    (void) unused;
    unsigned value = 0;
    while (atomic_load_explicit(&coordinator_spam_run, memory_order_acquire)) {
        coop_input_send_controller(0, 1, (short) (value & 1u), 0, 0,
                                   (short) value, 0, 0, 0);
        coop_input_coord_pump();
        value++;
    }
    return NULL;
}

void testCoordinatorStopCannotRaceSendPumpOrReusedFd(void) {
    connect_peer();
    atomic_store_explicit(&coordinator_spam_run, true, memory_order_release);
    pthread_t thread;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, coordinator_spam, NULL));
    usleep(10000);

    coop_input_coord_stop();
    atomic_store_explicit(&coordinator_spam_run, false, memory_order_release);
    TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));
    TEST_ASSERT_FALSE(coop_input_coord_active());
    close_peer();

    /* A new socket is likely to reuse one of the just-closed descriptor values.
     * The old channel must be fully detached before that can happen. */
    connect_peer();
    coop_input_send_mouse_button(1, 7);
    coop_input_msg_t message = {0};
    bool received = false;
    const uint64_t deadline = monotonic_ms() + 1000;
    while (!(received = receive_one(&message)) && monotonic_ms() < deadline) {
        coop_input_coord_pump();
        usleep(1000);
    }
    TEST_ASSERT_TRUE(received);
    TEST_ASSERT_EQUAL_UINT8(COOP_INPUT_MSG_MOUSE_BUTTON, message.type);
    TEST_ASSERT_EQUAL_UINT16(7, message.buttons_lo);
}

static atomic_bool rumble_spam_run;

static void *rumble_spam(void *unused) {
    (void) unused;
    uint16_t value = 0;
    while (atomic_load_explicit(&rumble_spam_run, memory_order_acquire)) {
        coop_input_secondary_rumble(0, value, (uint16_t) ~value);
        value++;
    }
    return NULL;
}

void testSecondaryStopDisablesCallbacksBeforeDetach(void) {
    coop_input_coord_start(NULL);
    TEST_ASSERT_TRUE(coop_input_secondary_start());
    const uint64_t deadline = monotonic_ms() + 1000;
    while (!coop_input_peer_attached() && monotonic_ms() < deadline) {
        coop_input_coord_pump();
        usleep(1000);
    }
    TEST_ASSERT_TRUE(coop_input_peer_attached());

    atomic_store_explicit(&rumble_spam_run, true, memory_order_release);
    pthread_t thread;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, rumble_spam, NULL));
    usleep(10000);
    coop_input_secondary_stop();
    atomic_store_explicit(&rumble_spam_run, false, memory_order_release);
    TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));

    /* A late Limelight callback after stop is an intentional no-op. */
    coop_input_secondary_rumble(0, 1, 2);
}

/* The argc/argv form: on Windows SDL renames main to SDL_main(int, char **), and `int main(void)`
 * then fails to compile as a conflicting declaration. */
int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;
    UNITY_BEGIN();
    RUN_TEST(testBackpressureKeepsFinalStateAndOrderedEdges);
    RUN_TEST(testCoordinatorStopCannotRaceSendPumpOrReusedFd);
    RUN_TEST(testSecondaryStopDisablesCallbacksBeforeDetach);
    return UNITY_END();
}

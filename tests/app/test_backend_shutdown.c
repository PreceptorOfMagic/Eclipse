/* Isolated lifetime test: actual backend_destroy + executor, fake discovery/manager.
 * An in-flight request resumes during shutdown and must still own a live manager
 * and GameStream mutex. No network, display, or host is used.
 *
 * cc -std=gnu11 -O1 -g -Isrc/app -Icore/libgamestream -Icore/moonlight-common-c/src \
 *   -Ithird_party/commons/util/executor -Ithird_party/commons/util/logging \
 *   -Ithird_party/commons/util/linked-list -Ithird_party/commons/util/uuidstr \
 *   -Ithird_party/commons/net/sockaddr -Ithird_party/commons/net/hostport \
 *   $(sdl2-config --cflags) tests/app/test_backend_shutdown.c \
 *   src/app/backend/backend_root.c third_party/commons/util/executor/executor.c \
 *   $(sdl2-config --libs) -o /tmp/test_backend_shutdown
 * timeout 10s /tmp/test_backend_shutdown
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <SDL.h>

#include "backend/backend_root.h"
#include "backend/pcmanager.h"
#include "executor.h"
#include "logging.h"

struct pcmanager_t { bool alive; };
static struct pcmanager_t manager;
static SDL_sem *entered;
static SDL_sem *release;
static SDL_atomic_t worker_finished;
static bool discovery_stopped;
static bool pending_callbacks_drained;

pcmanager_t *pcmanager_new(app_t *app, executor_t *executor) {
    (void) app;
    (void) executor;
    manager.alive = true;
    return &manager;
}

void pcmanager_auto_discovery_stop(pcmanager_t *value) {
    assert(value == &manager && value->alive);
    discovery_stopped = true;
}

void app_bus_drain(void) {
    assert(discovery_stopped && manager.alive);
    pending_callbacks_drained = true;
}

void pcmanager_destroy(pcmanager_t *value) {
    assert(value == &manager && value->alive);
    assert(SDL_AtomicGet(&worker_finished));
    assert(discovery_stopped && pending_callbacks_drained);
    value->alive = false;
}

void commons_log_printf(commons_log_level level, const char *tag, const char *fmt, ...) {
    (void) level;
    (void) tag;
    (void) fmt;
}

static int in_flight_request(void *arg) {
    app_backend_t *backend = arg;
    SDL_SemPost(entered);
    assert(SDL_SemWaitTimeout(release, 2000) == 0);
    assert(manager.alive);
    assert(SDL_LockMutex(backend->gs_client_mutex) == 0);
    assert(SDL_UnlockMutex(backend->gs_client_mutex) == 0);
    SDL_AtomicSet(&worker_finished, 1);
    return 0;
}

static int finish_request(void *arg) {
    (void) arg;
    SDL_Delay(100);
    SDL_SemPost(release);
    return 0;
}

int main(void) {
    assert(SDL_Init(0) == 0);
    entered = SDL_CreateSemaphore(0);
    release = SDL_CreateSemaphore(0);
    assert(entered && release);
    app_backend_t backend;
    backend_init(&backend, NULL);
    assert(executor_submit(backend.executor, in_flight_request, NULL, &backend));
    assert(SDL_SemWaitTimeout(entered, 2000) == 0);
    SDL_Thread *finisher = SDL_CreateThread(finish_request, "finish-test", NULL);
    assert(finisher);
    backend_destroy(&backend);
    SDL_WaitThread(finisher, NULL);
    assert(!manager.alive && backend.executor == NULL && backend.gs_client_mutex == NULL);
    SDL_DestroySemaphore(entered);
    SDL_DestroySemaphore(release);
    SDL_Quit();
    puts("PASS: in-flight request finishes before manager and GameStream mutex are destroyed");
    return 0;
}

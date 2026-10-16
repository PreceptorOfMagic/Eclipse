#include "discovery.h"
#include "throttle.h"

#include "impl/impl.h"

#include "util/bus.h"
#include "logging.h"

static int discovery_worker_wrapper(void *arg);

#if TARGET_ANDROID
#include <pthread.h>

/* NO JOIN WHILE EVENTS ARE DISPATCHED. app_process_events() hands every event to the app through
 * SDL_FilterEvents, which holds SDL's event-queue lock for the whole pass. Opening a stream there closes
 * the launcher, which stops discovery and joined its thread -- and a discovery thread that had just found
 * a host was itself waiting for that lock in SDL_PushEvent. Neither could go on: the co-op dialog froze on
 * "Checking ..." and the stream never started (SHIELD TV, 2026-10-16; stacks from a bug report). While the
 * pass runs, a stopped task is only told to stop; app_process_events() waits for it once the lock is free. */
static pthread_mutex_t deferred_lock = PTHREAD_MUTEX_INITIALIZER;
static discovery_task_t *deferred_tasks[8];
static int deferred_count = 0;
static bool events_dispatching = false;

static void discovery_task_finish(discovery_task_t *task) {
    SDL_WaitThread(task->thread, NULL);
    SDL_DestroyMutex(task->lock);
    SDL_free(task);
}

static void discovery_reap_deferred(void) {
    for (;;) {
        pthread_mutex_lock(&deferred_lock);
        discovery_task_t *task = deferred_count > 0 ? deferred_tasks[--deferred_count] : NULL;
        pthread_mutex_unlock(&deferred_lock);
        if (task == NULL) {
            return;
        }
        discovery_task_finish(task);
    }
}

void discovery_events_dispatching(bool dispatching) {
    pthread_mutex_lock(&deferred_lock);
    events_dispatching = dispatching;
    pthread_mutex_unlock(&deferred_lock);
    if (!dispatching) {
        discovery_reap_deferred();
    }
}
#endif

void discovery_init(discovery_t *discovery, discovery_callback callback, void *user_data) {
    discovery->lock = SDL_CreateMutex();
    discovery->task = NULL;
    discovery_throttle_init(&discovery->throttle, callback, user_data);
}

void discovery_deinit(discovery_t *discovery) {
    discovery_stop(discovery);
#if TARGET_ANDROID
    discovery_reap_deferred();      /* a task stopped during an event pass still uses this discovery */
#endif
    discovery_throttle_deinit(&discovery->throttle);
    SDL_DestroyMutex(discovery->lock);
}

void discovery_start(discovery_t *discovery) {
    SDL_LockMutex(discovery->lock);
    if (discovery->task != NULL) {
        SDL_UnlockMutex(discovery->lock);
        return;
    }
    discovery_task_t *task = SDL_calloc(1, sizeof(discovery_task_t));
    if (task == NULL) {
        commons_log_error("Discovery", "Could not allocate discovery task");
        SDL_UnlockMutex(discovery->lock);
        return;
    }
    commons_log_info("Discovery", "Start task %p", task);
    task->discovery = discovery;
    task->lock = SDL_CreateMutex();
    if (task->lock == NULL) {
        commons_log_error("Discovery", "Could not create discovery task mutex: %s", SDL_GetError());
        SDL_free(task);
        SDL_UnlockMutex(discovery->lock);
        return;
    }
    task->stop = false;
    task->thread = SDL_CreateThread(discovery_worker_wrapper, "discovery", task);
    if (task->thread == NULL) {
        commons_log_error("Discovery", "Could not start discovery thread: %s", SDL_GetError());
        SDL_DestroyMutex(task->lock);
        SDL_free(task);
        SDL_UnlockMutex(discovery->lock);
        return;
    }
    discovery->task = task;
    SDL_UnlockMutex(discovery->lock);
}

void discovery_stop(discovery_t *discovery) {
    SDL_LockMutex(discovery->lock);
    discovery_task_t *task = discovery->task;
    if (task == NULL) {
        SDL_UnlockMutex(discovery->lock);
        return;
    }
    discovery->task = NULL;
    discovery_worker_stop(task);
    SDL_UnlockMutex(discovery->lock);
#if TARGET_ANDROID
    pthread_mutex_lock(&deferred_lock);
    const bool defer = events_dispatching && deferred_count < (int) (sizeof(deferred_tasks) / sizeof(deferred_tasks[0]));
    if (defer) {
        deferred_tasks[deferred_count++] = task;
    }
    pthread_mutex_unlock(&deferred_lock);
    if (defer) {
        return;
    }
#endif
    SDL_WaitThread(task->thread, NULL);
    SDL_DestroyMutex(task->lock);
    SDL_free(task);
}

void discovery_discovered(struct discovery_t *discovery, const sockaddr_t *addr) {
    discovery_throttle_on_discovered(&discovery->throttle, addr, 10000);
}

int discovery_worker_wrapper(void *arg) {
    discovery_task_t *task = (discovery_task_t *) arg;
    return discovery_worker(task);
}

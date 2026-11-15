#include "discovery.h"
#include "throttle.h"

#include "impl/impl.h"

#include "util/bus.h"
#include "logging.h"

static int discovery_worker_wrapper(void *arg);

/* NO JOIN WHILE EVENTS ARE DISPATCHED (every platform since 2026-11-14; Android only before). app_process_events() hands every event to the app through
 * SDL_FilterEvents, which holds SDL's event-queue lock for the whole pass. Opening a stream there closes
 * the launcher, which stops discovery and joined its thread -- and a discovery thread that had just found
 * a host was itself waiting for that lock in SDL_PushEvent. Neither could go on: the co-op dialog froze on
 * "Checking ..." and the stream never started (SHIELD TV, 2026-10-16; stacks from a bug report). While the
 * pass runs, a stopped task is only told to stop; app_process_events() waits for it once the lock is free.
 * SDL_FilterEvents is the same on every SDL platform, and so was the deadlock: MEASURED 2026-11-14 on the Linux
 * Flatpak (co-op started 10 s after launch, VEGA just discovered): main thread in discovery_stop under
 * app_event_filter, discovery and session threads in bus_pushevent (gdb stack scan), dialog frozen on "Checking". */
static SDL_SpinLock deferred_lock = 0;
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
        SDL_AtomicLock(&deferred_lock);
        discovery_task_t *task = deferred_count > 0 ? deferred_tasks[--deferred_count] : NULL;
        SDL_AtomicUnlock(&deferred_lock);
        if (task == NULL) {
            return;
        }
        discovery_task_finish(task);
    }
}

void discovery_events_dispatching(bool dispatching) {
    SDL_AtomicLock(&deferred_lock);
    events_dispatching = dispatching;
    SDL_AtomicUnlock(&deferred_lock);
    if (!dispatching) {
        discovery_reap_deferred();
    }
}

void discovery_init(discovery_t *discovery, discovery_callback callback, void *user_data) {
    discovery->lock = SDL_CreateMutex();
    discovery->task = NULL;
    discovery_throttle_init(&discovery->throttle, callback, user_data);
}

void discovery_deinit(discovery_t *discovery) {
    discovery_stop(discovery);
    discovery_reap_deferred();      /* a task stopped during an event pass still uses this discovery */
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
    SDL_AtomicLock(&deferred_lock);
    const bool defer = events_dispatching && deferred_count < (int) (sizeof(deferred_tasks) / sizeof(deferred_tasks[0]));
    if (defer) {
        deferred_tasks[deferred_count++] = task;
    }
    SDL_AtomicUnlock(&deferred_lock);
    if (defer) {
        return;
    }
    SDL_WaitThread(task->thread, NULL);
    SDL_DestroyMutex(task->lock);
    SDL_free(task);
}

void discovery_discovered(struct discovery_t *discovery, const sockaddr_t *addr) {
    discovery_throttle_on_discovered(&discovery->throttle, addr, 10000);
}

int discovery_worker_wrapper(void *arg) {
    discovery_task_t *task = (discovery_task_t *) arg;
#if defined(_WIN32) || (defined(OS_LINUX) && !defined(TARGET_WEBOS) && !defined(TARGET_ANDROID))
    /* THIS PC'S OWN HOST IS ASKED DIRECTLY, NOT ONLY OVER mDNS. An unpaired host is never saved, so for
     * host-and-client co-op this PC entered the list only if its own mDNS answer arrived while discovery ran.
     * MEASURED 2026-11-15 on the Linux Flatpak (VEGA, Umbra up on 47989): one launch listed it, the next did
     * not, and the co-op dialog offered only the two other PCs. Loopback is "this PC" for
     * local_host_is_this_pc(); nothing listening costs one refused connection. Same throttle as mDNS. */
    {
        sockaddr_t *self = sockaddr_new();
        if (self != NULL) {
            struct in_addr loopback;
            loopback.s_addr = htonl(INADDR_LOOPBACK);
            if (sockaddr_set_ip(self, AF_INET, &loopback) == 0) {
                sockaddr_set_port(self, 47989);
                discovery_discovered(task->discovery, self);
            }
            sockaddr_free(self);
        }
    }
#endif
    return discovery_worker(task);
}

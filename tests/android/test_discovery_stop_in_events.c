/*
 * Stopping discovery from inside the app's event pass, while its thread is pushing an event (backend/pcmanager/
 * discovery/discovery.c, compiled here with TARGET_ANDROID).
 *
 * app_process_events() runs every event through SDL_FilterEvents, which holds SDL's event-queue lock for the whole
 * pass. On the SHIELD TV, opening a stream inside that pass stopped discovery and joined its thread while the thread
 * was waiting for that lock in SDL_PushEvent: both waited forever and the co-op dialog froze (2026-10-16). Here a fake
 * discovery worker pushes events until it is told to stop, and discovery is stopped from inside SDL_FilterEvents.
 * The stop must return; the thread must be joined once the pass is over. A watchdog fails the test if it hangs.
 */
#include DISCOVERY_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* --- fakes: the worker and the throttle ------------------------------------------------------------------------ */
static SDL_atomic_t worker_pushing, worker_exited;

int discovery_worker(discovery_task_t *task) {
    for (;;) {
        SDL_LockMutex(task->lock);
        const bool stop = task->stop;
        SDL_UnlockMutex(task->lock);
        if (stop) {
            break;
        }
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_USEREVENT;
        SDL_AtomicSet(&worker_pushing, 1);
        SDL_PushEvent(&e);                     /* a found host: posted to the main loop, as pclist_upsert_async does */
        SDL_Delay(1);
    }
    SDL_AtomicSet(&worker_exited, 1);
    return 0;
}

void discovery_worker_stop(discovery_task_t *task) {
    SDL_LockMutex(task->lock);
    task->stop = true;
    SDL_UnlockMutex(task->lock);
}

void discovery_throttle_init(discovery_throttle_t *throttle, discovery_callback callback, void *user_data) {
    (void) throttle; (void) callback; (void) user_data;
}
void discovery_throttle_deinit(discovery_throttle_t *throttle) { (void) throttle; }
void discovery_throttle_on_discovered(discovery_throttle_t *throttle, const sockaddr_t *addr, uint32_t ttl) {
    (void) throttle; (void) addr; (void) ttl;
}
void commons_log_printf(commons_log_level level, const char *tag, const char *fmt, ...) {
    (void) level; (void) tag; (void) fmt;
}

/* --- the event pass --------------------------------------------------------------------------------------------- */
static discovery_t discovery;
static int stopped_in_pass;

static int filter(void *userdata, SDL_Event *event) {
    (void) userdata;
    if (event->type == SDL_USEREVENT && !stopped_in_pass) {
        stopped_in_pass = 1;
        /* Make sure the worker is inside SDL_PushEvent, i.e. waiting for the lock this pass holds. */
        SDL_Delay(20);
        discovery_stop(&discovery);           /* "the launcher closes": must not wait for the thread here */
    }
    return 0;
}

static int watchdog(void *arg) {
    (void) arg;
    SDL_Delay(10000);
    fprintf(stderr, "FAIL: stopping discovery inside the event pass deadlocked\n");
    _exit(1);
}

int main(void) {
    if (SDL_Init(SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_DetachThread(SDL_CreateThread(watchdog, "watchdog", NULL));
    discovery_init(&discovery, NULL, NULL);
    discovery_start(&discovery);
    while (!SDL_AtomicGet(&worker_pushing)) {
        SDL_Delay(1);
    }
    SDL_Delay(10);

    /* One pass of app_process_events(), as the Android build brackets it. */
    SDL_PumpEvents();
    discovery_events_dispatching(true);
    SDL_FilterEvents(filter, NULL);
    discovery_events_dispatching(false);

    int failures = 0;
    if (!stopped_in_pass) {
        fprintf(stderr, "FAIL: no event reached the pass\n");
        failures++;
    }
    if (!SDL_AtomicGet(&worker_exited)) {
        fprintf(stderr, "FAIL: the discovery thread was not joined after the pass\n");
        failures++;
    }
    if (discovery.task != NULL) {
        fprintf(stderr, "FAIL: discovery still has a task after stop\n");
        failures++;
    }
    /* Started again and stopped outside a pass: joined at once, as before. */
    SDL_AtomicSet(&worker_exited, 0);
    discovery_start(&discovery);
    discovery_stop(&discovery);
    if (!SDL_AtomicGet(&worker_exited)) {
        fprintf(stderr, "FAIL: a stop outside the pass did not join the thread\n");
        failures++;
    }
    discovery_deinit(&discovery);
    SDL_Quit();
    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

/*
 * QUITTING WITH A COVER STILL LOADING MUST NOT HANG THE EXIT.
 *
 * Measured 2026-09-26 on Windows (gdb on a process whose window had closed but which never
 * exited): the main thread in app_deinit -> backend_destroy -> executor_destroy, joining the
 * "moonlight-io" worker; that worker in img_loader's run_on_main, waiting for the main thread
 * to run a cover callback. The app bus had refused the callback (app_bus_post returns false once
 * the app stops running) and the loader waited for it anyway.
 *
 * This reproduces that order: a download in flight, the main thread gone, the executor destroyed.
 */
#include <SDL.h>

#include "unity.h"
#include "executor.h"
#include "util/img_loader.h"

struct img_loader_req_t {
    int unused;
};

static SDL_sem *fetch_entered;
static SDL_sem *fetch_release;
static SDL_sem *destroy_done;
static SDL_atomic_t posts;
static SDL_atomic_t callbacks;

static bool never_cached(img_loader_req_t *req) {
    (void) req;
    return false;
}

static void nothing(img_loader_req_t *req) {
    (void) req;
}

static void counted(img_loader_req_t *req) {
    (void) req;
    SDL_AtomicIncRef(&callbacks);
}

/** A download that is still going when the app quits, and then fails (the hang's task ended
 *  with EIO). */
static bool slow_fetch(img_loader_req_t *req) {
    (void) req;
    SDL_SemPost(fetch_entered);
    SDL_SemWait(fetch_release);
    return false;
}

/** The app has stopped running: its bus takes nothing more, exactly as app_bus_post() does. */
static bool main_thread_gone(img_loader_t *loader, img_loader_run_on_main_fn fn, void *args) {
    (void) loader;
    (void) fn;
    (void) args;
    SDL_AtomicIncRef(&posts);
    return false;
}

static int destroy_executor(void *executor) {
    executor_destroy(executor);
    SDL_SemPost(destroy_done);
    return 0;
}

void setUp(void) {
}

void tearDown(void) {
}

void testQuitWithACoverLoadingDoesNotHang(void) {
    fetch_entered = SDL_CreateSemaphore(0);
    fetch_release = SDL_CreateSemaphore(0);
    destroy_done = SDL_CreateSemaphore(0);
    SDL_AtomicSet(&posts, 0);
    SDL_AtomicSet(&callbacks, 0);

    executor_t *executor = executor_create("test-io", 1);
    const img_loader_impl_t impl = {
            .memcache_get = never_cached,
            .memcache_put = nothing,
            .filecache_get = never_cached,
            .filecache_put = nothing,
            .fetch = slow_fetch,
            .run_on_main = main_thread_gone,
    };
    img_loader_t *loader = img_loader_create(&impl, executor);
    const img_loader_cb_t cb = {
            .start_cb = nothing,
            .fail_cb = counted,
            .complete_cb = counted,
            .cancel_cb = counted,
    };
    struct img_loader_req_t req = {0};
    TEST_ASSERT_NOT_NULL(img_loader_load(loader, &req, &cb));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, SDL_SemWaitTimeout(fetch_entered, 5000), "the download never started");

    /* app_deinit's order: the main loop has ended, then the backend's executor is destroyed,
     * which joins the worker still inside this download. Destroyed on its own thread so a hang
     * fails the test instead of stopping the run. */
    SDL_Thread *destroyer = SDL_CreateThread(destroy_executor, "destroyer", executor);
    SDL_SemPost(fetch_release);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, SDL_SemWaitTimeout(destroy_done, 5000),
                                  "executor_destroy never returned: the worker is waiting for a "
                                  "main thread that is no longer running");
    SDL_WaitThread(destroyer, NULL);

    /* It did try to reach the main thread, and ran nothing it could not hand over. */
    TEST_ASSERT_TRUE(SDL_AtomicGet(&posts) >= 1);
    TEST_ASSERT_EQUAL_INT(0, SDL_AtomicGet(&callbacks));

    SDL_DestroySemaphore(fetch_entered);
    SDL_DestroySemaphore(fetch_release);
    SDL_DestroySemaphore(destroy_done);
}

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;
    UNITY_BEGIN();
    RUN_TEST(testQuitWithACoverLoadingDoesNotHang);
    return UNITY_END();
}

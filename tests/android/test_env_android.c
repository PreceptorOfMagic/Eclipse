/* src/app/platform/android/env_android.c: another thread may read the environment while this program changes it.
 *
 * A reader walks environ as getenv does, in instrumented code, while a writer adds and removes names through
 * setenv/unsetenv. Built with -fsanitize=address and -Wl,--wrap=setenv,--wrap=unsetenv against env_android.c it
 * must run clean: a wrapper that freed an array or a string a reader may hold fails here. (The race itself is
 * Bionic's, seen on the SHIELD TV as SIGSEGV in getenv on RenderThread, 2026-10-15; the host's glibc 2.43 did not
 * show it when this test was run against it.) The wrapper's results are left in place by design, so leak detection
 * is off for this test. Also checks that setenv/unsetenv behave as the C library's do for this program's uses. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

static atomic_bool done;
static atomic_ulong reads;

static void *reader(void *arg) {
    (void) arg;
    size_t sink = 0;
    while (!atomic_load(&done)) {
        char **env = __atomic_load_n(&environ, __ATOMIC_ACQUIRE);
        for (; env != NULL && *env != NULL; env++) {
            sink += strlen(*env);
        }
        atomic_fetch_add(&reads, 1);
    }
    return (void *) sink;
}

static int failures = 0;
static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

int main(void) {
    /* The semantics this program relies on. */
    check(setenv("ECLIPSE_ENV_TEST", "one", 1) == 0 && strcmp(getenv("ECLIPSE_ENV_TEST"), "one") == 0, "set a new name");
    check(setenv("ECLIPSE_ENV_TEST", "two", 0) == 0 && strcmp(getenv("ECLIPSE_ENV_TEST"), "one") == 0, "no overwrite");
    check(setenv("ECLIPSE_ENV_TEST", "three", 1) == 0 && strcmp(getenv("ECLIPSE_ENV_TEST"), "three") == 0, "overwrite");
    check(unsetenv("ECLIPSE_ENV_TEST") == 0 && getenv("ECLIPSE_ENV_TEST") == NULL, "unset");
    check(unsetenv("ECLIPSE_ENV_TEST") == 0, "unset an absent name");
    check(setenv("BAD=NAME", "x", 1) == -1 && setenv("", "x", 1) == -1, "invalid names are refused");
    check(setenv("ECLIPSE_ENV_TEST_A", "a", 1) == 0 && setenv("ECLIPSE_ENV_TEST_B", "b", 1) == 0 &&
          unsetenv("ECLIPSE_ENV_TEST_A") == 0 && strcmp(getenv("ECLIPSE_ENV_TEST_B"), "b") == 0 &&
          getenv("ECLIPSE_ENV_TEST_A") == NULL, "unset leaves the other names");
    unsetenv("ECLIPSE_ENV_TEST_B");

    /* Readers while the environment changes. */
    pthread_t threads[2];
    for (int i = 0; i < 2; i++) {
        pthread_create(&threads[i], NULL, reader, NULL);
    }
    char name[32], value[32];
    for (int round = 0; round < 60; round++) {
        for (int i = 0; i < 200; i++) {
            snprintf(name, sizeof(name), "ECLIPSE_ENV_RACE_%d", i);
            snprintf(value, sizeof(value), "%d-%d", round, i);
            setenv(name, value, 1);
            setenv(name, value, 1);   /* a new value for an existing name too */
        }
        for (int i = 0; i < 200; i++) {
            snprintf(name, sizeof(name), "ECLIPSE_ENV_RACE_%d", i);
            unsetenv(name);
        }
    }
    atomic_store(&done, true);
    for (int i = 0; i < 2; i++) {
        pthread_join(threads[i], NULL);
    }
    check(atomic_load(&reads) > 0, "the readers ran");
    check(getenv("ECLIPSE_ENV_RACE_0") == NULL, "every race name removed");
    printf("%s: %lu environment walks during 24000 changes\n", failures == 0 ? "PASS" : "FAIL",
           (unsigned long) atomic_load(&reads));
    return failures == 0 ? 0 : 1;
}

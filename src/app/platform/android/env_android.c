/*
 * setenv and unsetenv for the Android build, which links libmain.so, the co-op peer and the MediaCodec module with
 * -Wl,--wrap=setenv,--wrap=unsetenv (CMakeLists.txt): every call this program makes comes here.
 *
 * Bionic's setenv (OpenBSD's) reallocates the environ array to add a name, freeing the old one, and getenv takes
 * no lock. That is safe only while nothing else reads the environment, and on Android something always does: the
 * framework's RenderThread initialises EGL as the activity first draws, at the moment SDL_main starts, and NVIDIA's
 * driver calls getenv there. On the SHIELD TV it read a freed array (SIGSEGV in getenv <-
 * NvGlsiSettings::NvGlsiSettings, RenderThread) in one launch out of about twenty (2026-10-15).
 *
 * So nothing a reader may hold is ever freed: a new name or a removed one publishes a new array with one pointer
 * store and leaves the old array, and a new value replaces one pointer in the array and leaves the old string (as
 * Bionic does). What is left is a few hundred bytes per added or removed name. Writers take a lock; readers --
 * getenv anywhere in the process -- need none.
 */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

int __wrap_setenv(const char *name, const char *value, int overwrite);
int __wrap_unsetenv(const char *name);

static pthread_mutex_t env_lock = PTHREAD_MUTEX_INITIALIZER;

static int valid_name(const char *name) {
    return name != NULL && name[0] != '\0' && strchr(name, '=') == NULL;
}

static size_t env_count(char **env) {
    size_t n = 0;
    if (env != NULL) {
        while (env[n] != NULL) {
            n++;
        }
    }
    return n;
}

static long env_find(char **env, const char *name, size_t len) {
    if (env == NULL) {
        return -1;
    }
    for (long i = 0; env[i] != NULL; i++) {
        if (strncmp(env[i], name, len) == 0 && env[i][len] == '=') {
            return i;
        }
    }
    return -1;
}

int __wrap_setenv(const char *name, const char *value, int overwrite) {
    if (!valid_name(name)) {
        errno = EINVAL;
        return -1;
    }
    if (value == NULL) {
        value = "";
    }
    const size_t len = strlen(name), value_len = strlen(value);
    pthread_mutex_lock(&env_lock);
    char **env = environ;
    const long at = env_find(env, name, len);
    if (at >= 0 && !overwrite) {
        pthread_mutex_unlock(&env_lock);
        return 0;
    }
    char *entry = malloc(len + value_len + 2);
    if (entry == NULL) {
        pthread_mutex_unlock(&env_lock);
        errno = ENOMEM;
        return -1;
    }
    memcpy(entry, name, len);
    entry[len] = '=';
    memcpy(entry + len + 1, value, value_len + 1);
    if (at >= 0) {
        __atomic_store_n(&env[at], entry, __ATOMIC_RELEASE);
    } else {
        const size_t n = env_count(env);
        char **next = malloc((n + 2) * sizeof(*next));
        if (next == NULL) {
            free(entry);
            pthread_mutex_unlock(&env_lock);
            errno = ENOMEM;
            return -1;
        }
        if (n > 0) {
            memcpy(next, env, n * sizeof(*next));
        }
        next[n] = entry;
        next[n + 1] = NULL;
        __atomic_store_n(&environ, next, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&env_lock);
    return 0;
}

int __wrap_unsetenv(const char *name) {
    if (!valid_name(name)) {
        errno = EINVAL;
        return -1;
    }
    const size_t len = strlen(name);
    pthread_mutex_lock(&env_lock);
    char **env = environ;
    if (env_find(env, name, len) < 0) {
        pthread_mutex_unlock(&env_lock);
        return 0;
    }
    const size_t n = env_count(env);
    char **next = malloc((n + 1) * sizeof(*next));
    if (next == NULL) {
        pthread_mutex_unlock(&env_lock);
        errno = ENOMEM;
        return -1;
    }
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        if (!(strncmp(env[i], name, len) == 0 && env[i][len] == '=')) {
            next[kept++] = env[i];
        }
    }
    next[kept] = NULL;
    __atomic_store_n(&environ, next, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&env_lock);
    return 0;
}

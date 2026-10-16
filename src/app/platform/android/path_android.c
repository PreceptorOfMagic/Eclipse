/*
 * Android paths. Every location is the app's own, published by EclipseActivity as environment variables
 * (android_platform.h) so the co-op peer -- an exec'd process with no Java VM -- resolves exactly the same
 * directories as the primary:
 *
 *   preferences  Context.getFilesDir()                 moonlight.ini, pairing key, known hosts, kept logs
 *   cache        Context.getCacheDir()/moonlight-tv    cover art; runtime/ for co-op sockets and traces
 *   assets       filesDir/assets-<versionCode>         APK assets, unpacked once per installed version
 *   executable   nativeLibraryDir/libeclipse_peer.so   the co-op peer (see CMakeLists.txt)
 *
 * All of it is removed with the app; nothing is written anywhere shared.
 */
#include "util/path.h"
#include "android_platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *owned_copy(const char *value) {
    if (value == NULL) {
        return NULL;
    }
    const size_t length = strlen(value) + 1;
    char *copy = malloc(length);
    if (copy != NULL) {
        memcpy(copy, value, length);
    }
    return copy;
}

static const char *env_dir(const char *name) {
    const char *value = getenv(name);
    return value != NULL && value[0] == '/' ? value : NULL;
}

static const char *path_user_file(const char *basename) {
    /* Consumed immediately by fopen/stat, as in path_sdl.c: per-thread storage, no ownership at call sites. */
    static _Thread_local char *file_path;
    const char *pref = env_dir(ANDROID_ENV_FILES_DIR);
    char *next = path_join(pref != NULL ? pref : ".", basename);
    if (next != NULL) {
        free(file_path);
        file_path = next;
    }
    return file_path != NULL ? file_path : basename;
}

const char *path_diag_log(void) {
    return path_user_file("aurora-diag.log");
}

const char *path_secondary_log(void) {
    return path_user_file("coop_secondary.log");
}

const char *path_multiview_log(void) {
    return path_user_file("coop_multiview.log");
}

char *path_assets() {
    return owned_copy(env_dir(ANDROID_ENV_ASSETS_DIR));
}

char *path_pref(bool *persistent) {
    const char *files = env_dir(ANDROID_ENV_FILES_DIR);
    if (files == NULL || path_dir_ensure(files) != 0) {
        *persistent = false;
        return NULL;
    }
    *persistent = true;
    return owned_copy(files);
}

char *path_cache() {
    const char *cache = env_dir(ANDROID_ENV_CACHE_DIR);
    if (cache == NULL) {
        return NULL;
    }
    /* A leaf of our own, never the cache root itself: runtime_files.c tightens the directory it is given to
     * 0700, and the root belongs to the package manager's layout. */
    char *appcache = path_join(cache, "moonlight-tv");
    if (appcache != NULL) {
        path_dir_ensure(appcache);
    }
    return appcache;
}

char *path_runtime_cache(void) {
    return path_cache();
}

bool path_self_executable(char *dest, size_t maxlen) {
    /* The program a co-op primary execs as its secondary. On Android that is not the running image
     * (/proc/self/exe is the zygote's app_process) but the peer executable packaged beside libmain.so. */
    if (dest == NULL || maxlen == 0) {
        return false;
    }
    const char *dir = env_dir(ANDROID_ENV_NATIVE_DIR);
    if (dir == NULL) {
        return false;
    }
    const int written = snprintf(dest, maxlen, "%s/%s", dir, ANDROID_PEER_EXECUTABLE);
    if (written < 0 || (size_t) written >= maxlen) {
        return false;
    }
    return access(dest, X_OK) == 0;
}

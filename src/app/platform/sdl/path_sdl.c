#include "util/path.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

static char *path_cache_parent();

static char *path_owned_copy(const char *value) {
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

static const char *path_user_file(const char *basename) {
    /* The returned path is consumed immediately by fopen/stat. Per-thread
     * storage avoids both heap ownership at every diagnostic call and races
     * between the render, network and audio threads. */
    static _Thread_local char *file_path;
    bool persistent = false;
    char *pref = path_pref(&persistent);
    (void) persistent;
    char *next;
    if (pref != NULL) {
        next = path_join(pref, basename);
        free(pref);
    } else {
        next = path_join(".", basename);
    }
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
    char *sdl_path = SDL_GetBasePath();
    if (sdl_path == NULL) {
        return NULL;
    }
    const size_t length = strlen(sdl_path) + 1;
    char *path = malloc(length);
    if (path != NULL) {
        memcpy(path, sdl_path, length);
    }
    SDL_free(sdl_path);
    return path;
}

char *path_pref(bool *persistent) {
    char *sdl_path = SDL_GetPrefPath("com.limelight", "moonlight-tv");
    if (sdl_path == NULL) {
        *persistent = false;
        return NULL;
    }
    const size_t allocation = strlen(sdl_path) + 1;
    char *path = malloc(allocation);
    if (path == NULL) {
        SDL_free(sdl_path);
        *persistent = false;
        return NULL;
    }
    memcpy(path, sdl_path, allocation);
    SDL_free(sdl_path);
    unsigned int len = SDL_strlen(path);
    if (len && path[len - 1] == PATH_SEPARATOR) {
        path[len - 1] = '\0';
    }
    *persistent = true;
    return path;
}

char *path_cache() {
    char *cachedir = path_cache_parent();
    char *appcache = path_join(cachedir, "moonlight-tv");
    free(cachedir);
    path_dir_ensure(appcache);
    return appcache;
}

char *path_runtime_cache(void) {
#if defined(__linux__)
    const char *xdg = SDL_getenv("XDG_CACHE_HOME");
    const char *home = SDL_getenv("HOME");
    if (xdg != NULL) {
        if (xdg[0] != '/') return NULL;
    } else if (home == NULL || home[0] != '/') {
        return NULL;
    }
    return path_cache();
#else
    return NULL;
#endif
}

bool path_self_executable(char *dest, size_t maxlen) {
    if (dest == NULL || maxlen == 0) {
        return false;
    }
#if defined(_WIN32)
    /* GetModuleFileNameW is independent of the process working directory and preserves
     * profile/install paths that are not representable in the active ANSI code page. */
    wchar_t wide[32768];
    DWORD count = GetModuleFileNameW(NULL, wide, (DWORD) (sizeof(wide) / sizeof(wide[0])));
    if (count == 0 || count >= sizeof(wide) / sizeof(wide[0])) {
        return false;
    }
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, (int) count,
                                    dest, (int) maxlen - 1, NULL, NULL);
    if (bytes <= 0 || (size_t) bytes >= maxlen) {
        return false;
    }
    dest[bytes] = '\0';
    return true;
#elif defined(__linux__)
    ssize_t count = readlink("/proc/self/exe", dest, maxlen - 1);
    if (count < 0 || (size_t) count >= maxlen - 1) {
        return false;
    }
    dest[count] = '\0';
    return true;
#elif defined(__APPLE__)
    uint32_t cap = (uint32_t) maxlen;
    if (_NSGetExecutablePath(dest, &cap) != 0) {
        return false;
    }
    dest[maxlen - 1] = '\0';
    return true;
#else
    (void) dest;
    (void) maxlen;
    return false;
#endif
}

static char *path_cache_parent() {
#ifdef _WIN32
    /* SDL owns the platform-specific roaming/profile lookup. This avoids assuming
     * LOCALAPPDATA exists in the launching shell and keeps cache/config ownership in
     * the same per-user application directory. */
    char *appdir = SDL_GetPrefPath("com.limelight", "moonlight-tv");
    if (appdir == NULL) {
        return path_owned_copy(".");
    }
    char *cachedir = path_join(appdir, "cache");
    SDL_free(appdir);
    path_dir_ensure(cachedir);
    return cachedir;
#else
    char *cachedir = SDL_getenv("XDG_CACHE_HOME");
    if (cachedir) return path_owned_copy(cachedir);
    char *homedir = SDL_getenv("HOME");
    if (homedir) {
        cachedir = path_join(homedir, ".cache");
        path_dir_ensure(cachedir);
        return cachedir;
    }
    cachedir = path_join("/tmp", "moonlight-tv-cache");
    path_dir_ensure(cachedir);
    return cachedir;
#endif
}

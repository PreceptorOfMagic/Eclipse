#include "path.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <unistd.h>
#include <errno.h>

#include "util/compat.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wchar.h>
#endif

#if defined(_WIN32)
static void path_set_errno_from_win32(DWORD error) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION: errno = EACCES; break;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS: errno = EEXIST; break;
        case ERROR_FILENAME_EXCED_RANGE: errno = ENAMETOOLONG; break;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL: errno = ENOSPC; break;
        case ERROR_INVALID_NAME:
        case ERROR_INVALID_PARAMETER: errno = EINVAL; break;
        default: errno = EIO; break;
    }
}

static wchar_t *path_utf8_to_wide(const char *value) {
    if (value == NULL) {
        errno = EINVAL;
        return NULL;
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            value, -1, NULL, 0);
    if (length <= 0) {
        errno = EILSEQ;
        return NULL;
    }
    wchar_t *wide = calloc((size_t) length, sizeof(*wide));
    if (wide == NULL) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            value, -1, wide, length) != length) {
        free(wide);
        errno = EILSEQ;
        return NULL;
    }
    return wide;
}
#endif

char *path_join(const char *parent, const char *basename) {
    unsigned int parentlen = strlen(parent);
    if (parentlen && parent[parentlen - 1] == PATH_SEPARATOR) {
        parentlen -= 1;
    }
    char *joined = calloc(parentlen + 1 + strlen(basename) + 1, sizeof(char));
    sprintf(joined, "%.*s%c%s", parentlen, parent, PATH_SEPARATOR, basename);
    return joined;
}

void path_join_to(char *dest, size_t maxlen, const char *parent, const char *basename) {
    unsigned int parentlen = strlen(parent);
    if (parentlen && parent[parentlen - 1] == PATH_SEPARATOR) {
        parentlen -= 1;
    }
    snprintf(dest, maxlen, "%.*s%c%s", parentlen, parent, PATH_SEPARATOR, basename);
}

int path_dir_ensure(const char *dir) {
#if defined(_WIN32)
    if (dir == NULL || dir[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    wchar_t *wide = path_utf8_to_wide(dir);
    if (wide == NULL) {
        return -1;
    }

    /* Work entirely in UTF-16.  SDL's preference path is normally a drive path,
     * but redirected profiles can be UNC paths, whose \\server\share root must
     * never be passed to CreateDirectoryW one component at a time. */
    for (wchar_t *p = wide; *p != L'\0'; ++p) {
        if (*p == L'/') {
            *p = L'\\';
        }
    }
    wchar_t *scan = wide;
    if (wide[0] == L'\\' && wide[1] == L'\\') {
        scan = wcschr(wide + 2, L'\\');
        scan = scan != NULL ? wcschr(scan + 1, L'\\') : NULL;
        if (scan == NULL) {
            free(wide);
            return 0; /* A bare UNC share is already the root, not a directory to create. */
        }
        ++scan;
    } else if (wide[0] != L'\0' && wide[1] == L':') {
        scan = wide + 2;
        if (*scan == L'\\') {
            ++scan;
        }
    } else if (*scan == L'\\') {
        ++scan;
    }

    int result = 0;
    for (wchar_t *p = scan; ; ++p) {
        if (*p != L'\\' && *p != L'\0') {
            continue;
        }
        const wchar_t saved = *p;
        *p = L'\0';
        if (wide[0] != L'\0' && !CreateDirectoryW(wide, NULL)) {
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) {
                path_set_errno_from_win32(error);
                result = -1;
            }
        }
        *p = saved;
        if (result != 0 || saved == L'\0') {
            break;
        }
    }
    free(wide);
    return result;
#else
    if (access(dir, F_OK | W_OK) == 0) {
        return 0;
    }
    if (errno == ENOENT) {
        char tmp[PATH_MAX];
        char *last_slash = strrchr(dir, PATH_SEPARATOR);
        if (last_slash && last_slash != dir) {
            strncpy(tmp, dir, last_slash - dir);
            tmp[last_slash - dir] = '\0';
            if (path_dir_ensure(tmp) == -1) {
                return -1;
            }
        }
        return MKDIR(dir, 0755);
    }
    return -1;
#endif
}

FILE *path_fopen(const char *path, const char *mode) {
#if defined(_WIN32)
    wchar_t *wide_path = path_utf8_to_wide(path);
    wchar_t *wide_mode = path_utf8_to_wide(mode);
    FILE *file = wide_path != NULL && wide_mode != NULL
                 ? _wfopen(wide_path, wide_mode) : NULL;
    free(wide_path);
    free(wide_mode);
    return file;
#else
    return fopen(path, mode);
#endif
}

FILE *path_freopen(const char *path, const char *mode, FILE *stream) {
    if (stream == NULL) {
        errno = EINVAL;
        return NULL;
    }
#if defined(_WIN32)
    wchar_t *wide_path = path_utf8_to_wide(path);
    wchar_t *wide_mode = path_utf8_to_wide(mode);
    FILE *file = wide_path != NULL && wide_mode != NULL
                 ? _wfreopen(wide_path, wide_mode, stream) : NULL;
    free(wide_path);
    free(wide_mode);
    return file;
#else
    return freopen(path, mode, stream);
#endif
}

bool path_file_readable(const char *path) {
    FILE *file = path_fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    fclose(file);
    return true;
}

int path_file_copy(const char *source, const char *target) {
    FILE *input = path_fopen(source, "rb");
    if (input == NULL) {
        return -1;
    }
    FILE *output = path_fopen(target, "wb");
    if (output == NULL) {
        fclose(input);
        return -1;
    }

    int result = 0;
    unsigned char buffer[8192];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), input)) > 0) {
        if (fwrite(buffer, 1, count, output) != count) {
            result = -1;
            break;
        }
    }
    if (ferror(input) || fflush(output) != 0) {
        result = -1;
    }
    if (fclose(output) != 0) {
        result = -1;
    }
    if (fclose(input) != 0) {
        result = -1;
    }
    if (result != 0) {
        const int saved_errno = errno;
        path_file_remove(target);
        errno = saved_errno;
    }
    return result;
}

int path_file_remove(const char *path) {
#if defined(_WIN32)
    wchar_t *wide = path_utf8_to_wide(path);
    if (wide == NULL) {
        return -1;
    }
    const BOOL removed = DeleteFileW(wide);
    if (!removed) {
        path_set_errno_from_win32(GetLastError());
    }
    free(wide);
    return removed ? 0 : -1;
#else
    return remove(path);
#endif
}

bool path_file_replace(const char *source, const char *target) {
#if defined(_WIN32)
    wchar_t *wide_source = path_utf8_to_wide(source);
    wchar_t *wide_target = path_utf8_to_wide(target);
    const BOOL replaced = wide_source != NULL && wide_target != NULL &&
                          MoveFileExW(wide_source, wide_target,
                                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!replaced && wide_source != NULL && wide_target != NULL) {
        path_set_errno_from_win32(GetLastError());
    }
    free(wide_source);
    free(wide_target);
    return replaced != 0;
#else
    return rename(source, target) == 0;
#endif
}

char *path_self_executable_alloc(void) {
    /* Windows permits a 32K UTF-16 path whose UTF-8 form can be much larger
     * than PATH_MAX. Grow the caller buffer instead of imposing a second,
     * narrower path limit at the co-op process boundary. The same API carries
     * directly to Linux/macOS. */
    for (size_t capacity = 512; capacity <= 256u * 1024u; capacity *= 2u) {
        char *path = malloc(capacity);
        if (path == NULL) {
            return NULL;
        }
        if (path_self_executable(path, capacity)) {
            return path;
        }
        free(path);
    }
    return NULL;
}

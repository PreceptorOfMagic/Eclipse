#include "file_compat.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wchar.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#define GS_PATH_SEPARATOR '\\'

static void set_errno_from_win32(DWORD error) {
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

static wchar_t *utf8_to_wide(const char *value) {
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
#else
#define GS_PATH_SEPARATOR '/'
#endif

FILE *gs_fopen_utf8(const char *path, const char *mode) {
#if defined(_WIN32)
    wchar_t *wide_path = utf8_to_wide(path);
    wchar_t *wide_mode = utf8_to_wide(mode);
    FILE *file = wide_path != NULL && wide_mode != NULL
                 ? _wfopen(wide_path, wide_mode) : NULL;
    free(wide_path);
    free(wide_mode);
    return file;
#else
    return fopen(path, mode);
#endif
}

#if !defined(_WIN32)
/* Generated RSA PEM uses mkcert.c's 4096-byte buffer. Allow larger imported
 * credentials without allowing a misplaced file to trigger an unbounded copy. */
#define GS_PRIVATE_KEY_MAX_BYTES 16384

static int same_file(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size && a->st_mtime == b->st_mtime &&
           a->st_ctime == b->st_ctime && a->st_nlink == b->st_nlink;
}

/* Old TV profiles can belong to the developer account, with writable parents.
 * Preserve those identities by publishing an owned copy, never regenerating.
 * A hostile writer in a legacy world-writable parent remains outside this
 * boundary: POSIX rename cannot atomically compare the destination inode. */
static int migrate_private_key(const char *path, int original, const struct stat *before) {
    char *parent = strdup(path);
    if (parent == NULL) return -1;
    char *slash = strrchr(parent, '/');
    const char *name = slash ? strrchr(path, '/') + 1 : path;
    if (slash == parent) slash[1] = '\0';
    else if (slash) *slash = '\0';
    else { free(parent); parent = strdup("."); }
    if (parent == NULL) return -1;
    int directory = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    free(parent);
    if (directory < 0) return -1;

    int replacement = -1, result = -1;
    char temporary[80] = "";
    struct stat current, created;
    unsigned char bytes[GS_PRIVATE_KEY_MAX_BYTES], verify[GS_PRIVATE_KEY_MAX_BYTES];
    const size_t size = (size_t) before->st_size;
    size_t copied = 0;
    while (copied < size) {
        ssize_t count = read(original, bytes + copied, size - copied);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (count == 0) errno = EIO; goto done; }
        copied += (size_t) count;
    }
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        snprintf(temporary, sizeof(temporary), ".eclipse-key-%ld-%u.tmp", (long)getpid(), attempt);
        replacement = openat(directory, temporary, O_RDWR | O_CREAT | O_EXCL |
                             O_NOFOLLOW | O_CLOEXEC, 0600);
        if (replacement >= 0 || errno != EEXIST) break;
    }
    if (replacement < 0) { temporary[0] = '\0'; goto done; }
    if (fstat(replacement, &created) != 0 || fchmod(replacement, 0600) != 0) goto done;
    copied = 0;
    while (copied < size) {
        ssize_t count = write(replacement, bytes + copied, size - copied);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (count == 0) errno = EIO; goto done; }
        copied += (size_t) count;
    }
    if (fsync(replacement) != 0 || fstat(replacement, &current) != 0) goto done;
    if (!S_ISREG(current.st_mode) || current.st_uid != geteuid() ||
        current.st_nlink != 1 || (current.st_mode & 07777) != 0600 ||
        current.st_size != before->st_size) { errno = EACCES; goto done; }
    if (lseek(original, 0, SEEK_SET) == -1) goto done;
    copied = 0;
    while (copied < size) {
        ssize_t count = read(original, verify + copied, size - copied);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (count == 0) errno = EIO; goto done; }
        copied += (size_t) count;
    }
    if (memcmp(bytes, verify, size) != 0) { errno = EAGAIN; goto done; }
    if (fstat(original, &current) != 0 || !same_file(before, &current) ||
        fstatat(directory, name, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
        !same_file(before, &current) || !S_ISREG(current.st_mode)) {
        errno = EAGAIN; goto done;
    }
    if (fstatat(directory, temporary, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
        current.st_dev != created.st_dev || current.st_ino != created.st_ino ||
        !S_ISREG(current.st_mode) || current.st_uid != geteuid() ||
        current.st_nlink != 1 || (current.st_mode & 07777) != 0600) {
        errno = EAGAIN; goto done;
    }
    if (lseek(replacement, 0, SEEK_SET) == -1 ||
        renameat(directory, temporary, directory, name) != 0) goto done;
    temporary[0] = '\0';
    if (fsync(directory) != 0) goto done;
    result = replacement;
    replacement = -1;
done:;
    const int error = errno;
    if (replacement >= 0) {
        if (temporary[0] && fstat(replacement, &created) == 0 &&
            fstatat(directory, temporary, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
            current.st_dev == created.st_dev && current.st_ino == created.st_ino)
            unlinkat(directory, temporary, 0);
        close(replacement);
    }
    close(directory);
    errno = error;
    return result;
}
#endif

FILE *gs_fopen_private_utf8(const char *path, int writing) {
#if defined(_WIN32)
    return gs_fopen_utf8(path, writing ? "wb" : "rb");
#else
    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }
    /* O_NONBLOCK prevents an unsafe FIFO from hanging before fstat rejects it.
     * Never truncate or chmod a foreign/shared inode while checking identity. */
    int fd = open(path, (writing ? O_WRONLY | O_CREAT : O_RDONLY) |
                       O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd == -1) {
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        goto fail;
    }
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || (writing && st.st_uid != geteuid())) {
        errno = EACCES;
        goto fail;
    }
    if (!writing && (st.st_size < 0 || st.st_size > GS_PRIVATE_KEY_MAX_BYTES)) {
        errno = EFBIG;
        goto fail;
    }
    if (!writing && st.st_uid != geteuid()) {
        int migrated = migrate_private_key(path, fd, &st);
        if (migrated < 0) goto fail;
        close(fd);
        fd = migrated;
    }
    if (fchmod(fd, 0600) != 0 || (writing && ftruncate(fd, 0) != 0)) {
        goto fail;
    }
    FILE *file = fdopen(fd, writing ? "wb" : "rb");
    if (file != NULL) {
        return file;
    }
fail:;
    const int error = errno;
    close(fd);
    errno = error;
    return NULL;
#endif
}

char *gs_path_join(const char *parent, const char *basename) {
    if (parent == NULL || basename == NULL) {
        errno = EINVAL;
        return NULL;
    }
    size_t parent_length = strlen(parent);
    while (parent_length > 0 &&
           (parent[parent_length - 1] == '/' || parent[parent_length - 1] == '\\')) {
        --parent_length;
    }
    const size_t basename_length = strlen(basename);
    if (parent_length > SIZE_MAX - basename_length - 2) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    char *joined = malloc(parent_length + basename_length + 2);
    if (joined == NULL) {
        return NULL;
    }
    memcpy(joined, parent, parent_length);
    joined[parent_length] = GS_PATH_SEPARATOR;
    memcpy(joined + parent_length + 1, basename, basename_length + 1);
    return joined;
}

int gs_mkdirs_utf8(const char *directory) {
    if (directory == NULL || directory[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
#if defined(_WIN32)
    wchar_t *wide = utf8_to_wide(directory);
    if (wide == NULL) {
        return -1;
    }
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
            return 0;
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
                set_errno_from_win32(error);
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
    char *copy = strdup(directory);
    if (copy == NULL) {
        return -1;
    }
    int result = 0;
    for (char *p = copy + (copy[0] == '/' ? 1 : 0); ; ++p) {
        if (*p != '/' && *p != '\0') {
            continue;
        }
        const char saved = *p;
        *p = '\0';
        if (copy[0] != '\0' && mkdir(copy, 0775) != 0 && errno != EEXIST) {
            result = -1;
        }
        *p = saved;
        if (result != 0 || saved == '\0') {
            break;
        }
    }
    free(copy);
    return result;
#endif
}

static int read_file_utf8(const char *path, unsigned char **data_out, size_t *size_out,
                          int private_file) {
    if (path == NULL || data_out == NULL || size_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *data_out = NULL;
    *size_out = 0;
    FILE *file = private_file ? gs_fopen_private_utf8(path, 0) : gs_fopen_utf8(path, "rb");
    if (file == NULL) {
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    const long end = ftell(file);
    if (end < 0 || (uintmax_t) end > SIZE_MAX - 1 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        errno = end < 0 ? EIO : EFBIG;
        return -1;
    }
    const size_t size = (size_t) end;
    unsigned char *data = malloc(size + 1);
    if (data == NULL) {
        fclose(file);
        return -1;
    }
    if (size > 0 && fread(data, 1, size, file) != size) {
        free(data);
        fclose(file);
        errno = EIO;
        return -1;
    }
    data[size] = '\0';
    if (fclose(file) != 0) {
        free(data);
        return -1;
    }
    *data_out = data;
    *size_out = size;
    return 0;
}

int gs_read_file_utf8(const char *path, unsigned char **data_out, size_t *size_out) {
    return read_file_utf8(path, data_out, size_out, 0);
}

int gs_read_private_file_utf8(const char *path, unsigned char **data_out, size_t *size_out) {
    return read_file_utf8(path, data_out, size_out, 1);
}

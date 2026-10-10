#include "runtime_files.h"
#undef fopen
#undef access
#undef unlink
#undef stat

#ifdef ECLIPSE_RUNTIME_ACTIVE
#include "util/path.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static pthread_once_t runtime_once = PTHREAD_ONCE_INIT;
static int runtime_fd = -1;
static int runtime_error = EACCES;
static _Thread_local bool runtime_initializing;

static bool private_directory(int fd, bool strict) {
    struct stat st;
    if (fstat(fd, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & (strict ? 0077 : 0022)) != 0) {
        errno = EACCES;
        return false;
    }
    return true;
}

static void runtime_init(void) {
    /* Platform cache creation uses the normal path helpers, whose libc calls
     * also see this header. Resolve those real paths without recursive once(). */
    runtime_initializing = true;
    char *cache = path_runtime_cache();
    runtime_initializing = false;
    int parent = -1;
    if (cache == NULL) { runtime_error = ENOMEM; return; }
    /* Both platform implementations have an emergency shared temporary fallback.
     * Those locations are outside app uninstall ownership and are not acceptable. */
    if (cache[0] != '/') {
        free(cache);
        return;
    }
    parent = open(cache, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    free(cache);
    if (parent < 0) { runtime_error = errno; return; }
    struct stat parent_st;
    if (fstat(parent, &parent_st) != 0) goto failed;
    if (!S_ISDIR(parent_st.st_mode) || parent_st.st_uid != geteuid()) {
        errno = EACCES;
        goto failed;
    }
    /* Only this verified application cache leaf, never its XDG/HOME parent.
     * Older owned caches may be group writable; tighten the held inode first. */
    if (fchmod(parent, 0700) != 0 || !private_directory(parent, true)) goto failed;
    if (mkdirat(parent, "runtime", 0700) != 0 && errno != EEXIST) goto failed;
    runtime_fd = openat(parent, "runtime", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (runtime_fd < 0) goto failed;
    /* A webOS 26 boot reopens the whole app tree to 0775 (path_webos.c). The leaf is ours, so
     * close it again like the parent above; a leaf owned by anyone else still fails the check. */
    if (!private_directory(runtime_fd, true) &&
        (fchmod(runtime_fd, 0700) != 0 || !private_directory(runtime_fd, true))) {
        int saved = errno;
        close(runtime_fd);
        runtime_fd = -1;
        errno = saved;
        goto failed;
    }
    close(parent);
    /* No recursive exit cleanup: another primary/secondary may still use this
     * directory. Existing stop paths unlink their sockets; uninstall owns cache. */
    return;
failed:
    runtime_error = errno;
    close(parent);
}

/* Only flat app-owned legacy names are virtual. Nested user/config paths keep
 * their native meaning even if a parent directory happens to share the prefix. */
static int runtime_name(const char *path, const char **name) {
    if (path == NULL) { errno = EINVAL; return -1; }
    if (runtime_initializing) return 0;
    if (strncmp(path, "/tmp/coop-", 10) != 0 &&
        strncmp(path, "/tmp/coop_", 10) != 0 &&
        strncmp(path, "/tmp/aurora-", 12) != 0) return 0;
    *name = path + 5;
    if (strchr(*name, '/') != NULL) return 0;
    pthread_once(&runtime_once, runtime_init);
    if (runtime_fd < 0) { errno = runtime_error; return -1; }
    return 1;
}

static int runtime_entry(const char *name, struct stat *st) {
    if (fstatat(runtime_fd, name, st, AT_SYMLINK_NOFOLLOW) != 0) return -1;
    if (st->st_uid != geteuid() || S_ISLNK(st->st_mode) ||
        !(S_ISREG(st->st_mode) || S_ISSOCK(st->st_mode)) || st->st_nlink != 1) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

FILE *eclipse_runtime_fopen(const char *path, const char *mode) {
    const char *name;
    int mapped = runtime_name(path, &name);
    if (mapped == 0) return fopen(path, mode);
    if (mapped < 0) return NULL;
    if (mode == NULL || !strchr("rwa", mode[0]) || mode[0] == '\0') {
        errno = EINVAL; return NULL;
    }
    for (const char *m = mode + 1; *m; ++m) {
        if (!strchr("+bex", *m)) { errno = EINVAL; return NULL; }
    }
    int flags = strchr(mode, '+') ? O_RDWR : (mode[0] == 'r' ? O_RDONLY : O_WRONLY);
    if (mode[0] != 'r') flags |= O_CREAT;
    if (mode[0] == 'a') flags |= O_APPEND;
    if (strchr(mode, 'x')) flags |= O_EXCL;
    /* Validate before truncation and refuse symlinks/hardlinks, including leftovers
     * from older versions. Cache is private, but never trust an existing entry. */
    int fd = openat(runtime_fd, name, flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1) {
        close(fd); errno = EACCES; return NULL;
    }
    if ((mode[0] != 'r' && fchmod(fd, 0600) != 0) ||
        (mode[0] == 'w' && ftruncate(fd, 0) != 0)) {
        int saved = errno; close(fd); errno = saved; return NULL;
    }
    FILE *file = fdopen(fd, mode);
    if (file == NULL) { int saved = errno; close(fd); errno = saved; }
    return file;
}

int eclipse_runtime_stat(const char *path, struct stat *out) {
    const char *name;
    int mapped = runtime_name(path, &name);
    if (mapped == 0) return stat(path, out);
    return mapped < 0 ? -1 : runtime_entry(name, out);
}

int eclipse_runtime_access(const char *path, int mode) {
    const char *name;
    int mapped = runtime_name(path, &name);
    if (mapped == 0) return access(path, mode);
    if (mapped < 0) return -1;
    struct stat st;
    if (runtime_entry(name, &st) != 0) return -1;
    return faccessat(runtime_fd, name, mode, 0);
}

int eclipse_runtime_unlink(const char *path) {
    const char *name;
    int mapped = runtime_name(path, &name);
    if (mapped == 0) return unlink(path);
    if (mapped < 0) return -1;
    struct stat st;
    if (runtime_entry(name, &st) != 0) return -1;
    return unlinkat(runtime_fd, name, 0);
}

int eclipse_runtime_socket_path(const char *path, char *out, size_t size) {
    const char *name;
    int mapped = runtime_name(path, &name);
    if (mapped < 0) return -1;
    int written = mapped ? snprintf(out, size, "/proc/self/fd/%d/%s", runtime_fd, name)
                         : snprintf(out, size, "%s", path);
    if (written < 0 || (size_t) written >= size) { errno = ENAMETOOLONG; return -1; }
    return 0;
}
#endif

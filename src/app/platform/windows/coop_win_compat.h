/**
 * POSIX surface the co-op engine uses, implemented on Win32.
 *
 * WHY A SHIM AND NOT AN #ifdef IN EVERY FILE: these sources are shared verbatim with the
 * webOS client (src/app/stream/video/coop_*.c). Every `#ifdef _WIN32` added there is a
 * place the two copies can drift, and the co-op engine's comments carry measured history
 * that must stay attached to the code it describes. So the .c files are copied UNCHANGED
 * and this header is force-included ahead of them (cl /FI). If you find yourself editing
 * a coop_*.c for Windows, add to this file instead -- and if that is impossible, say so
 * in the file's header so the next reader knows the two copies diverged and why.
 *
 * The wrappers are defined BEFORE the macros that rename them, so each wrapper body still
 * reaches the real Winsock function. Do not reorder.
 */
#ifndef COOP_WIN_COMPAT_H
#define COOP_WIN_COMPAT_H

#ifdef _WIN32

/* C4996 ("consider strerror_s / _dupenv_s / fopen_s ...") fires on the shared engine
 * sources' ordinary C library calls: strerror, fopen, fscanf, getenv. Those files are
 * copied byte-identical from the webOS client and MUST NOT be edited here, so the
 * warning is disabled for the translation units this header is force-included into --
 * i.e. exactly the co-op C sources, and NOT the rest of the application, which keeps
 * the warning. Defining _CRT_SECURE_NO_WARNINGS project-wide would have silenced it
 * everywhere instead. */
#if defined(_MSC_VER)
#pragma warning(disable : 4996)
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>   /* inet_ntop/inet_pton live here, not in winsock2.h */
#include <afunix.h>      /* AF_UNIX on Windows 10 1803+; filesystem paths only, no
                          * abstract namespace -- the engine already uses a real path. */
#include <windows.h>
#include <io.h>
#include <process.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>        /* struct timespec (C11) -- only clock_gettime() is missing */
#include <wchar.h>

/* --------------------------------------------------------------- runtime paths
 * Shared co-op sources intentionally keep the TV's /tmp names. Windows has no /tmp,
 * and treating those strings as C:\tmp made every production sentinel and AF_UNIX
 * endpoint depend on a directory a fresh user does not have. Translate that virtual
 * prefix in ONE platform seam to a per-user directory we create on demand.
 *
 * No environment override is accepted here: production behavior must not depend on an
 * undocumented variable inherited from a developer shell. */
#define COOP_WIN_RUNTIME_SUFFIX L"Aurora\\runtime"

static __inline bool coop_win_utf8_to_wide(const char *src, wchar_t *dst, size_t cap) {
    if (src == NULL || dst == NULL || cap == 0 || cap > INT_MAX) {
        return false;
    }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, dst, (int) cap);
    return n > 0 && (size_t) n <= cap;
}

static __inline bool coop_win_wide_to_utf8(const wchar_t *src, char *dst, size_t cap) {
    if (src == NULL || dst == NULL || cap == 0 || cap > INT_MAX) {
        return false;
    }
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1,
                                dst, (int) cap, NULL, NULL);
    return n > 0 && (size_t) n <= cap;
}

static __inline bool coop_win_make_dirs_w(wchar_t *path) {
    if (path == NULL || path[0] == L'\0') {
        return false;
    }
    /* Preserve a drive root (C:\) or UNC prefix while walking components. */
    wchar_t *p = path;
    if (p[0] != L'\0' && p[1] == L':') {
        p += 3;
    } else if (p[0] == L'\\' && p[1] == L'\\') {
        /* Skip both \\server and \share. Creating either component is invalid
         * and used to make redirected (UNC) profiles fail before any socket call. */
        p = wcschr(p + 2, L'\\');
        p = p != NULL ? wcschr(p + 1, L'\\') : NULL;
        if (p == NULL) {
            return true;
        }
        ++p;
    }
    for (; *p != L'\0'; ++p) {
        if (*p != L'\\' && *p != L'/') {
            continue;
        }
        wchar_t saved = *p;
        *p = L'\0';
        if (path[0] != L'\0' && !CreateDirectoryW(path, NULL) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            *p = saved;
            return false;
        }
        *p = saved;
    }
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static __inline bool coop_win_runtime_root_w(wchar_t *dst, size_t cap) {
    if (dst == NULL || cap < 4 || cap > 0xFFFFFFFFu) {
        return false;
    }
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dst, (DWORD) cap);
    if (n == 0 || n >= cap) {
        n = GetTempPathW((DWORD) cap, dst);
        if (n == 0 || n >= cap) {
            return false;
        }
    }
    size_t used = wcslen(dst);
    while (used > 0 && (dst[used - 1] == L'\\' || dst[used - 1] == L'/')) {
        dst[--used] = L'\0';
    }
    const wchar_t suffix[] = L"\\" COOP_WIN_RUNTIME_SUFFIX;
    if (used + (sizeof(suffix) / sizeof(suffix[0])) > cap) {
        return false;
    }
    wcscat(dst, suffix);
    return coop_win_make_dirs_w(dst);
}

static __inline bool coop_win_resolve_path_w(const char *path, wchar_t *dst, size_t cap) {
    static const char prefix[] = "/tmp/";
    if (path == NULL || dst == NULL || cap == 0) {
        return false;
    }
    if (strncmp(path, prefix, sizeof(prefix) - 1) != 0) {
        return coop_win_utf8_to_wide(path, dst, cap);
    }
    if (!coop_win_runtime_root_w(dst, cap)) {
        return false;
    }
    size_t used = wcslen(dst);
    if (used + 2 >= cap) {
        return false;
    }
    dst[used++] = L'\\';
    dst[used] = L'\0';
    wchar_t name[512];
    if (!coop_win_utf8_to_wide(path + sizeof(prefix) - 1, name,
                               sizeof(name) / sizeof(name[0])) ||
        used + wcslen(name) + 1 > cap) {
        return false;
    }
    wcscat(dst, name);
    return true;
}

static __inline bool coop_win_resolve_path(const char *path, char *dst, size_t cap) {
    wchar_t wide[32768];
    return coop_win_resolve_path_w(path, wide, sizeof(wide) / sizeof(wide[0])) &&
           coop_win_wide_to_utf8(wide, dst, cap);
}

/* sockaddr_un::sun_path is only 108 bytes on Windows.  The ordinary resolved
 * path can exceed that solely because the user's profile name is long or not
 * ASCII. Ask NTFS for the existing runtime directory's short alias, then append
 * the small, build-owned socket basename. Sentinel/log files keep using the
 * full UTF-16 path above. */
static __inline bool coop_win_resolve_socket_path(const char *path, char *dst, size_t cap) {
    static const char prefix[] = "/tmp/";
    if (path == NULL || dst == NULL || cap == 0) {
        return false;
    }
    if (strncmp(path, prefix, sizeof(prefix) - 1) != 0) {
        return coop_win_resolve_path(path, dst, cap);
    }

    wchar_t root[32768];
    if (!coop_win_runtime_root_w(root, sizeof(root) / sizeof(root[0]))) {
        return false;
    }
    wchar_t short_root[32768];
    DWORD short_length = GetShortPathNameW(root, short_root,
                                           (DWORD) (sizeof(short_root) / sizeof(short_root[0])));
    const wchar_t *chosen_root = short_length > 0 && short_length < sizeof(short_root) / sizeof(short_root[0])
                                 ? short_root : root;
    wchar_t basename[512];
    if (!coop_win_utf8_to_wide(path + sizeof(prefix) - 1, basename,
                               sizeof(basename) / sizeof(basename[0]))) {
        return false;
    }
    const size_t root_length = wcslen(chosen_root);
    const size_t basename_length = wcslen(basename);
    if (root_length + basename_length + 2 > sizeof(short_root) / sizeof(short_root[0])) {
        return false;
    }
    wchar_t socket_path[32768];
    memcpy(socket_path, chosen_root, (root_length + 1) * sizeof(*socket_path));
    socket_path[root_length] = L'\\';
    memcpy(socket_path + root_length + 1, basename,
           (basename_length + 1) * sizeof(*socket_path));
    return coop_win_wide_to_utf8(socket_path, dst, cap);
}

static __inline FILE *coop_win_fopen(const char *path, const char *mode) {
    wchar_t wide_path[32768], wide_mode[32];
    if (!coop_win_resolve_path_w(path, wide_path, sizeof(wide_path) / sizeof(wide_path[0])) ||
        !coop_win_utf8_to_wide(mode, wide_mode, sizeof(wide_mode) / sizeof(wide_mode[0]))) {
        errno = EINVAL;
        return NULL;
    }
    return _wfopen(wide_path, wide_mode);
}

static __inline int coop_win_access(const char *path, int mode) {
    wchar_t wide[32768];
    if (!coop_win_resolve_path_w(path, wide, sizeof(wide) / sizeof(wide[0]))) {
        errno = EINVAL;
        return -1;
    }
    return _waccess(wide, mode);
}

static __inline int coop_win_unlink(const char *path) {
    wchar_t wide[32768];
    if (!coop_win_resolve_path_w(path, wide, sizeof(wide) / sizeof(wide[0]))) {
        errno = EINVAL;
        return -1;
    }
    return _wunlink(wide);
}

static __inline int coop_win_stat(const char *path, struct stat *out) {
    wchar_t wide[32768];
    struct _stat64 value;
    if (out == NULL ||
        !coop_win_resolve_path_w(path, wide, sizeof(wide) / sizeof(wide[0]))) {
        errno = EINVAL;
        return -1;
    }
    if (_wstat64(wide, &value) != 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->st_size = value.st_size;
    out->st_mtime = value.st_mtime;
    out->st_mode = value.st_mode;
    return 0;
}

/* ---------------------------------------------------------------- errno mapping
 * Winsock does not touch the CRT's errno, but the engine branches on EAGAIN/EINTR after
 * every socket call. Each wrapper below therefore SETS errno from WSAGetLastError(), so
 * the unmodified POSIX error handling keeps working. */
static __inline int coop_win_errno_from_wsa(int wsa) {
    switch (wsa) {
        case WSAEWOULDBLOCK: return EAGAIN;
        case WSAEINTR:       return EINTR;
        case WSAECONNRESET:  return ECONNRESET;
        case WSAECONNABORTED:return ECONNABORTED;
        case WSAENOTCONN:    return ENOTCONN;
        case WSAESHUTDOWN:   return EPIPE;
        case WSAEINVAL:      return EINVAL;
        case WSAEACCES:      return EACCES;
        case WSAEADDRINUSE:  return EADDRINUSE;
        default:             return EIO;
    }
}
static __inline void coop_win_set_errno(void) {
    errno = coop_win_errno_from_wsa(WSAGetLastError());
}

/* One-time Winsock startup. Every entry point that can be reached first calls this, so
 * callers need no new init step -- another thing that would have to be added to the
 * shared sources. */
static __inline bool coop_win_sock_init(void) {
    /* 0 = uninitialised, 1 = one thread is inside WSAStartup, 2 = ready,
     * -1 = startup failed. Publishing READY only after WSAStartup returns is
     * essential: another app worker can create the input/audio link at the
     * same time as the video coordinator. */
    static volatile LONG state = 0;
    LONG observed = InterlockedCompareExchange(&state, 1, 0);
    if (observed == 0) {
        WSADATA wsa;
        const int result = WSAStartup(MAKEWORD(2, 2), &wsa);
        InterlockedExchange(&state, result == 0 ? 2 : -1);
        if (result != 0) {
            errno = coop_win_errno_from_wsa(result);
            return false;
        }
        return true;
    }
    while (observed == 1) {
        Sleep(0);
        observed = InterlockedCompareExchange(&state, 0, 0);
    }
    if (observed != 2) {
        errno = EIO;
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------- socket ops
 * SOCKET is UINT_PTR, so every wrapper preserves it in intptr_t. Storing one in
 * int is not an ABI contract and can turn a valid 64-bit handle into another
 * socket or INVALID_SOCKET. */
static __inline intptr_t coop_win_socket(int domain, int type, int protocol) {
    if (!coop_win_sock_init()) { return -1; }
    SOCKET s = socket(domain, type, protocol);
    if (s == INVALID_SOCKET) { coop_win_set_errno(); return -1; }
    return (intptr_t) s;
}
static __inline int coop_win_bind(intptr_t fd, const struct sockaddr *a, int len) {
    struct sockaddr_un mapped;
    if (a != NULL && a->sa_family == AF_UNIX) {
        const struct sockaddr_un *source = (const struct sockaddr_un *) a;
        char path[sizeof(mapped.sun_path)];
        if (!coop_win_resolve_socket_path(source->sun_path, path, sizeof(path))) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memset(&mapped, 0, sizeof(mapped));
        mapped.sun_family = AF_UNIX;
        memcpy(mapped.sun_path, path, strlen(path) + 1);
        a = (const struct sockaddr *) &mapped;
        len = sizeof(mapped);
    }
    if (bind((SOCKET) fd, a, len) == SOCKET_ERROR) { coop_win_set_errno(); return -1; }
    return 0;
}
static __inline int coop_win_listen(intptr_t fd, int backlog) {
    if (listen((SOCKET) fd, backlog) == SOCKET_ERROR) { coop_win_set_errno(); return -1; }
    return 0;
}
static __inline intptr_t coop_win_accept(intptr_t fd, struct sockaddr *a, int *len) {
    if (!coop_win_sock_init()) { return -1; }
    SOCKET s = accept((SOCKET) fd, a, len);
    if (s == INVALID_SOCKET) { coop_win_set_errno(); return -1; }
    return (intptr_t) s;
}
static __inline int coop_win_connect(intptr_t fd, const struct sockaddr *a, int len) {
    struct sockaddr_un mapped;
    if (a != NULL && a->sa_family == AF_UNIX) {
        const struct sockaddr_un *source = (const struct sockaddr_un *) a;
        char path[sizeof(mapped.sun_path)];
        if (!coop_win_resolve_socket_path(source->sun_path, path, sizeof(path))) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memset(&mapped, 0, sizeof(mapped));
        mapped.sun_family = AF_UNIX;
        memcpy(mapped.sun_path, path, strlen(path) + 1);
        a = (const struct sockaddr *) &mapped;
        len = sizeof(mapped);
    }
    if (connect((SOCKET) fd, a, len) == SOCKET_ERROR) { coop_win_set_errno(); return -1; }
    return 0;
}
static __inline int coop_win_close(intptr_t fd) {
    if (closesocket((SOCKET) fd) == SOCKET_ERROR) { coop_win_set_errno(); return -1; }
    return 0;
}
/* read()/write() do not work on Winsock handles; recv()/send() are the equivalents. */
static __inline int coop_win_read(intptr_t fd, void *buf, size_t len) {
    int n = recv((SOCKET) fd, (char *) buf, (int) len, 0);
    if (n == SOCKET_ERROR) { coop_win_set_errno(); return -1; }
    return n;
}
static __inline int coop_win_write(intptr_t fd, const void *buf, size_t len) {
    int n = send((SOCKET) fd, (const char *) buf, (int) len, 0);
    if (n == SOCKET_ERROR) { coop_win_set_errno(); return -1; }
    return n;
}
/* The shared TV transport calls send()/recv() directly so it can supply
 * MSG_DONTWAIT/MSG_NOSIGNAL.  Winsock reports failures only through
 * WSAGetLastError(); mirror the POSIX errno contract here just as read()/write()
 * do above.  A plain cast-only macro makes an ordinary WSAEWOULDBLOCK look like
 * a fatal disconnect whenever CRT errno happens to contain another value. */
static __inline int coop_win_send(intptr_t fd, const void *buf, size_t len, int flags) {
    int n = send((SOCKET) fd, (const char *) buf, (int) len, flags);
    if (n == SOCKET_ERROR) { coop_win_set_errno(); return -1; }
    return n;
}
static __inline int coop_win_recv(intptr_t fd, void *buf, size_t len, int flags) {
    int n = recv((SOCKET) fd, (char *) buf, (int) len, flags);
    if (n == SOCKET_ERROR) { coop_win_set_errno(); return -1; }
    return n;
}
/* Winsock spells the shutdown() directions SD_*, with the same meanings. */
#ifndef SHUT_RD
#define SHUT_RD   SD_RECEIVE
#define SHUT_WR   SD_SEND
#define SHUT_RDWR SD_BOTH
#endif
static __inline int coop_win_shutdown(intptr_t fd, int how) {
    if (shutdown((SOCKET) fd, how) == SOCKET_ERROR) { coop_win_set_errno(); return -1; }
    return 0;
}
static __inline int coop_win_poll(struct pollfd *fds, unsigned long n, int timeout_ms) {
    int r = WSAPoll(fds, (ULONG) n, timeout_ms);
    if (r == SOCKET_ERROR) { coop_win_set_errno(); return -1; }
    return r;
}

/* fcntl(F_SETFL, O_NONBLOCK) is the only fcntl the engine uses; ioctlsocket is the
 * Winsock equivalent. The F_GETFL call has no counterpart and no effect here, so it
 * returns a harmless 0 that satisfies the engine's `flags >= 0` guard. */
#ifndef F_GETFL
#define F_GETFL 3
#define F_SETFL 4
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x0004
#endif
static __inline int coop_win_fcntl(intptr_t fd, int cmd, int arg) {
    if (cmd == F_GETFL) { return 0; }
    if (cmd == F_SETFL) {
        u_long nb = (arg & O_NONBLOCK) ? 1u : 0u;
        if (ioctlsocket((SOCKET) fd, FIONBIO, &nb) == SOCKET_ERROR) {
            coop_win_set_errno();
            return -1;
        }
        return 0;
    }
    errno = EINVAL;
    return -1;
}

/* --------------------------------------------------------------------- time/misc */
/* access() modes. Windows' _access has no execute bit, so X_OK degrades to an
 * existence test -- the engine only ever uses F_OK (does this sentinel exist). */
#ifndef F_OK
#define F_OK 0
#define X_OK 0
#define W_OK 2
#define R_OK 4
#endif

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
/* MSVC's <time.h> declares struct timespec (C11), so only the accessor is missing. */
static __inline int coop_win_clock_gettime(int clk, struct timespec *ts) {
    (void) clk;
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) { QueryPerformanceFrequency(&freq); }
    QueryPerformanceCounter(&now);
    ts->tv_sec = (time_t) (now.QuadPart / freq.QuadPart);
    ts->tv_nsec = (long) (((now.QuadPart % freq.QuadPart) * 1000000000ll) / freq.QuadPart);
    return 0;
}
static __inline int coop_win_usleep(unsigned int us) {
    Sleep(us / 1000u);          /* sub-millisecond sleeps are not expressible here; the
                                 * engine only uses this for backoff, never for pacing. */
    return 0;
}
static __inline int coop_win_setenv(const char *k, const char *v, int overwrite) {
    if (!overwrite && getenv(k) != NULL) { return 0; }
    return _putenv_s(k, v) == 0 ? 0 : -1;
}
static __inline int coop_win_unsetenv(const char *k) {
    return _putenv_s(k, "") == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------------- threads
 * SRWLOCK, not CRITICAL_SECTION: it has a static initialiser (SRWLOCK_INIT), which is
 * what PTHREAD_MUTEX_INITIALIZER needs at file scope, and it is non-recursive -- matching
 * the default pthread mutex the engine was written against. A CRITICAL_SECTION would be
 * recursive, which would silently permit a double-lock the POSIX build would deadlock on,
 * so the two platforms would not fail the same way. */
typedef SRWLOCK pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
static __inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr) {
    (void) attr; InitializeSRWLock(m); return 0;
}
static __inline int pthread_mutex_destroy(pthread_mutex_t *m) { (void) m; return 0; }
static __inline int pthread_mutex_lock(pthread_mutex_t *m) {
    AcquireSRWLockExclusive(m); return 0;
}
static __inline int pthread_mutex_unlock(pthread_mutex_t *m) {
    ReleaseSRWLockExclusive(m); return 0;
}

typedef HANDLE pthread_t;
typedef struct { void *(*fn)(void *); void *arg; } coop_win_thread_start_t;
static DWORD WINAPI coop_win_thread_trampoline(LPVOID p) {
    coop_win_thread_start_t *s = (coop_win_thread_start_t *) p;
    void *(*fn)(void *) = s->fn;
    void *arg = s->arg;
    free(s);                    /* freed before the body runs: the body may never return */
    fn(arg);
    return 0;
}
static __inline int pthread_create(pthread_t *t, const void *attr,
                                   void *(*fn)(void *), void *arg) {
    (void) attr;
    coop_win_thread_start_t *s = (coop_win_thread_start_t *) malloc(sizeof(*s));
    if (s == NULL) { return EAGAIN; }
    s->fn = fn;
    s->arg = arg;
    HANDLE h = CreateThread(NULL, 0, coop_win_thread_trampoline, s, 0, NULL);
    if (h == NULL) { free(s); return EAGAIN; }
    *t = h;
    return 0;
}
static __inline int pthread_join(pthread_t t, void **retval) {
    if (retval != NULL) { *retval = NULL; }   /* the engine never reads a thread's return */
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}
static __inline int pthread_detach(pthread_t t) { CloseHandle(t); return 0; }

#define socket(a, b, c)         coop_win_socket((a), (b), (c))
#define bind(a, b, c)           coop_win_bind((a), (const struct sockaddr *) (b), (int) (c))
#define listen(a, b)            coop_win_listen((a), (b))
#define accept(a, b, c)         coop_win_accept((a), (struct sockaddr *) (b), (int *) (c))
#define connect(a, b, c)        coop_win_connect((a), (const struct sockaddr *) (b), (int) (c))
#define close(a)                coop_win_close(a)
#define read(a, b, c)           coop_win_read((a), (b), (c))
#define write(a, b, c)          coop_win_write((a), (b), (c))
#define poll(a, b, c)           coop_win_poll((a), (unsigned long) (b), (c))
#define shutdown(a, b)          coop_win_shutdown((a), (b))
#define fcntl(a, b, c)          coop_win_fcntl((a), (b), (int) (c))
#define clock_gettime(a, b)     coop_win_clock_gettime((a), (b))
#define usleep(a)               coop_win_usleep((unsigned int) (a))
#define setenv(a, b, c)         coop_win_setenv((a), (b), (c))
#define unsetenv(a)             coop_win_unsetenv(a)
#ifdef fopen
#undef fopen
#endif
#define fopen(a, b)             coop_win_fopen((a), (b))
#define unlink(a)               coop_win_unlink(a)
#define access(a, b)            coop_win_access((a), (b))
#define stat(a, b)              coop_win_stat((a), (b))
#define getpid()                ((int) GetCurrentProcessId())

#ifndef ssize_t
typedef long long coop_ssize_t;
#define ssize_t coop_ssize_t
#endif

/* ---------------------------------------------------------------- send()/recv() flags
 * and the pointer types winsock disagrees with POSIX about. Added for the TV app's own
 * co-op input/audio sources, which the Qt client never compiled.
 *
 * MSG_NOSIGNAL asks POSIX not to raise SIGPIPE on a dead peer. Windows has no SIGPIPE at
 * all, so ZERO is the correct translation, not a stub -- the behaviour it requests is
 * already the behaviour you get.
 *
 * MSG_DONTWAIT is NOT equally harmless. POSIX uses it for a one-off non-blocking call on
 * an otherwise blocking socket; winsock has no per-call equivalent, and mapping it to 0
 * turns such a call BLOCKING. Every co-op socket here is already put in non-blocking mode
 * (coop_ipc_set_nonblocking / the O_NONBLOCK fcntl above) before any of these calls, so
 * the flag is redundant on this path -- but if a future caller relies on it against a
 * blocking socket, it will hang, and that is why this says so. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

/* Keep the TV transport's POSIX errno semantics on Winsock. */
#define send(s, buf, len, flags)  coop_win_send((s), (buf), (len), (flags))
#define recv(s, buf, len, flags)  coop_win_recv((s), (buf), (len), (flags))

/* winsock spells the address-length type `int`. */
#ifndef COOP_WIN_HAVE_SOCKLEN_T
#define COOP_WIN_HAVE_SOCKLEN_T
typedef int socklen_t;
#endif

/* setsockopt() takes const char* on winsock, not const void*. */
#define setsockopt(s, lvl, opt, val, len) \
        setsockopt((SOCKET) (s), (lvl), (opt), (const char *) (val), (int) (len))
#define getsockopt(s, lvl, opt, val, len) \
        getsockopt((SOCKET) (s), (lvl), (opt), (char *) (val), (int *) (len))

/* localtime_r is POSIX; the CRT has localtime_s with the arguments the other way round
 * and a different return convention. Wrap rather than edit the shared source. */
static inline struct tm *coop_win_localtime_r(const time_t *t, struct tm *out) {
    return localtime_s(out, t) == 0 ? out : NULL;
}
#define localtime_r(t, out) coop_win_localtime_r((t), (out))

/* BACKTRACES ARE A DIAGNOSTIC, NOT A FEATURE, and Windows has no execinfo. Returning zero
 * frames is the honest answer: the caller prints "0 frames" rather than a wrong trace.
 * Said out loud here because a silent empty backtrace during a crash hunt would waste the
 * exact session it is meant to help. */
static inline int coop_win_backtrace(void **buf, int size) { (void) buf; (void) size; return 0; }
static inline char **coop_win_backtrace_symbols(void *const *buf, int size) {
    (void) buf; (void) size; return NULL;
}
#define backtrace(b, s)          coop_win_backtrace((b), (s))
#define backtrace_symbols(b, s)  coop_win_backtrace_symbols((b), (s))

/* Windows has no SIGBUS. Mapping it to SIGSEGV is the faithful translation rather than a
 * fudge: the faults POSIX reports as SIGBUS (bad address, misaligned access) surface on
 * Windows as access violations, which the CRT delivers as SIGSEGV. The crash handler is
 * registered for SIGSEGV anyway, so this just makes the second signal() call idempotent
 * instead of failing with SIG_ERR. */
#ifndef SIGBUS
#define SIGBUS SIGSEGV
#endif

#endif /* _WIN32 */
#endif /* COOP_WIN_COMPAT_H */

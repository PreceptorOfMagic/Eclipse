/* See diag_logs.h: keep the app's log in rotating files, and serve them read-only while asked to. */
#include "diag_logs.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include "logging.h"
#include "util/crash_log.h"
#include "util/path.h"
#include "uuidstr.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#define sock_close closesocket
#define sock_shutdown(s) shutdown((s), SD_BOTH)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
#define sock_close close
#define sock_shutdown(s) shutdown((s), SHUT_RDWR)
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0   /* Windows has no SIGPIPE */
#endif

static bool keeping = false;

/* ---------------------------------------------------------------------------------------------------
 * KEEP
 * ------------------------------------------------------------------------------------------------- */

void diag_logs_dir(const char *conf_dir, char *dest, size_t maxlen) {
    path_join_to(dest, maxlen, conf_dir != NULL ? conf_dir : ".", "logs");
}

bool diag_logs_active(void) {
    return keeping;
}

/* The kept logs, oldest first, one file name per line: an index instead of a directory listing, so it
 * works the same through the UTF-8 path layer on every platform. */
static int read_index(const char *dir, char names[][64], int max) {
    char path[1024];
    path_join_to(path, sizeof(path), dir, "index.txt");
    FILE *f = path_fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    int n = 0;
    char line[128];
    while (n < max && fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "eclipse-", 8) == 0 && strstr(line, "..") == NULL && strchr(line, '/') == NULL &&
            strchr(line, '\\') == NULL) {
            snprintf(names[n++], 64, "%.63s", line);
        }
    }
    fclose(f);
    return n;
}

static void write_index(const char *dir, char names[][64], int n) {
    char path[1024];
    path_join_to(path, sizeof(path), dir, "index.txt");
    FILE *f = path_fopen(path, "w");
    if (f == NULL) {
        return;
    }
    for (int i = 0; i < n; i++) {
        fprintf(f, "%s\n", names[i]);
    }
    fclose(f);
}

static char run_marker[1024];

bool diag_logs_run_begin(const char *conf_dir, char *previous, size_t previous_len) {
    if (previous != NULL && previous_len > 0) {
        previous[0] = '\0';
    }
    char dir[1024];
    diag_logs_dir(conf_dir, dir, sizeof(dir));
    if (conf_dir == NULL || path_dir_ensure(dir) != 0) {
        return false;
    }
    path_join_to(run_marker, sizeof(run_marker), dir, "running.txt");
    bool unclean = false;
    FILE *f = path_fopen(run_marker, "r");
    if (f != NULL) {
        unclean = true;
        char line[256] = "";
        if (fgets(line, sizeof(line), f) != NULL) {
            line[strcspn(line, "\r\n")] = '\0';
        }
        fclose(f);
        if (previous != NULL && previous_len > 0) {
            snprintf(previous, previous_len, "%s", line[0] != '\0' ? line : "no details");
        }
    }
    f = path_fopen(run_marker, "w");
    if (f == NULL) {
        run_marker[0] = '\0';
        return unclean;
    }
    time_t now = time(NULL);
    struct tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_now);
    fprintf(f, "started %s, Eclipse %s\n", stamp, APP_VERSION);
    fclose(f);
    return unclean;
}

void diag_logs_run_end(void) {
    if (run_marker[0] != '\0') {
        path_file_remove(run_marker);
        run_marker[0] = '\0';
    }
}

void diag_logs_start(const char *conf_dir) {
    if (keeping || conf_dir == NULL) {
        return;
    }
    char dir[1024];
    diag_logs_dir(conf_dir, dir, sizeof(dir));
    if (path_dir_ensure(dir) != 0) {
        commons_log_warn("DiagLogs", "Cannot create %s: keeping no log", dir);
        return;
    }
    char names[DIAG_LOGS_KEEP + 32][64];
    int n = read_index(dir, names, DIAG_LOGS_KEEP + 31);
    while (n >= DIAG_LOGS_KEEP) {   /* make room: drop the oldest */
        char old[1024];
        path_join_to(old, sizeof(old), dir, names[0]);
        path_file_remove(old);
        memmove(names[0], names[1], sizeof(names[0]) * (size_t) (n - 1));
        n--;
    }
    time_t now = time(NULL);
    struct tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    char name[64];
    strftime(name, sizeof(name), "eclipse-%Y%m%d-%H%M%S.log", &tm_now);
    char path[1024];
    path_join_to(path, sizeof(path), dir, name);
    fflush(stderr);
    if (path_freopen(path, "a", stderr) == NULL) {
        commons_log_warn("DiagLogs", "Cannot open %s: keeping no log", path);
        return;
    }
#ifdef _WIN32
    /* Windows' CRT treats _IOLBF as FULL buffering: the last lines before a crash never reached the file
     * (measured 2026-09-29). A kept log exists for exactly those lines, so write it unbuffered there. */
    setvbuf(stderr, NULL, _IONBF, 0);
#else
    setvbuf(stderr, NULL, _IOLBF, 0);
#endif
#ifndef _WIN32
    /* webOS's logger prints info/debug lines to stdout: keep those too. */
    fflush(stdout);
    dup2(fileno(stderr), fileno(stdout));
    setvbuf(stdout, NULL, _IOLBF, 0);
#if defined(__GLIBC__)
    /* ...AND SEND THE stdout STREAM ITSELF THROUGH stderr's. The dup2 alone was not enough on the TV
     * (webOS 26, 2026-09-29): not one info line reached the kept log, not even after a clean close, while
     * every stderr line did. Why is not known; this routes info lines down the path measured to work.
     * glibc allows assigning stdout, and the logger reads it at every call. */
    stdout = stderr;
#endif
#endif
    snprintf(names[n++], 64, "%s", name);
    write_index(dir, names, n);
    keeping = true;
    crash_log_install();   /* a crash now ends the log with its signal/exception and stack (util/crash_log.h) */
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_now);
    commons_log_info("DiagLogs", "Kept diagnostic log started %s (%s)", stamp, path);
}

/* ---------------------------------------------------------------------------------------------------
 * SHARE
 * ------------------------------------------------------------------------------------------------- */

typedef enum { KIND_LOG, KIND_INI } file_kind;

typedef struct {
    char label[80];
    char path[1024];
    file_kind kind;
} shared_file_t;

static SDL_Thread *share_thread = NULL;
static SDL_atomic_t share_stop;
static sock_t listen_sock = SOCK_INVALID;
/* Only the worker closes this socket. Stop may shut it down under the lock to
 * interrupt a blocked send/recv without racing descriptor reuse. */
static SDL_SpinLock share_client_lock;
static sock_t share_client = SOCK_INVALID;
#ifdef _WIN32
static bool share_winsock = false;
#endif
static char share_token[12];
static char share_conf[1024];
static char *share_summary = NULL;

static int shared_files(const char *conf_dir, shared_file_t *out, int max) {
    int n = 0;
    char dir[1024];
    diag_logs_dir(conf_dir, dir, sizeof(dir));
    char names[DIAG_LOGS_KEEP + 32][64];
    int kept = read_index(dir, names, DIAG_LOGS_KEEP + 31);
    for (int i = kept - 1; i >= 0 && n < max; i--) {   /* newest first */
        snprintf(out[n].label, sizeof(out[n].label), "%s%s", names[i], i == kept - 1 ? " (latest)" : "");
        path_join_to(out[n].path, sizeof(out[n].path), dir, names[i]);
        out[n++].kind = KIND_LOG;
    }
#ifdef AURORA_DIAG_LOG
    if (n < max) {
        snprintf(out[n].label, sizeof(out[n].label), "co-op and streaming diagnostics");
        snprintf(out[n].path, sizeof(out[n].path), "%s", AURORA_DIAG_LOG);
        out[n++].kind = KIND_LOG;
    }
#endif
#ifdef AURORA_SECONDARY_LOG
    if (n < max) {
        snprintf(out[n].label, sizeof(out[n].label), "co-op second PC diagnostics");
        snprintf(out[n].path, sizeof(out[n].path), "%s", AURORA_SECONDARY_LOG);
        out[n++].kind = KIND_LOG;
    }
#endif
    /* EVERY OTHER DIAGNOSTIC FILE THE APP WRITES (support logs must stand alone, 2026-09-30). A file that does not
     * exist on this device is listed as such, which is itself an answer. */
    const struct {
        const char *label;
        const char *path;
    } extra[] = {
#ifdef AURORA_MULTIVIEW_LOG
            {"co-op multiview / TV capture probe", AURORA_MULTIVIEW_LOG},
#endif
#if defined(TARGET_WEBOS)
#ifdef AURORA_CRASH_LOG
            {"crashes and hangs (webOS)", AURORA_CRASH_LOG},
#endif
#ifdef AURORA_STDERR_LOG
            {"early stderr: loader, GStreamer (webOS)", AURORA_STDERR_LOG},
#endif
#endif
            {NULL, NULL},
    };
    for (size_t i = 0; extra[i].label != NULL && n < max; i++) {
        snprintf(out[n].label, sizeof(out[n].label), "%s", extra[i].label);
        snprintf(out[n].path, sizeof(out[n].path), "%s", extra[i].path);
        out[n++].kind = KIND_LOG;
    }
#if defined(_WIN32)
    const char *program_data = getenv("ProgramData");
    if (program_data != NULL && n < max) {
        snprintf(out[n].label, sizeof(out[n].label), "this-PC host hand-over (Windows)");
        snprintf(out[n].path, sizeof(out[n].path), "%s\\Eclipse\\local-host.log", program_data);
        out[n++].kind = KIND_LOG;
    }
#endif
    const char *inis[] = {"moonlight.ini", "hosts.ini"};
    const char *labels[] = {"settings (secrets removed)", "computers"};
    for (int i = 0; i < 2 && n < max; i++) {
        snprintf(out[n].label, sizeof(out[n].label), "%s", labels[i]);
        path_join_to(out[n].path, sizeof(out[n].path), conf_dir, inis[i]);
        out[n++].kind = KIND_INI;
    }
    return n;
}

/* Where shared text goes: a share page connection, or the one-file bundle (diag_logs_write_bundle). */
typedef struct {
    sock_t s;
    FILE *f;
} out_t;

static bool send_all(const out_t *o, const char *data, size_t len) {
    if (o->f != NULL) {
        return fwrite(data, 1, len, o->f) == len;
    }
    while (len > 0) {
        int sent = send(o->s, data, (int) len, MSG_NOSIGNAL);
        if (sent <= 0) {
            return false;
        }
        data += sent;
        len -= (size_t) sent;
    }
    return true;
}

static bool send_str(const out_t *o, const char *text) {
    return send_all(o, text, strlen(text));
}

/* The report goes inside <pre> on the page: a device or host name with < or & must not break it. */
static bool send_html(const out_t *o, const char *text) {
    char buf[512];
    size_t n = 0;
    for (const char *p = text; *p != '\0'; p++) {
        const char *rep = *p == '<' ? "&lt;" : *p == '>' ? "&gt;" : *p == '&' ? "&amp;" : NULL;
        const size_t need = rep ? strlen(rep) : 1;
        if (n + need >= sizeof(buf)) {
            if (!send_all(o, buf, n)) {
                return false;
            }
            n = 0;
        }
        if (rep) {
            memcpy(buf + n, rep, need);
        } else {
            buf[n] = *p;
        }
        n += need;
    }
    return send_all(o, buf, n);
}

/* A secret in the settings (the host PIN for automatic unlock, anything like it) is never served. */
static bool key_is_secret(const char *line) {
    char key[64] = {0};
    size_t i = 0;
    while (line[i] != '\0' && line[i] != '=' && i < sizeof(key) - 1) {
        key[i] = (char) tolower((unsigned char) line[i]);
        i++;
    }
    return line[i] == '=' && (strstr(key, "pin_code") != NULL || strstr(key, "password") != NULL ||
                              strstr(key, "token") != NULL || strstr(key, "secret") != NULL);
}

/* Log files are written for a terminal: drop the colour escapes (ESC [ ... letter) on the way out. */
static bool send_file(const out_t *o, const shared_file_t *file) {
    FILE *f = path_fopen(file->path, "rb");
    if (f == NULL) {
        return send_str(o, "(this file does not exist on this device)\n");
    }
    bool ok = true;
    if (file->kind == KIND_INI) {
        char line[2048];
        while (ok && fgets(line, sizeof(line), f) != NULL) {
            if (key_is_secret(line)) {
                char *eq = strchr(line, '=');
                *eq = '\0';
                ok = send_str(o, line) && send_str(o, "= <removed>\n");
            } else {
                ok = send_str(o, line);
            }
        }
    } else {
        char in[16384], out[16384];
        int esc = 0;   /* 0 plain, 1 after ESC, 2 inside ESC [ ... */
        size_t got;
        while (ok && (got = fread(in, 1, sizeof(in), f)) > 0) {
            size_t used = 0;
            for (size_t i = 0; i < got; i++) {
                const unsigned char c = (unsigned char) in[i];
                if (esc == 0) {
                    if (c == 0x1b) {
                        esc = 1;
                    } else {
                        out[used++] = (char) c;
                    }
                } else if (esc == 1) {
                    esc = c == '[' ? 2 : 0;
                } else if (c >= 0x40 && c <= 0x7e) {
                    esc = 0;
                }
            }
            ok = send_all(o, out, used);
        }
    }
    fclose(f);
    return ok;
}

/* The summary, then every file under a heading: the page's "everything" download, and the bundle. */
static bool send_everything(const out_t *o, const char *conf_dir, const char *summary) {
    shared_file_t files[DIAG_LOGS_KEEP + 16];
    const int count = shared_files(conf_dir, files, (int) (sizeof(files) / sizeof(files[0])));
    if (!send_str(o, summary != NULL ? summary : "")) {
        return false;
    }
    char head[128];
    for (int i = 0; i < count; i++) {
        snprintf(head, sizeof(head), "\n\n===== %.79s =====\n", files[i].label);
        if (!send_str(o, head) || !send_file(o, &files[i])) {
            return false;
        }
    }
    return true;
}

static long file_size(const char *path) {
    FILE *f = path_fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    return size;
}

static void respond(sock_t sock, const char *target) {
    const out_t out = {sock, NULL}, *o = &out;
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "/%s/", share_token);
    const size_t plen = strlen(prefix);
    if (strncmp(target, prefix, plen - 1) != 0 || (target[plen - 1] != '/' && target[plen - 1] != '\0')) {
        send_str(o, "HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nNot found\n");
        return;
    }
    if (target[plen - 1] == '\0') {   /* "/token" -> "/token/", so the page's relative links resolve */
        char head[160];
        snprintf(head, sizeof(head), "HTTP/1.0 301 Moved\r\nLocation: %s\r\nConnection: close\r\n\r\n", prefix);
        send_str(o, head);
        return;
    }
    const char *rest = target + plen;
    shared_file_t files[DIAG_LOGS_KEEP + 16];
    const int count = shared_files(share_conf, files, (int) (sizeof(files) / sizeof(files[0])));
    char head[512];
    if (*rest == '\0') {
        send_str(o, "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
                    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
                    "<title>Eclipse logs</title><body style='font-family:sans-serif;max-width:48em;margin:1em auto'>"
                    "<h1>Eclipse diagnostic logs</h1><pre style='white-space:pre-wrap'>");
        send_html(o, share_summary != NULL ? share_summary : "");
        send_str(o, "</pre><p><a href='all.txt'><b>Download everything as one file</b></a> "
                    "&mdash; send that file to whoever asked for your logs.</p><ul>");
        for (int i = 0; i < count; i++) {
            const long size = file_size(files[i].path);
            snprintf(head, sizeof(head), "<li><a href='f/%d'>%.79s</a> (%s)</li>", i, files[i].label,
                     size < 0 ? "none" : size < 2048 ? "small" : "");
            if (size >= 2048) {
                snprintf(head, sizeof(head), "<li><a href='f/%d'>%.79s</a> (%ld KB)</li>", i, files[i].label,
                         size / 1024);
            }
            send_str(o, head);
        }
        send_str(o, "</ul><p>This page closes when you leave the Share logs screen in Eclipse.</p>");
        return;
    }
    if (strcmp(rest, "all.txt") == 0) {
        send_str(o, "HTTP/1.0 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                    "Content-Disposition: attachment; filename=\"eclipse-logs.txt\"\r\nConnection: close\r\n\r\n");
        send_everything(o, share_conf, share_summary);
        return;
    }
    if (strncmp(rest, "f/", 2) == 0 && isdigit((unsigned char) rest[2])) {
        const int i = atoi(rest + 2);
        if (i >= 0 && i < count) {
            snprintf(head, sizeof(head), "HTTP/1.0 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                                         "Connection: close\r\n\r\n");
            send_str(o, head);
            send_file(o, &files[i]);
            return;
        }
    }
    send_str(o, "HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nNot found\n");
}

static void serve_one(sock_t s) {
    char req[2048];
    size_t got = 0;
    const Uint32 deadline = SDL_GetTicks() + 3000;
    while (!SDL_AtomicGet(&share_stop) && got < sizeof(req) - 1 &&
           !SDL_TICKS_PASSED(SDL_GetTicks(), deadline)) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        struct timeval tv = {0, 200000};
        if (select((int) s + 1, &rd, NULL, NULL, &tv) <= 0) {
            continue;
        }
        int r = recv(s, req + got, (int) (sizeof(req) - 1 - got), 0);
        if (r <= 0) {
            break;
        }
        got += (size_t) r;
        req[got] = '\0';
        if (strstr(req, "\r\n\r\n") != NULL || strstr(req, "\n\n") != NULL) {
            break;
        }
    }
    req[got] = '\0';
    char method[8] = {0}, target[512] = {0};
    if (!SDL_AtomicGet(&share_stop) &&
        sscanf(req, "%7s %511s", method, target) == 2 && strcmp(method, "GET") == 0) {
        respond(s, target);
    }
}

static int share_thread_main(void *arg) {
    (void) arg;
    while (!SDL_AtomicGet(&share_stop)) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(listen_sock, &rd);
        struct timeval tv = {0, 250000};
        if (select((int) listen_sock + 1, &rd, NULL, NULL, &tv) <= 0) {
            continue;
        }
        sock_t c = accept(listen_sock, NULL, NULL);
        if (c == SOCK_INVALID) {
            continue;
        }
        SDL_AtomicLock(&share_client_lock);
        const bool stopping = SDL_AtomicGet(&share_stop) != 0;
        if (!stopping) {
            share_client = c;
        }
        SDL_AtomicUnlock(&share_client_lock);
        if (!stopping) {
            serve_one(c);
        }
        SDL_AtomicLock(&share_client_lock);
        share_client = SOCK_INVALID;
        sock_close(c);
        SDL_AtomicUnlock(&share_client_lock);
    }
    return 0;
}

bool diag_logs_write_bundle(const char *conf_dir, const char *summary, char *path, size_t path_len) {
    char dir[1024];
    diag_logs_dir(conf_dir != NULL ? conf_dir : ".", dir, sizeof(dir));
    if (path_dir_ensure(dir) != 0) {
        return false;
    }
    path_join_to(path, path_len, dir, DIAG_LOGS_BUNDLE_NAME);
    FILE *f = path_fopen(path, "wb");
    if (f == NULL) {
        commons_log_warn("DiagLogs", "Cannot write %s", path);
        return false;
    }
    const out_t out = {SOCK_INVALID, f};
    const bool ok = send_everything(&out, conf_dir != NULL ? conf_dir : ".", summary);
    fclose(f);
    commons_log_info("DiagLogs", "Wrote the log bundle %s%s", path, ok ? "" : " (incomplete)");
    return ok;
}

/* The address other devices reach this one at: the local end of a (never sent) UDP "connection" towards
 * the network -- the interface the default route uses. */
static bool primary_ipv4(char *dest, size_t maxlen) {
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == SOCK_INVALID) {
        return false;
    }
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(9);
    to.sin_addr.s_addr = htonl(0xC0000201u);   /* 192.0.2.1, TEST-NET: nothing is sent */
    bool ok = false;
    if (connect(s, (struct sockaddr *) &to, sizeof(to)) == 0) {
        struct sockaddr_in me = {0};
        socklen_t len = sizeof(me);
        if (getsockname(s, (struct sockaddr *) &me, &len) == 0 && me.sin_addr.s_addr != 0) {
            ok = inet_ntop(AF_INET, &me.sin_addr, dest, (socklen_t) maxlen) != NULL;
        }
    }
    sock_close(s);
    return ok;
}

bool diag_logs_share_start(const char *conf_dir, const char *summary, char *urls, size_t urls_len) {
    diag_logs_share_stop();
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return false;
    }
    share_winsock = true;
#endif
    snprintf(share_conf, sizeof(share_conf), "%s", conf_dir != NULL ? conf_dir : ".");
    free(share_summary);
    share_summary = summary != NULL ? strdup(summary) : NULL;
    uuidstr_t id;
    uuidstr_random(&id);
    snprintf(share_token, sizeof(share_token), "%.8s", id.data);
    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == SOCK_INVALID) {
        diag_logs_share_stop();
        return false;
    }
    int one = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof(one));
    int port = -1;
    for (int p = DIAG_LOGS_SHARE_PORT; p < DIAG_LOGS_SHARE_PORT + 10 && port < 0; p++) {
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t) p);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(listen_sock, (struct sockaddr *) &addr, sizeof(addr)) == 0) {
            port = p;
        }
    }
    if (port < 0 || listen(listen_sock, 4) != 0) {
        sock_close(listen_sock);
        listen_sock = SOCK_INVALID;
        commons_log_warn("DiagLogs", "Cannot open the share page: no free port from %d", DIAG_LOGS_SHARE_PORT);
        diag_logs_share_stop();
        return false;
    }
    SDL_AtomicSet(&share_stop, 0);
    share_thread = SDL_CreateThread(share_thread_main, "diaglogs", NULL);
    if (share_thread == NULL) {
        sock_close(listen_sock);
        listen_sock = SOCK_INVALID;
        diag_logs_share_stop();
        return false;
    }
    char ip[64];
    if (!primary_ipv4(ip, sizeof(ip))) {
        snprintf(ip, sizeof(ip), "<this device's IP address>");
    }
    snprintf(urls, urls_len, "http://%s:%d/%s/", ip, port, share_token);
    commons_log_info("DiagLogs", "Sharing logs at %s", urls);
    return true;
}

void diag_logs_share_stop(void) {
    if (share_thread != NULL) {
        SDL_AtomicSet(&share_stop, 1);
        SDL_AtomicLock(&share_client_lock);
        if (share_client != SOCK_INVALID) {
            sock_shutdown(share_client);
        }
        SDL_AtomicUnlock(&share_client_lock);
        SDL_WaitThread(share_thread, NULL);
        share_thread = NULL;
        commons_log_info("DiagLogs", "Stopped sharing logs");
    }
    if (listen_sock != SOCK_INVALID) {
        sock_close(listen_sock);
        listen_sock = SOCK_INVALID;
    }
    free(share_summary);
    share_summary = NULL;
#ifdef _WIN32
    if (share_winsock) {
        WSACleanup();
        share_winsock = false;
    }
#endif
}

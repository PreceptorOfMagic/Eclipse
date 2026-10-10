/* VENDOR DIAGNOSTICS (webOS only, compiled in only with -DAURORA_VENDOR_DIAG=ON).
 *
 * WHY THIS EXISTS. The TV keeps no log a developer can read -- no journal, an empty
 * /var/log -- so every vendor media library (NDL, starfish-media-pipeline, av-proxy)
 * logs into the void. After the webOS 26 (11.2.0) update the media load hung with no
 * error anywhere, and the only way to see why was a throwaway probe app that captured
 * those libraries' PmLog output in-process. This file is that capture, built into the
 * app so the NEXT firmware update can be diagnosed in one run instead of a day.
 *
 * 1. PmLog tap. The executable exports PmLogGetContext/PmLogString_/_PmLogMsgKV/
 *    PmLogSetContextLevel (vendor_diag.list), so the vendor libraries' calls resolve
 *    here before libPmLogLib. Callers inside libNDL_* and libplayerAPIs get a private
 *    debug-enabled context and their text goes to AURORA_VENDOR_LOG. Every other caller
 *    is forwarded to the real libPmLogLib.
 * 2. Hang watch and fatal-signal record, to AURORA_CRASH_LOG. The first arm redirects
 *    stderr to AURORA_STDERR_LOG, because webOS discards it and the loader, GLib and
 *    GStreamer report there. Each arm/disarm writes ARM/DONE with its duration. If an armed
 *    call has not returned after HANG_FIRST_DUMP_MS (and again at HANG_SECOND_DUMP_MS),
 *    every thread's state, syscall, pc/lr and raw stack words are written with
 *    /proc/self/maps. SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT write the same record, then
 *    re-raise. That is how "never returns" became `pc == &g_log` on webOS 26.
 *
 * Never enable this in a release build: it changes nothing on screen, but it sits on every
 * PmLog call in the process and replaces the fatal-signal handlers. */
#define _GNU_SOURCE
#include "util/diag_logs.h"
#include "vendor_diag.h"

#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef AURORA_VENDOR_LOG
#define AURORA_VENDOR_LOG "/tmp/aurora-vendor.log"
#endif

#define HANG_FIRST_DUMP_MS 8000
#define HANG_SECOND_DUMP_MS 30000

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *vlog = NULL;

static long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static FILE *vendor_log(void) {
    if (vlog == NULL) {
        vlog = fopen(AURORA_VENDOR_LOG, "a");
        if (vlog != NULL) {
            setvbuf(vlog, NULL, _IOLBF, 0);
            fprintf(vlog, "=== vendor log opened pid=%d mono_ms=%ld ===\n", (int) getpid(), mono_ms());
        }
    }
    return vlog;
}

void vendor_diag_note(const char *fmt, ...) {
    pthread_mutex_lock(&log_lock);
    FILE *f = vendor_log();
    if (f != NULL) {
        fprintf(f, "%ld NOTE ", mono_ms());
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
    }
    pthread_mutex_unlock(&log_lock);
}

/* ---- 1. PmLog tap ------------------------------------------------------------ */

typedef struct { int enabledLevel; int flags; } TapContext;   /* = PmLogContextInfo */
typedef const TapContext *TapContextRef;
struct fake_ctx { TapContext info; char name[64]; };
#define MAX_FAKE 64
static struct fake_ctx fakes[MAX_FAKE];
static int nfakes = 0;
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;

static void *pm_real(const char *sym) {
    void *f = dlsym(RTLD_NEXT, sym);
    if (f == NULL) {
        static void *h = NULL;
        if (h == NULL) { h = dlopen("libPmLogLib.so.3", RTLD_NOW | RTLD_GLOBAL); }
        if (h != NULL) { f = dlsym(h, sym); }
    }
    return f;
}

static int is_fake(TapContextRef c) {
    return c >= &fakes[0].info && c < &fakes[MAX_FAKE].info;
}

static int media_caller(void *ra) {
    Dl_info di;
    if (!dladdr(ra, &di) || di.dli_fname == NULL) { return 0; }
    return strstr(di.dli_fname, "libNDL") != NULL || strstr(di.dli_fname, "libplayerAPIs") != NULL;
}

__attribute__((visibility("default")))
int PmLogGetContext(const char *name, TapContextRef *pctx) {
    static int (*real)(const char *, TapContextRef *) = NULL;
    if (real == NULL) { real = (int (*)(const char *, TapContextRef *)) pm_real("PmLogGetContext"); }
    int rc = real != NULL ? real(name, pctx) : -1;
    if (!media_caller(__builtin_return_address(0))) { return rc; }
    pthread_mutex_lock(&fake_lock);
    for (int i = 0; i < nfakes; i++) {
        if (strcmp(fakes[i].name, name ? name : "") == 0) {
            *pctx = &fakes[i].info;
            pthread_mutex_unlock(&fake_lock);
            return 0;
        }
    }
    if (nfakes < MAX_FAKE) {
        struct fake_ctx *f = &fakes[nfakes++];
        f->info.enabledLevel = 7;   /* debug */
        snprintf(f->name, sizeof(f->name), "%s", name ? name : "");
        *pctx = &f->info;
        rc = 0;
    }
    pthread_mutex_unlock(&fake_lock);
    return rc;
}

__attribute__((visibility("default")))
int PmLogSetContextLevel(TapContextRef c, int level) {
    static int (*real)(TapContextRef, int) = NULL;
    if (is_fake(c)) { return 0; }
    if (real == NULL) { real = (int (*)(TapContextRef, int)) pm_real("PmLogSetContextLevel"); }
    return real != NULL ? real(c, level) : 0;
}

__attribute__((visibility("default")))
int PmLogString_(TapContextRef c, int level, const char *msgid, const char *kv, const char *msg) {
    static int (*real)(TapContextRef, int, const char *, const char *, const char *) = NULL;
    if (is_fake(c)) {
        pthread_mutex_lock(&log_lock);
        FILE *f = vendor_log();
        if (f != NULL) {
            fprintf(f, "%ld %s L%d [%s] %s %s\n", mono_ms(), ((const struct fake_ctx *) c)->name, level,
                    msgid ? msgid : "-", kv ? kv : "", msg ? msg : "");
        }
        pthread_mutex_unlock(&log_lock);
        return 0;
    }
    if (real == NULL) {
        real = (int (*)(TapContextRef, int, const char *, const char *, const char *)) pm_real("PmLogString_");
    }
    return real != NULL ? real(c, level, msgid, kv, msg) : 0;
}

__attribute__((visibility("default")))
int _PmLogMsgKV(TapContextRef c, int level, unsigned flags, const char *msgid, size_t kv_count,
                const char *check_keywords, const char *check_formats, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt ? fmt : "", ap);
    va_end(ap);
    if (is_fake(c)) {
        pthread_mutex_lock(&log_lock);
        FILE *f = vendor_log();
        if (f != NULL) {
            fprintf(f, "%ld %s L%d [%s] %s\n", mono_ms(), ((const struct fake_ctx *) c)->name, level,
                    msgid ? msgid : "-", buf);
        }
        pthread_mutex_unlock(&log_lock);
        return 0;
    }
    /* Not a vendor media caller: forward as pre-formatted free text. Lossy for the
     * key/value structure, which nobody on this TV can read anyway. */
    static int (*real)(TapContextRef, int, unsigned, const char *, size_t, const char *, const char *,
                       const char *, ...) = NULL;
    if (real == NULL) {
        real = (int (*)(TapContextRef, int, unsigned, const char *, size_t, const char *, const char *,
                        const char *, ...)) pm_real("_PmLogMsgKV");
    }
    (void) kv_count;
    (void) check_keywords;
    (void) check_formats;
    return real != NULL ? real(c, level, flags, msgid, 0, "", "", "%s", buf) : 0;
}

/* ---- 2. Hang watch + fatal-signal record --------------------------------------
 * Everything below writes with raw write(2) to a pre-opened fd: no stdio, no malloc, no
 * dladdr, no mutex. A hang can be a lock (the loader lock, a malloc arena, a FILE lock)
 * and a crash handler runs in an unknown state, so anything that takes a lock could
 * block the very dump that is meant to explain the block. Addresses are printed raw
 * together with /proc/self/maps and symbolised OFFLINE against the TV's own libraries. */

#include <fcntl.h>

#ifndef AURORA_CRASH_LOG
#define AURORA_CRASH_LOG "/tmp/aurora-crash.log"
#endif
#ifndef AURORA_STDERR_LOG
#define AURORA_STDERR_LOG "/tmp/aurora-stderr.log"
#endif

static int crash_fd = -1;
/* A fatal signal is ALSO copied to fd 2 while a kept log owns it (util/diag_logs.h), so the kept log of the run
 * that crashed ends with the record instead of just stopping; the crash file itself is part of the share bundle. */
static int mirror_fd = -1;

static void w_str(const char *s) {
    if (crash_fd >= 0 && s != NULL) {
        size_t n = 0;
        while (s[n] != '\0') { n++; }
        ssize_t r = write(crash_fd, s, n);
        (void) r;
        if (mirror_fd >= 0) {
            r = write(mirror_fd, s, n);
            (void) r;
        }
    }
}

static void w_hex(uintptr_t v) {
    char b[11];
    b[0] = '0';
    b[1] = 'x';
    for (int i = 0; i < 8; i++) {
        unsigned d = (unsigned) (v >> (28 - 4 * i)) & 0xfu;
        b[2 + i] = (char) (d < 10 ? '0' + d : 'a' + d - 10);
    }
    b[10] = '\0';
    w_str(b);
}

static void w_dec(long v) {
    char b[24];
    int i = 23;
    b[i] = '\0';
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long) -v : (unsigned long) v;
    do { b[--i] = (char) ('0' + u % 10); u /= 10; } while (u != 0 && i > 1);
    if (neg) { b[--i] = '-'; }
    w_str(b + i);
}

static void w_file(const char *path) {
    int in = open(path, O_RDONLY);
    if (in < 0) { return; }
    char buf[1024];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (mirror_fd >= 0) {
            ssize_t m = write(mirror_fd, buf, (size_t) n);
            (void) m;
        }
        ssize_t r = write(crash_fd, buf, (size_t) n);
        (void) r;
    }
    close(in);
}

static void w_stack(uintptr_t sp, int words) {
    const volatile uintptr_t *p = (const volatile uintptr_t *) sp;
    for (int i = 0; i < words; i++) {
        if ((i & 7) == 0) { w_str("\n    "); }
        w_hex(p[i]);
        w_str(" ");
    }
    w_str("\n");
}

struct reg_snapshot { volatile int tid; volatile uintptr_t pc, lr, sp; };
static struct reg_snapshot snaps[64];
static volatile int nsnaps = 0;

static pthread_mutex_t watch_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *watch_label = NULL;
static long watch_since = 0;
static int watch_dumps = 0;
static int diag_started = 0;

static void on_snapshot_signal(int sig, siginfo_t *si, void *ctx) {
    (void) sig;
    (void) si;
    const ucontext_t *uc = ctx;
    int i = __sync_fetch_and_add(&nsnaps, 1);
    if (i < 64) {
        snaps[i].pc = uc->uc_mcontext.arm_pc;
        snaps[i].lr = uc->uc_mcontext.arm_lr;
        snaps[i].sp = uc->uc_mcontext.arm_sp;
        snaps[i].tid = (int) syscall(SYS_gettid);
    }
}

static void on_fatal_signal(int sig, siginfo_t *si, void *ctx) {
    const ucontext_t *uc = ctx;
    mirror_fd = diag_logs_active() ? 2 : -1;
    w_str("\n=== FATAL signal ");
    w_dec(sig);
    w_str(" code ");
    w_dec(si != NULL ? si->si_code : 0);
    w_str(" addr ");
    w_hex(si != NULL ? (uintptr_t) si->si_addr : 0);
    w_str(" tid ");
    w_dec((long) syscall(SYS_gettid));
    w_str(" mono_ms ");
    w_dec(mono_ms());
    w_str("\n  pc ");
    w_hex(uc->uc_mcontext.arm_pc);
    w_str(" lr ");
    w_hex(uc->uc_mcontext.arm_lr);
    w_str(" sp ");
    w_hex(uc->uc_mcontext.arm_sp);
    w_str(" r0 ");
    w_hex(uc->uc_mcontext.arm_r0);
    w_str("\n  stack:");
    w_stack(uc->uc_mcontext.arm_sp, 512);
    w_str("  maps:\n");
    w_file("/proc/self/maps");
    w_str("=== END FATAL\n");
    signal(sig, SIG_DFL);
    raise(sig);
}

static void dump_threads(const char *label, long blocked_ms) {
    const int self = (int) syscall(SYS_gettid);
    w_str("\n=== HANG '");
    w_str(label);
    w_str("' not returned after ");
    w_dec(blocked_ms);
    w_str(" ms, mono_ms ");
    w_dec(mono_ms());
    w_str("\n");
    nsnaps = 0;
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    while (d != NULL && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') { continue; }
        const int tid = atoi(e->d_name);
        char p[96];
        w_str("  tid ");
        w_dec(tid);
        w_str(tid == self ? " (watch) comm=" : " comm=");
        snprintf(p, sizeof(p), "/proc/self/task/%d/comm", tid);
        w_file(p);
        w_str("    wchan=");
        snprintf(p, sizeof(p), "/proc/self/task/%d/wchan", tid);
        w_file(p);
        w_str(" syscall=");
        snprintf(p, sizeof(p), "/proc/self/task/%d/syscall", tid);
        w_file(p);
        if (tid != self) { syscall(SYS_tgkill, getpid(), tid, SIGRTMIN + 5); }
    }
    if (d != NULL) { closedir(d); }
    usleep(300 * 1000);
    const int n = nsnaps < 64 ? nsnaps : 64;
    for (int i = 0; i < n; i++) {
        w_str("  == tid ");
        w_dec(snaps[i].tid);
        w_str(" pc ");
        w_hex(snaps[i].pc);
        w_str(" lr ");
        w_hex(snaps[i].lr);
        w_str(" sp ");
        w_hex(snaps[i].sp);
        w_stack(snaps[i].sp, 256);
    }
    w_str("  maps:\n");
    w_file("/proc/self/maps");
    w_str("=== END HANG\n");
}

static void *watch_thread(void *arg) {
    (void) arg;
    for (;;) {
        usleep(250 * 1000);
        pthread_mutex_lock(&watch_lock);
        const char *label = watch_label;
        const long since = watch_since;
        const int dumps = watch_dumps;
        pthread_mutex_unlock(&watch_lock);
        if (label == NULL) { continue; }
        const long el = mono_ms() - since;
        if ((dumps == 0 && el > HANG_FIRST_DUMP_MS) || (dumps == 1 && el > HANG_SECOND_DUMP_MS)) {
            dump_threads(label, el);
            pthread_mutex_lock(&watch_lock);
            if (watch_label == label && watch_since == since) { watch_dumps = dumps + 1; }
            pthread_mutex_unlock(&watch_lock);
        }
    }
    return NULL;
}

static void diag_start(void) {
    crash_fd = open(AURORA_CRASH_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
    w_str("\n=== diag start pid ");
    w_dec(getpid());
    w_str(" mono_ms ");
    w_dec(mono_ms());
    w_str("\n");
    /* The loader, GStreamer and abort() all report on stderr, which webOS discards -- UNLESS a kept log is
     * already taking it (util/diag_logs.h): this first arm runs at the END of app start, after the kept log
     * opened, and redirecting fd 2 here would silently move every later line out of it. (Missed only by luck
     * on 2026-09-29: another uid owned the /tmp file, so the open failed.) */
    int err_fd = diag_logs_active() ? -1 : open(AURORA_STDERR_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (err_fd >= 0) {
        dup2(err_fd, 2);
        close(err_fd);
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_snapshot_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGRTMIN + 5, &sa, NULL);
    struct sigaction fa;
    memset(&fa, 0, sizeof(fa));
    fa.sa_sigaction = on_fatal_signal;
    fa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    const int fatal[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++) { sigaction(fatal[i], &fa, NULL); }
    pthread_t t;
    if (pthread_create(&t, NULL, watch_thread, NULL) == 0) { pthread_detach(t); }
}

void vendor_diag_hang_arm(const char *label) {
    pthread_mutex_lock(&watch_lock);
    if (!diag_started) {
        diag_start();
        diag_started = 1;
    }
    watch_label = label;
    watch_since = mono_ms();
    watch_dumps = 0;
    pthread_mutex_unlock(&watch_lock);
    w_str("ARM ");
    w_str(label);
    w_str(" tid ");
    w_dec((long) syscall(SYS_gettid));
    w_str(" mono_ms ");
    w_dec(mono_ms());
    w_str("\n");
    vendor_diag_note("ARM %s tid=%ld", label, (long) syscall(SYS_gettid));
}

void vendor_diag_hang_disarm(const char *result) {
    pthread_mutex_lock(&watch_lock);
    const char *label = watch_label;
    const long since = watch_since;
    watch_label = NULL;
    pthread_mutex_unlock(&watch_lock);
    w_str("DONE ");
    w_str(label ? label : "?");
    w_str(" -> ");
    w_str(result ? result : "?");
    w_str(" after ");
    w_dec(mono_ms() - since);
    w_str(" ms\n");
    vendor_diag_note("DONE %s -> %s after %ld ms", label ? label : "?", result ? result : "?", mono_ms() - since);
}

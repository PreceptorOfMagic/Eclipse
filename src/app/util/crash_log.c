/* See crash_log.h. */
#include "crash_log.h"

#if defined(TARGET_WEBOS) && defined(AURORA_VENDOR_DIAG) && AURORA_VENDOR_DIAG

void crash_log_install(void) {
}

void crash_log_watch_ui(void) {
}

void crash_log_ui_alive(void) {
}

void crash_log_ui_shutdown(void) {
}

#else

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>

#define STALL_FIRST_S 5         /* no main-loop pass for this long = stuck */
#define STALL_STARTUP_S 20      /* ...before the first pass: the window and UI are still being made */
#define STALL_SHUTDOWN_S 15     /* ...once the app is closing: the main loop has stopped on purpose */
#define STALL_AGAIN_S 25        /* a second record this much later: did it move? */
#define STALL_MAX_RECORDS 6

static SDL_atomic_t ui_beats;
static SDL_atomic_t ui_closing;

/* Straight to fd 2 (the kept log), not through stdio or the logger: both take locks the stuck thread may hold. */
static void out_raw(const char *s, size_t n);

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void out(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        out_raw(buf, (size_t) n < sizeof(buf) ? (size_t) n : sizeof(buf) - 1);
    }
}

/* Shaped like a logger line, so it sits in time order with the rest. */
static void out_line(const char *text) {
    out("[%08.3f] [Watchdog]         W %s\n", (double) SDL_GetTicks() / 1000.0, text);
}

static bool watch_prepare(void);   /* on the UI thread: what the platform needs to sample it later */
static void dump_ui_stack(void);   /* on the watchdog thread */

static int watch_main(void *arg) {
    (void) arg;
    int last = SDL_AtomicGet(&ui_beats);
    int quiet = 0, records = 0;
    bool beaten = false, stuck = false;
    for (;;) {
        SDL_Delay(1000);
        const int now = SDL_AtomicGet(&ui_beats);
        if (now != last) {
            if (stuck) {
                char line[96];
                snprintf(line, sizeof(line), "The UI thread is running again, after about %d s stuck", quiet);
                out_line(line);
                stuck = false;
            }
            last = now;
            quiet = 0;
            beaten = true;
            continue;
        }
        quiet++;   /* counted in the watchdog's own seconds: a suspended process (sleep, SIGSTOP) is not a stall */
        const bool closing = SDL_AtomicGet(&ui_closing) != 0;
        const int first = closing ? STALL_SHUTDOWN_S : beaten ? STALL_FIRST_S : STALL_STARTUP_S;
        if ((quiet == first || quiet == first + STALL_AGAIN_S) && records < STALL_MAX_RECORDS) {
            const char *when = closing ? " while closing" : beaten ? "" : " while starting";
            char line[128];
            snprintf(line, sizeof(line), "The UI thread has not run for %d s%s: recording its stack", quiet, when);
            out_line(line);
            out("=== UI THREAD STUCK: no main-loop pass for %d s%s ===\n", quiet, when);
            dump_ui_stack();
            out("=== END UI THREAD STUCK ===\n");
            records++;
            stuck = true;
        }
    }
    return 0;
}

void crash_log_watch_ui(void) {
    static bool started = false;
    if (started || !watch_prepare()) {
        return;
    }
    started = true;
    SDL_Thread *t = SDL_CreateThread(watch_main, "uiwatch", NULL);
    if (t != NULL) {
        SDL_DetachThread(t);
    }
}

void crash_log_ui_alive(void) {
    SDL_AtomicAdd(&ui_beats, 1);
}

void crash_log_ui_shutdown(void) {
    SDL_AtomicSet(&ui_closing, 1);
}

#if defined(_WIN32)

#include <windows.h>
#include <dbghelp.h>
#include <io.h>

static void out_raw(const char *s, size_t n) {
    (void) _write(_fileno(stderr), s, (unsigned) n);
}

static void module_of(DWORD64 addr, char *name, size_t len, DWORD64 *base) {
    HMODULE m = NULL;
    name[0] = '\0';
    *base = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR) (uintptr_t) addr, &m) && m != NULL) {
        wchar_t w[MAX_PATH];
        if (GetModuleFileNameW(m, w, MAX_PATH) > 0) {
            const wchar_t *slash = wcsrchr(w, L'\\');
            WideCharToMultiByte(CP_UTF8, 0, slash ? slash + 1 : w, -1, name, (int) len, NULL, NULL);
        }
        *base = (DWORD64) (uintptr_t) m;
    }
    if (name[0] == '\0') {
        snprintf(name, len, "?");
    }
}

static bool syms_ready(void) {
    static int state = 0;   /* 0 = not tried, 1 = ready, -1 = failed; SymInitialize twice fails */
    if (state == 0) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        state = SymInitialize(GetCurrentProcess(), NULL, TRUE) ? 1 : -1;
    }
    return state == 1;
}

/* module+offset per frame (+ the nearest exported name, when dbghelp has one). */
static void walk(HANDLE thread, CONTEXT *ctx, PREAD_PROCESS_MEMORY_ROUTINE64 reader) {
    HANDLE proc = GetCurrentProcess();
    const bool syms = syms_ready();
    STACKFRAME64 f;
    memset(&f, 0, sizeof(f));
#if defined(_M_X64) || defined(__x86_64__)
    const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    f.AddrPC.Offset = ctx->Rip;
    f.AddrFrame.Offset = ctx->Rbp;
    f.AddrStack.Offset = ctx->Rsp;
#else
    const DWORD machine = IMAGE_FILE_MACHINE_I386;
    f.AddrPC.Offset = ctx->Eip;
    f.AddrFrame.Offset = ctx->Ebp;
    f.AddrStack.Offset = ctx->Esp;
#endif
    f.AddrPC.Mode = f.AddrFrame.Mode = f.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 48; i++) {
        if (!StackWalk64(machine, proc, thread, &f, ctx, reader, SymFunctionTableAccess64, SymGetModuleBase64, NULL) ||
            f.AddrPC.Offset == 0) {
            break;
        }
        char mod[128];
        DWORD64 base;
        module_of(f.AddrPC.Offset, mod, sizeof(mod), &base);
        char symbuf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *) symbuf;
        memset(symbuf, 0, sizeof(symbuf));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (syms && SymFromAddr(proc, f.AddrPC.Offset, &disp, sym)) {
            out("#%02d %s+0x%llx  %s+0x%llx\n", i, mod, (unsigned long long) (f.AddrPC.Offset - base), sym->Name,
                (unsigned long long) disp);
        } else {
            out("#%02d %s+0x%llx\n", i, mod, (unsigned long long) (f.AddrPC.Offset - base));
        }
    }
}

#define STACK_HEADER "--- stack (module+offset; scripts/support/crash_frames.py resolves them against this exact " \
                     "moonlight-tv.exe, which keeps its symbols) ---\n"

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    char mod[128];
    DWORD64 base;
    module_of((DWORD64) (uintptr_t) er->ExceptionAddress, mod, sizeof(mod), &base);
    out("\n=== CRASH: exception 0x%08lx at %p (%s+0x%llx), thread %lu\n", er->ExceptionCode, er->ExceptionAddress, mod,
        (unsigned long long) ((DWORD64) (uintptr_t) er->ExceptionAddress - base), GetCurrentThreadId());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const ULONG_PTR kind = er->ExceptionInformation[0];
        out("access violation: %s address %p\n",
            kind == 0 ? "reading" : kind == 1 ? "writing" : kind == 8 ? "executing" : "?",
            (void *) er->ExceptionInformation[1]);
    }
    out(STACK_HEADER);
    CONTEXT ctx = *ep->ContextRecord;
    walk(GetCurrentThread(), &ctx, NULL);
    out("=== END CRASH ===\n");
    return EXCEPTION_CONTINUE_SEARCH;   /* let Windows report and end the process as it would have */
}

void crash_log_install(void) {
    static int done = 0;
    if (!done) {
        done = 1;
        SetUnhandledExceptionFilter(crash_filter);
    }
}

/* The UI thread is sampled from the watchdog: suspend it, take its registers and COPY its stack, resume it at
 * once, and only then walk the copy. Nothing that can take a lock (dbghelp, the loader, the heap) runs while it is
 * suspended -- it may be holding that very lock. */
static HANDLE ui_thread;
static BYTE snap_stack[128 * 1024];
static DWORD64 snap_base;
static SIZE_T snap_len;

static BOOL CALLBACK snap_read(HANDLE proc, DWORD64 addr, PVOID buf, DWORD size, LPDWORD got) {
    if (addr >= snap_base && addr + size <= snap_base + snap_len) {
        memcpy(buf, snap_stack + (addr - snap_base), size);
        *got = size;
        return TRUE;
    }
    SIZE_T n = 0;
    const BOOL ok = ReadProcessMemory(proc, (LPCVOID) (uintptr_t) addr, buf, size, &n);
    *got = (DWORD) n;
    return ok;
}

static bool watch_prepare(void) {
    return DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &ui_thread,
                           THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0) != 0;
}

static void dump_ui_stack(void) {
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    if (SuspendThread(ui_thread) == (DWORD) -1) {
        out("(could not suspend the UI thread: error %lu)\n", GetLastError());
        return;
    }
    const BOOL got = GetThreadContext(ui_thread, &ctx);
    snap_len = 0;
    if (got) {
#if defined(_M_X64) || defined(__x86_64__)
        snap_base = ctx.Rsp;
#else
        snap_base = ctx.Esp;
#endif
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID) (uintptr_t) snap_base, &mbi, sizeof(mbi)) != 0) {
            const SIZE_T avail = (SIZE_T) ((uintptr_t) mbi.BaseAddress + mbi.RegionSize - (uintptr_t) snap_base);
            snap_len = avail < sizeof(snap_stack) ? avail : sizeof(snap_stack);
            memcpy(snap_stack, (const void *) (uintptr_t) snap_base, snap_len);
        }
    }
    ResumeThread(ui_thread);
    if (!got) {
        out("(could not read the UI thread's registers: error %lu)\n", GetLastError());
        return;
    }
#if defined(_M_X64) || defined(__x86_64__)
    const DWORD64 pc = ctx.Rip;
#else
    const DWORD64 pc = ctx.Eip;
#endif
    char mod[128];
    DWORD64 base;
    module_of(pc, mod, sizeof(mod), &base);
    out("UI thread %lu at %p (%s+0x%llx)\n", GetThreadId(ui_thread), (void *) (uintptr_t) pc, mod,
        (unsigned long long) (pc - base));
    out(STACK_HEADER);
    walk(ui_thread, &ctx, snap_read);
}

#else   /* Linux desktop and webOS */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <ucontext.h>
#include <unistd.h>

#if defined(__GLIBC__)
#include <execinfo.h>
#endif

static void out_raw(const char *s, size_t n) {
    ssize_t r = write(2, s, n);
    (void) r;
}

/* Only async-signal-safe calls in the handlers: write(2), open/read/close, no stdio. */
static void w_str(const char *s) {
    ssize_t r = write(2, s, strlen(s));
    (void) r;
}

static void w_num(uintptr_t v, int base) {
    char buf[32];
    int i = (int) sizeof(buf);
    buf[--i] = '\0';
    do {
        buf[--i] = "0123456789abcdef"[v % (uintptr_t) base];
        v /= (uintptr_t) base;
    } while (v != 0 && i > 2);
    if (base == 16) {
        buf[--i] = 'x';
        buf[--i] = '0';
    }
    w_str(buf + i);
}

static void w_int(int v) {
    if (v < 0) {
        w_str("-");
        w_num((uintptr_t) -(intptr_t) v, 10);
    } else {
        w_num((uintptr_t) v, 10);
    }
}

static void w_maps(void) {
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd >= 0) {
        char buf[4096];
        ssize_t got;
        while ((got = read(fd, buf, sizeof(buf))) > 0) {
            ssize_t r = write(2, buf, (size_t) got);
            (void) r;
        }
        close(fd);
    }
}

static uintptr_t context_pc(const ucontext_t *uc) {
#if defined(__x86_64__)
    return (uintptr_t) uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__i386__)
    return (uintptr_t) uc->uc_mcontext.gregs[REG_EIP];
#elif defined(__aarch64__)
    return (uintptr_t) uc->uc_mcontext.pc;
#elif defined(__arm__)
    return (uintptr_t) uc->uc_mcontext.arm_pc;
#else
    (void) uc;
    return 0;
#endif
}

static uintptr_t context_sp(const ucontext_t *uc) {
#if defined(__x86_64__)
    return (uintptr_t) uc->uc_mcontext.gregs[REG_RSP];
#elif defined(__i386__)
    return (uintptr_t) uc->uc_mcontext.gregs[REG_ESP];
#elif defined(__aarch64__)
    return (uintptr_t) uc->uc_mcontext.sp;
#elif defined(__arm__)
    return (uintptr_t) uc->uc_mcontext.arm_sp;
#else
    (void) uc;
    return 0;
#endif
}

static const char *signal_name(int sig) {
    switch (sig) {
        case SIGSEGV:
            return "SIGSEGV";
        case SIGBUS:
            return "SIGBUS";
        case SIGILL:
            return "SIGILL";
        case SIGFPE:
            return "SIGFPE";
        case SIGABRT:
            return "SIGABRT";
        default:
            return "?";
    }
}

static void crash_handler(int sig, siginfo_t *si, void *uctx) {
    const ucontext_t *uc = uctx;
    w_str("\n=== CRASH: signal ");
    w_num((uintptr_t) sig, 10);
    w_str(" (");
    w_str(signal_name(sig));
    w_str(")");
    /* si_code <= 0: a signal SENT by a process (kill, raise, abort) -- si_addr is then not an address at all but
     * overlaps the sender's pid and uid (the TV's test crash printed its own pid as the "fault address"). */
    if (si != NULL && si->si_code <= 0) {
        w_str(si->si_pid == getpid() ? ", raised by this process itself (abort/raise), code " : ", sent by pid ");
        if (si->si_pid != getpid()) {
            w_num((uintptr_t) si->si_pid, 10);
            w_str(" (uid ");
            w_num((uintptr_t) si->si_uid, 10);
            w_str("), code ");
        }
        w_int(si->si_code);
    } else {
        w_str(", fault address ");
        w_num(si != NULL ? (uintptr_t) si->si_addr : 0, 16);
        w_str(" (code ");
        w_int(si != NULL ? si->si_code : 0);
        w_str(")");
    }
    w_str(", pc ");
    w_num(context_pc(uc), 16);
#if defined(__arm__)
    /* 32-bit ARM (the TV's userland): backtrace() comes out empty there, so give the return address, the stack
     * pointer and the raw stack words -- enough to walk by hand against /proc/self/maps below. */
    w_str(", lr ");
    w_num((uintptr_t) uc->uc_mcontext.arm_lr, 16);
    w_str(", sp ");
    w_num(context_sp(uc), 16);
#endif
    w_str(", pid ");
    w_num((uintptr_t) getpid(), 10);
#if defined(__arm__)
    w_str("\n--- stack words ---");
    const uintptr_t *sp = (const uintptr_t *) context_sp(uc);
    for (int i = 0; sp != NULL && i < 128; i++) {
        w_str((i & 7) == 0 ? "\n" : " ");
        w_num(sp[i], 16);
    }
#endif
    w_str("\n--- backtrace ---\n");
#if defined(__GLIBC__)
    void *frames[64];
    const int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2);
#endif
    w_str("--- /proc/self/maps (to resolve the frames above) ---\n");
    w_maps();
    w_str("=== END CRASH ===\n");
    signal(sig, SIG_DFL);
    raise(sig);
}

void crash_log_install(void) {
    static int done = 0;
    if (done) {
        return;
    }
    done = 1;
#if defined(__GLIBC__)
    void *warm[2];
    backtrace(warm, 2);   /* the first call may load libgcc: do it now, not inside the handler */
#endif
    static char alt[65536];   /* a stack overflow must still be reportable */
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof(alt), .ss_flags = 0};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    const int fatal[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++) {
        sigaction(fatal[i], &sa, NULL);
    }
}

/* The UI thread is sampled the way vendor_diag.c samples threads on the TV: a signal to it, a handler that only
 * copies registers and stack words (and, off ARM, a backtrace) into a static snapshot, printed by the watchdog. */
static pthread_t ui_thread;
static int snap_signal;   /* 0 = unavailable: the stall lines still come, without a stack */
static struct {
    uintptr_t pc, lr, sp;
    int nwords;
    uintptr_t words[256];
    int nframes;
    void *frames[64];
    volatile sig_atomic_t done;
} snap;

static void on_snapshot(int sig, siginfo_t *si, void *uctx) {
    (void) sig;
    (void) si;
    const int saved = errno;
    const ucontext_t *uc = uctx;
    snap.pc = context_pc(uc);
    snap.sp = context_sp(uc);
    snap.nwords = 0;
    snap.nframes = 0;
#if defined(__arm__)
    snap.lr = (uintptr_t) uc->uc_mcontext.arm_lr;
    const uintptr_t *sp = (const uintptr_t *) snap.sp;
    for (int i = 0; sp != NULL && i < 256; i++) {
        snap.words[snap.nwords++] = sp[i];
    }
#elif defined(__GLIBC__)
    snap.nframes = backtrace(snap.frames, 64);
#endif
    snap.done = 1;
    errno = saved;
}

static bool watch_prepare(void) {
    ui_thread = pthread_self();
    const int sig = SIGRTMIN + 5;
    struct sigaction old;
    if (sigaction(sig, NULL, &old) == 0 && old.sa_handler == SIG_DFL) {   /* not taken by anything else */
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = on_snapshot;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        snap_signal = sigaction(sig, &sa, NULL) == 0 ? sig : 0;
    }
#if defined(__GLIBC__)
    void *warm[2];
    backtrace(warm, 2);
#endif
    return true;
}

static void dump_ui_stack(void) {
    static bool maps_done = false;
    if (snap_signal == 0) {
        out("(no stack: signal SIGRTMIN+5 is taken by something else in this process)\n");
        return;
    }
    snap.done = 0;
    if (pthread_kill(ui_thread, snap_signal) != 0) {
        out("(could not signal the UI thread)\n");
        return;
    }
    for (int i = 0; i < 200 && !snap.done; i++) {
        usleep(10 * 1000);
    }
    if (!snap.done) {
        out("(the UI thread did not answer within 2 s: stopped, or blocking the signal)\n");
        return;
    }
    out("UI thread at pc 0x%lx", (unsigned long) snap.pc);
#if defined(__arm__)
    out(", lr 0x%lx", (unsigned long) snap.lr);
#endif
    out(", sp 0x%lx, pid %d\n", (unsigned long) snap.sp, (int) getpid());
    if (snap.nwords > 0) {
        out("--- stack words ---");
        for (int i = 0; i < snap.nwords; i++) {
            out("%s0x%lx", (i & 7) == 0 ? "\n" : " ", (unsigned long) snap.words[i]);
        }
        out("\n");
    }
    out("--- backtrace ---\n");
#if defined(__GLIBC__)
    if (snap.nframes > 0) {
        backtrace_symbols_fd(snap.frames, snap.nframes, 2);
    }
#endif
    if (!maps_done) {
        maps_done = true;
        out("--- /proc/self/maps (to resolve the frames above) ---\n");
        w_maps();
    } else {
        out("(/proc/self/maps: as in this run's first UI THREAD STUCK record)\n");
    }
}

#endif

#endif

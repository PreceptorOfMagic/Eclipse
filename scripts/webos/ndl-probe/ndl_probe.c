/* ndl_probe -- replay Eclipse's NDL media-load sequence OUTSIDE Eclipse, as its own
 * throwaway webOS app (com.aurora.ndlprobe), and report what the vendor stack did.
 * Built for the webOS 26 investigation (docs/webos26-media-load.md); run it FIRST
 * after the next firmware update. Driver: run.sh in this directory.
 *
 * Why a separate app: the dev ssh shell cannot register on the luna bus
 * ("LSRegister fail"), so from ssh the SMP ResourceManager never gets a client and
 * the load dereferences NULL. An installed app gets a real app identity.
 *
 * What it gives you that Eclipse does not: every vendor media library's own log
 * (PmLog captured in-process, pmlog_tap.c -- the TV keeps no readable log), a
 * timed BEGIN/END for every NDL call, a thread dump (state, syscall, pc/lr and a
 * return-address scan resolved with dladdr) if a call blocks for 6 s, a crash
 * record on SIGSEGV/SIGBUS/SIGABRT, and a forced exit at 15 s so nothing is left
 * hung on the TV.
 *
 * usage (as an app, args read from /tmp/ndlprobe.args, log in /tmp/ndlprobe.log):
 *   <none|h264|h265> <none|pcm|opus> [flags]
 *   flags (comma separated):
 *     sdl         SDL_Init(VIDEO) + fullscreen window (NDL needs one since webOS 26)
 *     render      the main thread presents frames; the load runs on a worker thread
 *                 (Eclipse's shape: UI thread renders, connection thread loads)
 *     earlyinit   NDL_DirectMediaInit right after window creation, before any frame
 *                 is presented (Eclipse's SS4S_PostInit order); use with noinit
 *     noinit      the worker skips its own NDL_DirectMediaInit
 *     blockmain   main thread blocks (no pump, no present) while the worker loads
 *     pumponly / presentonly   main thread only pumps events / only presents
 *     backport    re-exec with Eclipse's bundled libSDL2 (lib/backports) first
 *     twostep     load video alone, unload, then load video+audio
 *     hold=N      keep the load N seconds before unloading
 *     audiopace   (with pcm) measure NDL's audio buffer queries while feeding 4 s of a
 *                 quiet tone in real time, then 1.5 s of drain; prefill=N ms ahead (40);
 *                 ptsoffset=N stamps every block N ms later than real time (feed timing
 *                 unchanged): a player that schedules audio by PTS holds N ms more
 *
 * Measured on 11.2.0 (2026-09-26), H.265 3840x2160 + PCM stereo 48 kHz:
 *   sdl (window never presented)     uMS says status[background]; Load NEVER returns
 *   sdl,render                       status[foreground]; Load rc=0 in ~100 ms
 *   ...,earlyinit,noinit             same, ~97 ms     (init order is not a factor)
 *   ...,blockmain                    same, ~89 ms     (main thread need not run)
 *   ...,backport                     same, ~96 ms     (Eclipse's SDL is not a factor)
 * i.e. the webOS 26 media stack BLOCKS a load until the app is foreground; it does
 * not fail it. Eclipse's own failure was different: see the doc above.
 */
#define _GNU_SOURCE
#include <NDL_directmedia_v2.h>
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
#include "pmlog_tap.c"   /* PmLog capture: see that file */

#define STEP_WARN_MS 6000
#define STEP_KILL_MS 15000

static long t0;
static volatile const char *g_step = "start";
static volatile long g_step_start = 0;
static volatile int g_step_active = 0;

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void step_begin(const char *name) {
    g_step = name;
    g_step_start = now_ms();
    g_step_active = 1;
    printf("[%6ld] BEGIN %s\n", now_ms() - t0, name);
    fflush(stdout);
}

static void step_end(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void step_end(const char *fmt, ...) {
    g_step_active = 0;
    printf("[%6ld] END   %s (%ld ms) ", now_ms() - t0, g_step, now_ms() - g_step_start);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* ---- register capture via SIGUSR1 ---- */
struct regs { volatile int tid; volatile uintptr_t pc, lr, sp; };
static struct regs g_regs[64];
static volatile int g_nregs = 0;

static void on_usr1(int sig, siginfo_t *si, void *ctx) {
    (void) sig; (void) si;
    ucontext_t *uc = ctx;
    int i = __sync_fetch_and_add(&g_nregs, 1);
    if (i < 64) {
        g_regs[i].pc = uc->uc_mcontext.arm_pc;
        g_regs[i].lr = uc->uc_mcontext.arm_lr;
        g_regs[i].sp = uc->uc_mcontext.arm_sp;
        g_regs[i].tid = (int) syscall(SYS_gettid);
    }
}

struct map { uintptr_t lo, hi; int x; };
static struct map g_maps[8192];
static int g_nmaps = 0;

static void load_maps(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    g_nmaps = 0;
    if (!f) return;
    char line[512];
    while (g_nmaps < 8192 && fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) == 3) {
            g_maps[g_nmaps].lo = lo;
            g_maps[g_nmaps].hi = hi;
            g_maps[g_nmaps].x = perms[2] == 'x';
            g_nmaps++;
        }
    }
    fclose(f);
}

static const struct map *find_map(uintptr_t a) {
    for (int i = 0; i < g_nmaps; i++)
        if (a >= g_maps[i].lo && a < g_maps[i].hi) return &g_maps[i];
    return NULL;
}

static void describe(const char *what, uintptr_t addr) {
    Dl_info di;
    uintptr_t a = addr & ~(uintptr_t) 1;
    if (dladdr((void *) a, &di) && di.dli_fname) {
        const char *base = strrchr(di.dli_fname, '/');
        printf("      %-4s %08lx %s+0x%lx  <%s+0x%lx>\n", what, (unsigned long) addr,
               base ? base + 1 : di.dli_fname, (unsigned long) (a - (uintptr_t) di.dli_fbase),
               di.dli_sname ? di.dli_sname : "?",
               di.dli_saddr ? (unsigned long) (a - (uintptr_t) di.dli_saddr) : 0ul);
    } else {
        printf("      %-4s %08lx ?\n", what, (unsigned long) addr);
    }
}

static void read_small(const char *path, char *buf, size_t n) {
    buf[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t r = fread(buf, 1, n - 1, f);
    buf[r] = 0;
    fclose(f);
    char *nl = strchr(buf, '\n');
    if (nl) *nl = 0;
}

static void dump_threads(void) {
    int self = (int) syscall(SYS_gettid);
    printf("---- THREAD DUMP: step '%s' blocked for %ld ms ----\n", g_step, now_ms() - g_step_start);
    load_maps();
    g_nregs = 0;
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    int tids[64], nt = 0;
    while (d && (e = readdir(d)) && nt < 64) {
        if (e->d_name[0] == '.') continue;
        tids[nt++] = atoi(e->d_name);
    }
    if (d) closedir(d);
    for (int i = 0; i < nt; i++) {
        char p[128], comm[64], wchan[128], sc[256], st[512];
        snprintf(p, sizeof p, "/proc/self/task/%d/comm", tids[i]); read_small(p, comm, sizeof comm);
        snprintf(p, sizeof p, "/proc/self/task/%d/wchan", tids[i]); read_small(p, wchan, sizeof wchan);
        snprintf(p, sizeof p, "/proc/self/task/%d/syscall", tids[i]); read_small(p, sc, sizeof sc);
        snprintf(p, sizeof p, "/proc/self/task/%d/stat", tids[i]); read_small(p, st, sizeof st);
        char *rp = strrchr(st, ')');
        printf("  tid %d%s comm=%s state=%c wchan=%s syscall=[%s]\n", tids[i], tids[i] == self ? "(watchdog)" : "",
               comm, rp && rp[1] ? rp[2] : '?', wchan, sc);
        if (tids[i] != self) syscall(SYS_tgkill, getpid(), tids[i], SIGUSR1);
    }
    usleep(300 * 1000);
    int n = g_nregs < 64 ? g_nregs : 64;
    for (int i = 0; i < n; i++) {
        char p[128], comm[64];
        snprintf(p, sizeof p, "/proc/self/task/%d/comm", g_regs[i].tid); read_small(p, comm, sizeof comm);
        printf("  == tid %d (%s)\n", g_regs[i].tid, comm);
        describe("pc", g_regs[i].pc);
        describe("lr", g_regs[i].lr);
        const struct map *sm = find_map(g_regs[i].sp);
        if (!sm) { printf("      sp %08lx unmapped\n", (unsigned long) g_regs[i].sp); continue; }
        uintptr_t end = sm->hi, sp = g_regs[i].sp;
        if (end - sp > 16384) end = sp + 16384;
        int shown = 0;
        for (uintptr_t a = sp; a + 4 <= end && shown < 28; a += 4) {
            uintptr_t w = *(volatile uintptr_t *) a;
            const struct map *m = find_map(w & ~(uintptr_t) 1);
            if (m && m->x) { describe("ret?", w); shown++; }
        }
    }
    printf("---- END THREAD DUMP ----\n");
    fflush(stdout);
}


static void on_segv(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    printf("[%6ld] SIGNAL %d in step '%s' fault_addr=%p tid=%ld\n", now_ms() - t0, sig, g_step, si->si_addr,
           (long) syscall(SYS_gettid));
    load_maps();
    describe("pc", uc->uc_mcontext.arm_pc);
    describe("lr", uc->uc_mcontext.arm_lr);
    printf("      r0=%08lx r1=%08lx r2=%08lx r3=%08lx r4=%08lx r5=%08lx\n",
           (unsigned long) uc->uc_mcontext.arm_r0, (unsigned long) uc->uc_mcontext.arm_r1,
           (unsigned long) uc->uc_mcontext.arm_r2, (unsigned long) uc->uc_mcontext.arm_r3,
           (unsigned long) uc->uc_mcontext.arm_r4, (unsigned long) uc->uc_mcontext.arm_r5);
    const struct map *sm = find_map(uc->uc_mcontext.arm_sp);
    if (sm) {
        uintptr_t end = sm->hi, sp = uc->uc_mcontext.arm_sp;
        if (end - sp > 16384) end = sp + 16384;
        int shown = 0;
        for (uintptr_t a = sp; a + 4 <= end && shown < 28; a += 4) {
            uintptr_t w = *(volatile uintptr_t *) a;
            const struct map *m = find_map(w & ~(uintptr_t) 1);
            if (m && m->x) { describe("ret?", w); shown++; }
        }
    }
    fflush(stdout);
    _exit(11);
}

static void *watchdog(void *arg) {
    (void) arg;
    int dumped = 0;
    for (;;) {
        usleep(200 * 1000);
        if (!g_step_active) { dumped = 0; continue; }
        long el = now_ms() - g_step_start;
        if (el > STEP_WARN_MS && !dumped) { dump_threads(); dumped = 1; }
        if (el > STEP_KILL_MS) {
            printf("[%6ld] HUNG: step '%s' never returned (%ld ms) -- exiting\n", now_ms() - t0, g_step, el);
            dump_threads();
            fflush(stdout);
            _exit(9);
        }
    }
    return NULL;
}

static void load_cb(int type, long long num, const char *str) {
    printf("[%6ld]   cb type=0x%02x num=%lld str=%s\n", now_ms() - t0, type, num, str ? str : "(null)");
    fflush(stdout);
}

static int has_flag(const char *flags, const char *f) {
    return flags && strstr(flags, f) != NULL;
}

static const char *g_args[3];
static volatile int g_worker_done = 0;
struct render_api {
    void *win, *ren;
    int present;
    void *(*create_renderer)(void *, int, uint32_t);
    int (*set_color)(void *, uint8_t, uint8_t, uint8_t, uint8_t);
    int (*clear)(void *);
    void (*present_fn)(void *);
    int (*poll)(void *);
    void (*delay)(uint32_t);
};
static struct render_api g_render;

static void pump_and_draw(unsigned frame) {
    unsigned char ev[64];
    while (g_render.poll(ev)) {}
    uint8_t v = (uint8_t) (40 + (frame % 60));
    g_render.set_color(g_render.ren, v, 20, 20, 255);
    g_render.clear(g_render.ren);
    g_render.present_fn(g_render.ren);
    g_render.delay(16);
}

/* audiopace: can NDL's own buffer queries pace a pull mixer on this TV? The SDK
 * declares NDL_DirectAudioGetAvailableBufferSize/GetTotalBufferSize with no units
 * or update rate, so measure them. Phase 1 feeds a -40 dBFS 440 Hz triangle in 5 ms
 * blocks by OUR clock (real time, prefill=N ms ahead, default 40) while polling every
 * 1 ms; phase 2 stops feeding and watches the drain. Only changes of the queried value
 * are kept, printed at the end as "ev t_us avail fed_bytes". PTS = ms since load,
 * as ss4s's NDL module passes it. */
typedef int (*ndl_size_fn)(int *);
#define PACE_FRAMES 240                     /* 5 ms at 48 kHz */
#define PACE_BYTES (PACE_FRAMES * 4)        /* stereo S16LE */
#define PACE_MAX_EVENTS 20000
static long long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}
static void audio_pace(const char *flags, long long load_us) {
    ndl_size_fn get_avail = (ndl_size_fn) dlsym(RTLD_DEFAULT, "NDL_DirectAudioGetAvailableBufferSize");
    ndl_size_fn get_total = (ndl_size_fn) dlsym(RTLD_DEFAULT, "NDL_DirectAudioGetTotalBufferSize");
    printf("[%6ld] audiopace: GetAvailableBufferSize=%p GetTotalBufferSize=%p\n",
           now_ms() - t0, (void *) get_avail, (void *) get_total);
    if (get_avail == NULL) return;
    int total = -1, avail = -1;
    const int trc = get_total ? get_total(&total) : -2;
    const int arc = get_avail(&avail);
    printf("[%6ld] audiopace: total rc=%d value=%d, available rc=%d value=%d before feeding\n",
           now_ms() - t0, trc, total, arc, avail);
    const char *p = strstr(flags, "prefill=");
    const long long prefill_us = (p ? atoi(p + 8) : 40) * 1000LL;
    const char *po = strstr(flags, "ptsoffset=");
    const long long pts_offset_ms = po ? atoll(po + 10) : 0;
    printf("[%6ld] audiopace: prefill %lld ms, PTS offset %lld ms\n", now_ms() - t0, prefill_us / 1000, pts_offset_ms);
    const long long feed_us = 4000000LL, drain_us = 1500000LL;
    static int16_t block[PACE_FRAMES * 2];
    static struct { long long t; int avail; long long fed; } ev[PACE_MAX_EVENTS];
    int nev = 0, last = avail, play_errors = 0, read_errors = 0;
    long long fed = 0, frames = 0;
    unsigned phase = 0;
    const long long start = now_us();
    step_begin("audiopace feed+drain");
    for (;;) {
        const long long now = now_us(), el = now - start;
        if (el > feed_us + drain_us) break;
        while (el < feed_us && frames * 1000000LL / 48000 < el + prefill_us) {
            for (int i = 0; i < PACE_FRAMES; ++i) {
                const long q = (long) phase * 4;   /* triangle, period 48000/440 samples */
                const long v = q < 48000 ? q : q < 144000 ? 96000 - q : q - 192000;
                block[2 * i] = block[2 * i + 1] = (int16_t) (328 * v / 48000);
                phase = (phase + 440) % 48000;
            }
            if (NDL_DirectAudioPlay(block, PACE_BYTES, (now - load_us) / 1000 + pts_offset_ms) != 0) play_errors++;
            fed += PACE_BYTES;
            frames += PACE_FRAMES;
        }
        int a = -1;
        if (get_avail(&a) != 0) read_errors++;
        else if (a != last && nev < PACE_MAX_EVENTS) {
            ev[nev].t = el; ev[nev].avail = a; ev[nev].fed = fed; nev++;
            last = a;
        }
        usleep(1000);
    }
    step_end("events=%d fed_bytes=%lld play_errors=%d read_errors=%d", nev, fed, play_errors, read_errors);
    printf("audiopace-events t_us avail fed_bytes\n");
    for (int i = 0; i < nev; ++i) printf("ev %lld %d %lld\n", ev[i].t, ev[i].avail, ev[i].fed);
    int after = -1;
    if (get_total) get_total(&after);
    printf("[%6ld] audiopace: total after %d\n", now_ms() - t0, after);
    fflush(stdout);
}

static int ndl_sequence(void) {
    const char *flags = g_args[2];
    const char *appid = getenv("APPID") ? getenv("APPID") : "com.aurora.gamestream";
    int rc;
    if (!has_flag(flags, "noinit")) {
    step_begin("NDL_DirectMediaInit");
    rc = NDL_DirectMediaInit(appid, NULL);
    step_end("rc=%d err=%s", rc, rc ? NDL_DirectMediaGetError() : "-");
    if (rc) return 4;
    }

    NDL_DIRECTMEDIA_DATA_INFO_T info;
    memset(&info, 0, sizeof info);
    if (!strcmp(g_args[0], "h264")) info.video.type = NDL_VIDEO_TYPE_H264;
    if (!strcmp(g_args[0], "h265")) info.video.type = NDL_VIDEO_TYPE_H265;
    if (info.video.type) { info.video.width = 3840; info.video.height = 2160; }
    if (!strcmp(g_args[1], "pcm")) {
        info.audio.pcm.type = NDL_AUDIO_TYPE_PCM;
        info.audio.pcm.format = NDL_DIRECTMEDIA_AUDIO_PCM_FORMAT_S16LE;
        info.audio.pcm.channelMode = "stereo";
        info.audio.pcm.sampleRate = NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_OF(48000);
    } else if (!strcmp(g_args[1], "opus")) {
        info.audio.opus.type = NDL_AUDIO_TYPE_OPUS;
        info.audio.opus.channels = 2;
        info.audio.opus.sampleRate = 48.0;
    }

    if (has_flag(flags, "twostep") && info.video.type && info.audio.type) {
        NDL_DIRECTMEDIA_DATA_INFO_T vonly = info;
        memset(&vonly.audio, 0, sizeof vonly.audio);
        step_begin("NDL_DirectMediaLoad(video only)");
        rc = NDL_DirectMediaLoad(&vonly, load_cb);
        step_end("rc=%d err=%s", rc, rc ? NDL_DirectMediaGetError() : "-");
        step_begin("NDL_DirectMediaUnload(video only)");
        rc = NDL_DirectMediaUnload();
        step_end("rc=%d", rc);
    }

    step_begin("NDL_DirectMediaLoad");
    rc = NDL_DirectMediaLoad(&info, load_cb);
    step_end("rc=%d err=%s", rc, rc ? NDL_DirectMediaGetError() : "-");
    const long long load_us = now_us();

    if (rc == 0 && info.audio.type == NDL_AUDIO_TYPE_PCM && has_flag(flags, "audiopace")) {
        audio_pace(flags, load_us);
    } else if (rc == 0 && info.audio.type == NDL_AUDIO_TYPE_PCM) {
        unsigned short silence[2 * 480];
        memset(silence, 0, sizeof silence);
        step_begin("NDL_DirectAudioPlay(10ms silence)");
        int prc = NDL_DirectAudioPlay(silence, sizeof silence, 0);
        step_end("rc=%d", prc);
    }
    const char *hold = strstr(flags, "hold=");
    if (hold && rc == 0) sleep(atoi(hold + 5));

    if (rc == 0) {
        step_begin("NDL_DirectMediaUnload");
        rc = NDL_DirectMediaUnload();
        step_end("rc=%d", rc);
    }
    step_begin("NDL_DirectMediaQuit");
    rc = NDL_DirectMediaQuit();
    step_end("rc=%d", rc);
    return 0;
}

static void *ndl_worker(void *arg) {
    (void) arg;
    usleep(1500 * 1000);
    ndl_sequence();
    g_worker_done = 1;
    return NULL;
}


int main(int argc, char **argv) {
    static char *av[8];
    static char argbuf[256];
    if (argc < 3 || argv[1][0] == '{') {
        /* launched as an app: args from a file, output to a log file */
        FILE *log = freopen("/tmp/ndlprobe.log", "w", stdout);
        (void) log;
        dup2(fileno(stdout), 2);
        FILE *af = fopen("/tmp/ndlprobe.args", "r");
        int n = 1;
        av[0] = argv[0];
        if (af && fgets(argbuf, sizeof argbuf, af)) {
            char *save = NULL;
            for (char *tok = strtok_r(argbuf, " \t\n", &save); tok && n < 7; tok = strtok_r(NULL, " \t\n", &save))
                av[n++] = tok;
        }
        if (af) fclose(af);
        if (n < 3) { av[1] = "h265"; av[2] = "pcm"; av[3] = "sdl"; n = 4; }
        printf("app-mode launch argv1=%s\n", argc > 1 ? argv[1] : "(none)");
        argc = n;
        argv = av;
    }
    if (argc > 3 && has_flag(argv[3], "backport") && getenv("PROBE_REEXEC") == NULL) {
        const char *bp = "/media/developer/apps/usr/palm/applications/com.aurora.gamestream/lib/backports";
        char ld[1024];
        snprintf(ld, sizeof ld, "%s:%s", bp, getenv("LD_LIBRARY_PATH") ? getenv("LD_LIBRARY_PATH") : "");
        setenv("LD_LIBRARY_PATH", ld, 1);
        setenv("PROBE_REEXEC", "1", 1);
        fflush(stdout);
        char *nargv[5] = {argv[0], argv[1], argv[2], argv[3], NULL};
        execv("/proc/self/exe", nargv);
        printf("re-exec failed\n");
    }
    if (getenv("PROBE_REEXEC")) {
        /* stdout was the log file before exec; reopen in append mode */
        freopen("/tmp/ndlprobe.log", "a", stdout);
        dup2(fileno(stdout), 2);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    t0 = now_ms();
    if (argc < 3) {
        fprintf(stderr, "usage: %s <none|h264|h265> <none|pcm|opus> [twostep,hold=N,sdl]\n", argv[0]);
        return 2;
    }
    const char *flags = argc > 3 ? argv[3] : "";
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_usr1;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
    struct sigaction sb;
    memset(&sb, 0, sizeof sb);
    sb.sa_sigaction = on_segv;
    sb.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sb, NULL);
    sigaction(SIGBUS, &sb, NULL);
    sigaction(SIGABRT, &sb, NULL);
    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, NULL);

    const char *appid = getenv("APPID") ? getenv("APPID") : "com.aurora.gamestream";
    printf("probe pid=%d appid=%s video=%s audio=%s flags=%s\n", getpid(), appid, argv[1], argv[2], flags);

    if (has_flag(flags, "sdl")) {
        step_begin("SDL_Init+CreateWindow");
        void *h = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
        if (!h) { step_end("dlopen failed: %s", dlerror()); return 3; }
        int (*init)(uint32_t) = dlsym(h, "SDL_Init");
        void *(*create)(const char *, int, int, int, int, uint32_t) = dlsym(h, "SDL_CreateWindow");
        const char *(*geterr)(void) = dlsym(h, "SDL_GetError");
        {
            Dl_info di;
            void (*getver)(unsigned char *) = dlsym(h, "SDL_GetVersion");
            unsigned char v[3] = {0, 0, 0};
            if (getver) getver(v);
            if (dladdr((void *) init, &di)) printf("SDL loaded from %s version %u.%u.%u\n", di.dli_fname, v[0], v[1], v[2]);
        }
        int r = init(0x20u /* SDL_INIT_VIDEO */);
        void *w = r == 0 ? create("ndl-probe", 0, 0, 1920, 1080, 0x1u /* FULLSCREEN */ | 0x2u /* OPENGL */) : NULL;
        step_end("init=%d window=%p err=%s", r, w, geterr());
        if (w && has_flag(flags, "render")) {
            g_render.win = w;
            g_render.create_renderer = dlsym(h, "SDL_CreateRenderer");
            g_render.set_color = dlsym(h, "SDL_SetRenderDrawColor");
            g_render.clear = dlsym(h, "SDL_RenderClear");
            g_render.present_fn = dlsym(h, "SDL_RenderPresent");
            g_render.poll = dlsym(h, "SDL_PollEvent");
            g_render.delay = dlsym(h, "SDL_Delay");
            g_render.ren = g_render.create_renderer(w, -1, 0x2u /* ACCELERATED */ | 0x4u /* PRESENTVSYNC */);
            printf("[%6ld] renderer=%p\n", now_ms() - t0, g_render.ren);
            if (g_render.ren && has_flag(flags, "earlyinit")) {
                const char *aid = getenv("APPID") ? getenv("APPID") : "com.aurora.gamestream";
                step_begin("NDL_DirectMediaInit(early, before first present)");
                int irc = NDL_DirectMediaInit(aid, NULL);
                step_end("rc=%d", irc);
            }
            if (g_render.ren) {
                g_render.present = 1;
                for (unsigned i = 0; i < 30; i++) pump_and_draw(i);
                printf("[%6ld] pre-rendered 30 frames\n", now_ms() - t0);
            }
        }
    }

    if (has_flag(flags, "render") && g_render.present) {
        g_args[0] = argv[1]; g_args[1] = argv[2]; g_args[2] = flags;
        pthread_t worker;
        pthread_create(&worker, NULL, ndl_worker, NULL);
        long until = now_ms() + 40000;
        unsigned frames = 0;
        if (has_flag(flags, "blockmain")) {
            printf("[%6ld] main: BLOCKING (no pump, no present) until the worker returns\n", now_ms() - t0);
            pthread_join(worker, NULL);
        } else if (has_flag(flags, "pumponly")) {
            unsigned char ev[64];
            while (!g_worker_done && now_ms() < until) { while (g_render.poll(ev)) {} g_render.delay(16); frames++; }
        } else if (has_flag(flags, "presentonly")) {
            while (!g_worker_done && now_ms() < until) {
                g_render.set_color(g_render.ren, 60, 20, 20, 255); g_render.clear(g_render.ren);
                g_render.present_fn(g_render.ren); g_render.delay(16); frames++;
            }
        } else {
            while (!g_worker_done && now_ms() < until) {
                pump_and_draw(frames++);
            }
        }
        printf("[%6ld] main: rendered %u frames, worker_done=%d\n", now_ms() - t0, frames, g_worker_done);
    } else {
        g_args[0] = argv[1]; g_args[1] = argv[2]; g_args[2] = flags;
        ndl_sequence();
    }
    printf("probe done\n");
    return 0;
}

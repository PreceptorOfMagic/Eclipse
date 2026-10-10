/* PmLog tap: give the vendor media libraries a debug-enabled context and print
 * everything they log.  Everyone else keeps the real PmLog. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct { int enabledLevel; int flags; } Ctx;
typedef const Ctx *PmLogContext;
struct fake { Ctx info; char name[64]; };
static struct fake g_fakes[64];
static int g_nfakes;
static int tap_all = 0;

static long tap_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}


static void *pm_real(const char *sym) {
    void *f = dlsym(RTLD_NEXT, sym);
    if (!f) {
        static void *h;
        if (!h) h = dlopen("libPmLogLib.so.3", RTLD_NOW | RTLD_GLOBAL);
        if (h) f = dlsym(h, sym);
    }
    return f;
}

static int is_fake(PmLogContext c) {
    return c >= &g_fakes[0].info && c < &g_fakes[64].info;
}

static int media_caller(void *ra) {
    Dl_info di;
    if (!dladdr(ra, &di) || !di.dli_fname) return 0;
    if (tap_all) return 1;
    return strstr(di.dli_fname, "NDL") || strstr(di.dli_fname, "playerAPIs");
}

int PmLogGetContext(const char *name, PmLogContext *pctx) {
    static int (*real)(const char *, PmLogContext *);
    if (!real) real = (int (*)(const char *, PmLogContext *)) pm_real("PmLogGetContext");
    int rc = real(name, pctx);
    if (media_caller(__builtin_return_address(0)) && g_nfakes < 64) {
        for (int i = 0; i < g_nfakes; i++)
            if (!strcmp(g_fakes[i].name, name ? name : "")) { *pctx = &g_fakes[i].info; return 0; }
        struct fake *f = &g_fakes[g_nfakes++];
        f->info.enabledLevel = 7;
        snprintf(f->name, sizeof f->name, "%s", name ? name : "");
        *pctx = &f->info;
        rc = 0;
    }
    return rc;
}

int PmLogSetContextLevel(PmLogContext c, int level) {
    static int (*real)(PmLogContext, int);
    if (is_fake(c)) return 0;
    if (!real) real = (int (*)(PmLogContext, int)) pm_real("PmLogSetContextLevel");
    return real(c, level);
}

int PmLogString_(PmLogContext c, int level, const char *msgid, const char *kv, const char *msg) {
    static int (*real)(PmLogContext, int, const char *, const char *, const char *);
    if (is_fake(c)) {
        printf("PMLOG %ld %s L%d [%s] %s %s\n", tap_ms(), ((const struct fake *) c)->name, level,
               msgid ? msgid : "-", kv ? kv : "", msg ? msg : "");
        fflush(stdout);
        return 0;
    }
    if (!real) real = (int (*)(PmLogContext, int, const char *, const char *, const char *)) pm_real("PmLogString_");
    return real(c, level, msgid, kv, msg);
}

int _PmLogMsgKV(PmLogContext c, int level, unsigned flags, const char *msgid, size_t kv_count,
                const char *ck, const char *cf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (is_fake(c)) {
        char buf[1024];
        vsnprintf(buf, sizeof buf, fmt ? fmt : "", ap);
        va_end(ap);
        printf("PMLOG %ld %s L%d [%s] %s\n", tap_ms(), ((const struct fake *) c)->name, level, msgid ? msgid : "-", buf);
        fflush(stdout);
        return 0;
    }
    /* not ours: forward, formatting to a plain string (kv already rendered by fmt) */
    static int (*real)(PmLogContext, int, unsigned, const char *, size_t, const char *, const char *, const char *, ...);
    if (!real) real = (int (*)(PmLogContext, int, unsigned, const char *, size_t, const char *, const char *, const char *, ...)) pm_real("_PmLogMsgKV");
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt ? fmt : "", ap);
    va_end(ap);
    (void) kv_count; (void) ck; (void) cf;
    return real(c, level, flags, msgid, 0, "", "", "%s", buf);
}

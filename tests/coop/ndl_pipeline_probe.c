/* ndl_pipeline_probe — is an NDL media pipeline per-PROCESS or per-APPID?
 *
 * THE co-op split-screen existential gate. The design needs two processes to
 * each hold their own decode pipeline (two streams, one TV). An earlier probe
 * tried this with two SS4S players inside ONE process and appeared to succeed,
 * but that was an artifact: ss4s ndl/webos5 keeps a single global
 * ActivatePlayerContext and calls the handle-less, process-global
 * NDL_DirectMediaLoad(), so the "second decoder" was just the one pipeline
 * being re-loaded. This probe answers the real question from separate
 * processes, and needs no Aurora stream at all.
 *
 * NDL_DirectMediaInit() takes the appId, so the interesting axis is whether
 * two processes conflict when they share an appId versus use different ones.
 * Pass the appId via the APPID env var (that is what ss4s itself reads).
 *
 * Usage:  ndl_pipeline_probe [hold_seconds]
 *   hold_seconds > 0 : hold the pipeline open (the "first process")
 *   hold_seconds = 0 : load, report, release immediately (the "second process")
 *
 * Exit: 0 = pipeline granted, 1 = denied, 2 = init failed (test is invalid,
 * not a denial — e.g. policy refused a non-foreground app entirely).
 *
 * Build (webOS SDK):
 *   $SDK/bin/arm-webos-linux-gnueabi-gcc -O1 -o ndl_pipeline_probe \
 *       tests/coop/ndl_pipeline_probe.c -lndl-directmedia
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <NDL_directmedia_v2.h>

/* Mirror everything to a file as well as stdout: when this runs as an installed
 * webOS app (the only way to get an LS2 role — a bare ssh process dies in
 * NDL_DirectMediaInit with "LSRegisterPubPriv FAILED"), stdout goes nowhere we
 * can read. */
static FILE *logf = NULL;

static void plog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (logf != NULL) {
        va_start(ap, fmt);
        vfprintf(logf, fmt, ap);
        va_end(ap);
        fflush(logf);
    }
}

static void LoadCallback(int type, long long numValue, const char *strValue) {
    plog("  [cb] type=%d num=%lld str=%s\n", type, numValue, strValue ? strValue : "(null)");
}

int main(int argc, char *argv[]) {
    /* Aurora's child inherits APPID=com.aurora.gamestream, and registering that
     * same LS2 service name a second time fails (LSRegisterPubPriv FAILED) because
     * the parent already holds it. /tmp/coop-probe-appid overrides the id so the
     * child can try registering as a DIFFERENT, installed app instead — the last
     * variant that could make a two-process split viable. */
    static char appid_buf[128];
    const char *appid = getenv("APPID");
    FILE *af = fopen("/tmp/coop-probe-appid", "r");
    if (af != NULL) {
        if (fgets(appid_buf, sizeof(appid_buf), af) != NULL) {
            appid_buf[strcspn(appid_buf, "\r\n")] = '\0';
            if (appid_buf[0] != '\0') { appid = appid_buf; }
        }
        fclose(af);
    }
    if (appid == NULL || appid[0] == '\0') {
        appid = "com.aurora.gamestream";
    }
    int hold = argc > 1 ? atoi(argv[1]) : 0;
    /* argv[2] overrides the log path. Each webOS app runs as its OWN uid and
     * /tmp is sticky, so a log created by one app cannot be appended to (or
     * removed) by another — a fixed path silently swallowed a whole run. */
    const char *logpath = argc > 2 ? argv[2] : "/tmp/coop_ndl_probe.log";
    logf = fopen(logpath, "a");

    plog("[probe] pid=%d appid=%s hold=%ds\n", (int) getpid(), appid, hold);

    int ret = NDL_DirectMediaInit(appid, NULL);
    plog("[probe] NDL_DirectMediaInit -> %d%s\n", ret,
           ret == 0 ? " (ok)" : "");
    if (ret != 0) {
        const char *err = NDL_DirectMediaGetError();
        plog("[probe] INIT FAILED: %s\n", err ? err : "(no error string)");
        plog("[probe] RESULT=INIT_FAILED (inconclusive — not a pipeline denial)\n");
        return 2;
    }

    NDL_DIRECTMEDIA_DATA_INFO_T info;
    memset(&info, 0, sizeof(info));
    info.video.width = 1280;
    info.video.height = 720;
    info.video.type = NDL_VIDEO_TYPE_H264;
    info.video.unknown1 = 0;

    ret = NDL_DirectMediaLoad(&info, LoadCallback);
    if (ret == 0) {
        plog("[probe] RESULT=GRANTED (NDL_DirectMediaLoad -> 0)\n");
    } else {
        const char *err = NDL_DirectMediaGetError();
        plog("[probe] RESULT=DENIED (NDL_DirectMediaLoad -> %d, %s)\n", ret,
               err ? err : "(no error string)");
    }
    fflush(stdout);

    if (ret == 0 && hold > 0) {
        plog("[probe] holding pipeline for %ds...\n", hold);
        fflush(stdout);
        sleep(hold);
    }

    if (ret == 0) {
        NDL_DirectMediaUnload();
    }
    NDL_DirectMediaQuit();
    plog("[probe] done\n");
    return ret == 0 ? 0 : 1;
}

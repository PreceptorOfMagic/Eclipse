#include <signal.h>
#include "app.h"
#include "app_webos.h"
#include "app_launch.h"
#include "stream/video/coop_secondary.h"

#include <pbnjson.h>

#include "logging.h"
#include "util/path.h"
#include "util/i18n.h"

#include "lunasynccall.h"

#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static char locale_system[16];

void app_open_url(const char *url) {
    SDL_OpenURL(url);
}

void app_init_locale() {
    if (app_configuration->language[0] && strcmp(app_configuration->language, "auto") != 0) {
        commons_log_debug("APP", "Override language to %s", app_configuration->language);
        i18n_setlocale(app_configuration->language);
        return;
    }
    SDL_Locale *locales = SDL_GetPreferredLocales();
    if (locales) {
        for (int i = 0; locales[i].language; i++) {
            if (locales[i].country) {
                snprintf(locale_system, sizeof(locale_system), "%s-%s", locales[i].language, locales[i].country);
            } else {
                strncpy(locale_system, locales[i].language, sizeof(locale_system));
            }
            locale_system[sizeof(locale_system) - 1] = '\0';
            const i18n_entry_t *entry = i18n_entry(locale_system);
            if (entry == NULL || strcmp(entry->locale, "auto") == 0) {
                continue;
            }
            i18n_setlocale(entry->locale);
            if (i18n_is_loaded()) {
                break;
            }
        }
        SDL_free(locales);
    }
}

#if defined(AURORA_TEST_HOOKS)
/* Co-op investigation: what does the TV's NATIVE Multi-View expose to us?
 *
 * webOS ships com.webos.service.multiviewcontroller, whose api-permissions file
 * lists all 54 methods under "public" — so it is callable from an ordinary app,
 * unlike the media pipeline we could not get a second of. If it will host Eclipse
 * as a Multi-View sub-app, the SYSTEM does the split and the surface/decoder
 * arbitration that a homebrew app cannot do for itself (see the four failed
 * pipeline variants in the co-op memory).
 *
 * Read-only queries only: support option, current status, and the app lists.
 * getChangeableAppList / getMultiviewAppList are the decisive ones — they say
 * WHICH apps the TV will accept in a pane, and whether a homebrew app is in it.
 *
 * Gated on /tmp/coop-probe-mv, runs at app start (no stream required), writes to
 * /tmp/coop_multiview.log.
 */
/* Run the script once. A leading "@repeat N ms" line makes the caller loop, so a
 * LIVE multiview session can be observed: launching Eclipse steals foreground and
 * closes multiview, so a one-shot probe can never see one. Polling from the
 * background lets the user open Multi-View while Eclipse watches. */
static int coop_probe_pass(long *repeat_out, long *interval_out);

static void *coop_probe_thread(void *arg) {
    (void) arg;
    long repeat = 0, interval = 2000;
    coop_probe_pass(&repeat, &interval);
    for (long i = 1; i < repeat; ++i) {
        struct timespec ts = {interval / 1000, (interval % 1000) * 1000000L};
        nanosleep(&ts, NULL);
        coop_probe_pass(NULL, NULL);
    }
    return NULL;
}

static void coop_multiview_probe(void) {
    /* Both processes handle launch, but capture is a display-wide operation.
     * A second caller races the same output file and doubles capture work. */
    if (coop_secondary_mode() != NULL || access("/tmp/coop-probe-mv", F_OK) != 0) {
        return;
    }
    /* Detached: never block app startup (a blocking probe would stop Eclipse coming up). */
    pthread_t t;
    if (pthread_create(&t, NULL, coop_probe_thread, NULL) == 0) {
        pthread_detach(t);
    }
}

static int coop_probe_pass(long *repeat_out, long *interval_out) {
    FILE *log = fopen(AURORA_MULTIVIEW_LOG, "a");
    if (log == NULL) {
        return 0;
    }
    fprintf(log, "=== multiview probe run=%ld ===\n", (long) time(NULL));

    /* If the sentinel has content, treat each line as "uri|payload" and call it.
     * That turns this into a rebuild-free luna console: queries can be iterated by
     * writing the file over SSH instead of recompiling and redeploying the app.
     * An empty sentinel runs the default multiview query set. */
    FILE *script = fopen("/tmp/coop-probe-mv", "r");
    int scripted = 0;
    if (script != NULL) {
        char line[512];
        while (fgets(line, sizeof(line), script) != NULL) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '\0' || line[0] == '#') {
                continue;
            }
            if (strncmp(line, "@repeat ", 8) == 0) {
                long r = 0, iv = 2000;
                if (sscanf(line + 8, "%ld %ld", &r, &iv) >= 1) {
                    if (repeat_out) *repeat_out = r;
                    if (interval_out) *interval_out = iv > 0 ? iv : 2000;
                }
                continue;
            }
            char *bar = strchr(line, '|');
            const char *payload = "{}";
            if (bar != NULL) {
                *bar = '\0';
                payload = bar + 1;
            }
            char *out = NULL;
            struct timespec wall_before, wall_after, mono_before, mono_after;
            clock_gettime(CLOCK_REALTIME, &wall_before);
            clock_gettime(CLOCK_MONOTONIC, &mono_before);
            bool ok = HLunaServiceCallSync(line, payload, true, &out);
            clock_gettime(CLOCK_MONOTONIC, &mono_after);
            clock_gettime(CLOCK_REALTIME, &wall_after);
            fprintf(log, "[%s] payload=%s ok=%d reply=%s\n", line, payload, (int) ok,
                    out ? out : "(null)");
            /* Bounds on this service call, not an assertion of physical scanout.
             * Used with a host-rendered clock to measure age of captured pixels. */
            fprintf(log, "[PROBE-TIME] pid=%d wall_ms=%lld,%lld mono_ms=%lld,%lld\n",
                    (int) getpid(),
                    (long long) wall_before.tv_sec * 1000 + wall_before.tv_nsec / 1000000,
                    (long long) wall_after.tv_sec * 1000 + wall_after.tv_nsec / 1000000,
                    (long long) mono_before.tv_sec * 1000 + mono_before.tv_nsec / 1000000,
                    (long long) mono_after.tv_sec * 1000 + mono_after.tv_nsec / 1000000);
            fflush(log);
            if (out != NULL) {
                free(out);
            }
            scripted++;
        }
        fclose(script);
    }

    if (scripted == 0) {
        static const char *const methods[] = {
                "getMultiviewSupportOption",
                "getStatus",
                "getMultiviewStatus",
                "getMultiviewAppList",
                "getChangeableAppList",
                "getMainAppList",
                "getSubAppList",
                "getAppList",
        };
        for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
            char uri[192];
            snprintf(uri, sizeof(uri), "luna://com.webos.service.multiviewcontroller/%s", methods[i]);
            char *out = NULL;
            bool ok = HLunaServiceCallSync(uri, "{}", true, &out);
            fprintf(log, "[%s] ok=%d reply=%s\n", methods[i], (int) ok, out ? out : "(null)");
            fflush(log);
            if (out != NULL) {
                free(out);
            }
        }
    }
    fprintf(log, "=== multiview probe end (%d scripted calls) ===\n", scripted);
    fclose(log);
    return scripted;
}
#endif

app_launch_params_t *app_handle_launch(app_t *app, int argc, char *argv[]) {
    (void) app;
#if defined(AURORA_TEST_HOOKS)
    coop_multiview_probe();   /* diagnostic builds only: executes a developer's Luna script */
#endif

    /* Keep launch boundaries and process roles in the same app-owned retained
     * logs as other diagnostics, without creating a shared temporary file. */
    bool secondary = false;
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] != NULL && strcmp(argv[i], "--coop-secondary") == 0) {
            secondary = true;
            break;
        }
    }
    char stamp[32];
    const time_t now = time(NULL);
    struct tm tm_buf;
    if (localtime_r(&now, &tm_buf) == NULL ||
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_buf) == 0) {
        snprintf(stamp, sizeof(stamp), "%ld", (long) now);
    }
#if defined(AURORA_TEST_HOOKS)
    /* Preserve the diagnostic harness's exact legacy run delimiter. */
    FILE *df = fopen(AURORA_DIAG_LOG, "a");
    if (df != NULL) {
        fprintf(df, "\n=== RUN START %s pid=%d role=%s (tick 0 is this instant) ===\n",
                stamp, (int) getpid(), secondary ? "COOP-SECONDARY" : "primary");
        fclose(df);
    }
#else
    commons_log_info("APP", "Run start %s pid=%d role=%s (tick 0 is this instant)",
                     stamp, (int) getpid(), secondary ? "COOP-SECONDARY" : "primary");
#endif
    /* The normal diagnostic logger is initialized before launch parsing and,
     * when enabled, captured in the selected app profile's retained logs. */
    commons_log_info("APP", "Launch argc=%d argv1=%s", argc,
                     argc > 1 && argv[1] ? argv[1] : "(none)");
    if (argc < 2) {
        return NULL;
    }
    JSchemaInfo schema_info;
    jschema_info_init(&schema_info, jschema_all(), NULL, NULL);
    jdomparser_ref parser = jdomparser_create(&schema_info, 0);
    if (!jdomparser_feed(parser, argv[1], (int) strlen(argv[1]))) {
        jdomparser_release(&parser);
        return NULL;
    }
    if (!jdomparser_end(parser)) {
        jdomparser_release(&parser);
        return NULL;
    }
    jvalue_ref params_obj = jdomparser_get_result(parser);
    if (!jis_valid(params_obj)) {
        jdomparser_release(&parser);
        return NULL;
    }
    app_launch_params_t *params = calloc(1, sizeof(app_launch_params_t));
    jvalue_ref host_uuid = jobject_get(params_obj, J_CSTR_TO_BUF("host_uuid"));
    if (jis_string(host_uuid)) {
        raw_buffer buf = jstring_get(host_uuid);
        uuidstr_fromchars(&params->default_host_uuid, buf.m_len, buf.m_str);
    }
    jvalue_ref host_app_id = jobject_get(params_obj, J_CSTR_TO_BUF("host_app_id"));
    if (jis_number(host_app_id)) {
        if (jnumber_get_i32(host_app_id, &params->default_app_id) != CONV_OK) {
            params->default_app_id = 0;
        }
    } else if (jis_string(host_app_id)) {
        raw_buffer buf = jstring_get(host_app_id);
        char *id_str = strndup(buf.m_str, buf.m_len);
        params->default_app_id = strtol(id_str, NULL, 10);
        if (params->default_app_id < 0) {
            params->default_app_id = 0;
        }
        free(id_str);
    }
    /* PER-LAUNCH CO-OP OPT-IN.
     *
     * Co-op used to activate purely on the presence of /tmp/coop-live and
     * /tmp/coop-spawn. Those files outlive the run that armed them, so after any co-op
     * test the NEXT ORDINARY STREAM silently became a co-op stream -- a secondary was
     * spawned and the picture was composed as a half-pane, with nothing in the UI to
     * explain it. That is a real regression a user hit ("the game stream moved out of
     * position").
     *
     * Consuming the sentinels in-app cannot fix it: the app runs as uid 6584 while the
     * sentinels are created over ssh by `prisoner` (6122) and /tmp is sticky, so
     * unlink() is refused. Verified live -- the files survived a session on a build
     * that tried. Activation therefore cannot be stateful at all.
     *
     * So co-op is now armed BY THE LAUNCH, which is inherently per-run and needs no
     * cleanup: `ares-launch -p '{"host_uuid":..,"host_app_id":N,"coop":true}'`. The
     * sentinels remain as CONFIGURATION (slice count, conceal, the peer to spawn) but
     * are only consulted when this flag says the launch wanted co-op. */
    jvalue_ref coop = jobject_get(params_obj, J_CSTR_TO_BUF("coop"));
    if (jis_boolean(coop)) {
        bool on = false;
        if (jboolean_get(coop, &on) == CONV_OK) {
            params->coop_requested = on;
        }
    } else if (jis_number(coop)) {
        int v = 0;
        params->coop_requested = (jnumber_get_i32(coop, &v) == CONV_OK && v != 0);
    } else if (jis_string(coop)) {
        raw_buffer buf = jstring_get(coop);
        params->coop_requested = (buf.m_len == 4 && strncasecmp(buf.m_str, "true", 4) == 0);
    }
    app_launch_set_coop_requested(params->coop_requested);
#if defined(AURORA_TEST_HOOKS)
    /* TEST ONLY, BETA BUILDS ONLY (AURORA_TEST_HOOKS): crash on purpose, so crash capture and log recovery can be
     * tested on a TV -- the dev shell runs as another user and cannot signal the app. The live app id never has it.
     * `lgtv.py launch com.aurora.gamestream.beta '{"aurora_test_crash":true}'` */
    jvalue_ref test_crash = jobject_get(params_obj, J_CSTR_TO_BUF("aurora_test_crash"));
    bool crash_now = false;
    if (jis_boolean(test_crash) && jboolean_get(test_crash, &crash_now) == CONV_OK && crash_now) {
        commons_log_warn("APP", "Test crash requested by the launch parameters (beta test hook): crashing now");
        raise(SIGSEGV);
    }
    /* ...and stall the UI thread N seconds on its next main-loop pass, for the frozen-UI record:
     * '{"aurora_test_hang":12}' */
    jvalue_ref test_hang = jobject_get(params_obj, J_CSTR_TO_BUF("aurora_test_hang"));
    int hang_s = 0;
    if (jis_number(test_hang) && jnumber_get_i32(test_hang, &hang_s) == CONV_OK && hang_s > 0) {
        commons_log_warn("APP", "Test hang requested by the launch parameters (beta test hook): %d s", hang_s);
        app_test_hang_request((unsigned) hang_s);
    }
#endif
    jdomparser_release(&parser);
    char uuidbuf[64] = {0};
    memcpy(uuidbuf, params->default_host_uuid.data,
           sizeof(params->default_host_uuid.data) < sizeof(uuidbuf) - 1
               ? sizeof(params->default_host_uuid.data) : sizeof(uuidbuf) - 1);
    commons_log_info("APP", "Launch parsed: app_id=%d host_uuid=%s",
                     params->default_app_id, uuidbuf);
    return params;
}

void app_webos_open_ribbon() {
    static const char *payload = "{\"id\":\"com.webos.app.home\",\"params\":{\"launchType\":\"homeKey\"}}";
    HLunaServiceCallSync("luna://com.webos.applicationManager/launch", payload, true, NULL);
}

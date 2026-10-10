/**
 * @file app_wakelock_webos.c
 * @brief Keep the TV's own screensaver off while a stream is running.
 *
 * THE PROBLEM. The panel blanking mid-session ends a stream's usefulness, and on this
 * project it has repeatedly ended a diagnostic run too -- the screensaver fires, the
 * capture goes black, and the black capture then reads as "the change under test broke
 * the picture". Clearing it needs the physical remote: `lgtv.py screenon` is refused
 * (401, the stored client key has no power scope), `bt_wake.sh` is built for standby
 * rather than the screensaver, and `luna-send` is root-only for `prisoner`.
 *
 * WHY THE GENERIC IMPLEMENTATION DOES NOTHING HERE. app_wakelock_common.c calls
 * SDL_DisableScreenSaver(), which drives the *host desktop's* screensaver (X11 and
 * friends). webOS's screensaver is owned by com.webos.service.tvpower and does not
 * listen to SDL at all, so on the TV the wakelock has always been a no-op. The call
 * sites were already correct and already in place (root.c, USER_STREAM_OPEN ->
 * keep_awake true, USER_STREAM_CLOSE -> false); only the platform half was missing.
 *
 * HOW IT WORKS. tvpower lets a client register for screensaver requests and answer
 * each one. We subscribe once, then every time the TV asks to blank we answer
 * `ack:false` while a stream is up, and `ack:true` when it is not.
 *
 * ONLY WHILE STREAMING, DELIBERATELY. Refusing unconditionally would keep an OLED lit
 * on a static app list for as long as Eclipse is foregrounded, which is the one thing
 * an OLED must not do. The screensaver is correct behaviour when we are idle; it is
 * only wrong over live video.
 *
 * EVERY REQUEST IS LOGGED, AND THAT IS THE POINT, NOT DEBUG NOISE. A screensaver
 * firing mid-stream was itself EVIDENCE -- it meant the decoder had presented nothing
 * for long enough that the TV thought the picture was static, and this project has used
 * that signal. Silently suppressing it would destroy the signal. Logging each refusal
 * replaces an inference drawn from a black screenshot with an explicit, timestamped
 * line saying the TV wanted to blank and we said no, which is strictly better evidence.
 */
#include "app.h"

#include <SDL.h>
#include <libhelpers.h>
#include <pbnjson.h>

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

#define SCREENSAVER_CLIENT APPID   /* per-variant: a beta must not register as the live app */
#define URI_REGISTER "luna://com.webos.service.tvpower/power/registerScreenSaverRequest"
#define URI_RESPOND  "luna://com.webos.service.tvpower/power/responseScreenSaverRequest"

/* Read from the luna dispatch thread, written from the UI thread. */
static atomic_bool wakelock_held = false;
static bool subscribed = false;

/**
 * Screensaver events go to /tmp/aurora-diag.log as well as the normal log.
 *
 * Not belt-and-braces: commons_log output is not reachable from this project's
 * workflow, while /tmp/aurora-diag.log is the file every script here greps and the one
 * pulled after each run. A refusal nobody can see would leave us exactly where we
 * started -- inferring a blanked panel from a black screenshot. Same append-and-close
 * pattern as coop_input.c, which is deliberately cheap and crash-safe.
 */
static void screensaver_log(const char *fmt, ...) {
    FILE *f = fopen(AURORA_DIAG_LOG, "a");
    if (f == NULL) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* The subscription outlives the call, so its context cannot be a local. */
static HContext screensaver_ctx;

/** Reply to our own response call: log it, then release the context. */
static bool respond_reply_cb(LSHandle *sh, LSMessage *reply, void *ctx) {
    (void) sh;
    const char *msg = HLunaServiceMessage(reply);
    screensaver_log("[SCRSAVER] response -> %s", msg != NULL ? msg : "(no reply)");
    free(ctx);
    return true;
}

/**
 * Answer one screensaver request.
 *
 * MUST NOT USE HLunaServiceCallSync HERE, EVEN THOUGH IT IS THE OBVIOUS CALL. This runs
 * on the luna dispatch thread, and the sync helper blocks its caller on a condvar until
 * ITS OWN reply callback fires -- on that same thread. The reply can never be
 * dispatched, so the first screensaver request would deadlock luna handling for the
 * rest of the process's life. The failure would look like "the screensaver fix does
 * nothing" while actually having wedged the thread that also serves the capture probe.
 * Fire-and-forget instead; we do not need the answer to proceed.
 *
 * The context outlives this function, so it is heap-allocated and freed by the reply
 * callback rather than being a local or a shared static (two overlapping responses
 * would race a static).
 *
 * `timestamp` IS MANDATORY AND IS NOT A FORMALITY -- tvpower rejects the response with
 * "'timestamp' is needed, invalid param" if it is missing, and it rejected a literal 0
 * as well, so it has to be echoed back from the request rather than invented. It is a
 * STRING in the request payload; sending it as a number does not match.
 */
static void respond(const char *timestamp, bool ack) {
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"clientName\":\"%s\",\"ack\":%s,\"timestamp\":\"%s\"}",
             SCREENSAVER_CLIENT, ack ? "true" : "false", timestamp ? timestamp : "");

    HContext *ctx = calloc(1, sizeof(HContext));
    if (ctx == NULL) {
        return;
    }
    ctx->callback = respond_reply_cb;
    ctx->multiple = 0;
    ctx->pub = 1;
    screensaver_log("[SCRSAVER] ack=%d timestamp=%s", (int) ack, timestamp ? timestamp : "(none)");
    if (HLunaServiceCall(URI_RESPOND, payload, ctx) != 0) {
        screensaver_log("[SCRSAVER] response call FAILED");
        free(ctx);
    }
}

static bool screensaver_cb(LSHandle *sh, LSMessage *reply, void *ctx) {
    (void) sh;
    (void) ctx;
    const char *msg = HLunaServiceMessage(reply);
    if (msg == NULL) {
        return true;
    }

    /* The registration ack arrives on this same subscription and carries NO timestamp;
     * only a real request has one. So "has a timestamp" is what distinguishes them.
     * Parsed rather than string-matched, so a reworded reply cannot become a missed
     * request -- and a missed request means the panel blanks. */
    JSchemaInfo schema_info;
    jschema_info_init(&schema_info, jschema_all(), NULL, NULL);
    jdomparser_ref parser = jdomparser_create(&schema_info, 0);
    if (parser == NULL) {
        return true;
    }
    if (!jdomparser_feed(parser, msg, (int) strlen(msg)) || !jdomparser_end(parser)) {
        jdomparser_release(&parser);
        return true;
    }
    jvalue_ref parsed = jdomparser_get_result(parser);
    if (jis_valid(parsed)) {
        jvalue_ref ts_val = jobject_get(parsed, J_CSTR_TO_BUF("timestamp"));
        if (jis_string(ts_val)) {
            /* raw_buffer is NOT null-terminated -- strndup it before use. */
            raw_buffer buf = jstring_get(ts_val);
            char *ts = strndup(buf.m_str, buf.m_len);
            const bool hold = atomic_load(&wakelock_held);
            screensaver_log("[SCRSAVER] TV asked to blank (timestamp=%s) streaming=%d -> %s",
                            ts ? ts : "?", (int) hold, hold ? "REFUSED" : "allowed");
            respond(ts, !hold);
            free(ts);
        }
    }
    jdomparser_release(&parser);
    return true;
}

static void screensaver_subscribe(void) {
    if (subscribed) {
        return;
    }
    memset(&screensaver_ctx, 0, sizeof(screensaver_ctx));
    screensaver_ctx.callback = screensaver_cb;
    screensaver_ctx.multiple = 1;  /* a subscription: fires for every request */
    screensaver_ctx.pub = 1;

    char payload[128];
    snprintf(payload, sizeof(payload), "{\"clientName\":\"%s\",\"subscribe\":true}", SCREENSAVER_CLIENT);
    if (HLunaServiceCall(URI_REGISTER, payload, &screensaver_ctx) != 0) {
        /* Not fatal: we simply keep the old behaviour of being blanked. Logged because
         * the symptom (runs cut short again) is otherwise indistinguishable from the
         * bug this file fixes. */
        commons_log_warn("APP", "screensaver: could not subscribe to tvpower; the panel may blank mid-stream");
        screensaver_log("[SCRSAVER] subscribe FAILED -- the panel may blank mid-stream");
        return;
    }
    subscribed = true;
    commons_log_info("APP", "screensaver: subscribed to tvpower requests as %s", SCREENSAVER_CLIENT);
    screensaver_log("[SCRSAVER] subscribed to tvpower as %s", SCREENSAVER_CLIENT);
}

void app_set_keep_awake(app_t *app, bool awake) {
    (void) app;
    /* Harmless on webOS and correct everywhere else; left in so this stays a superset
     * of the common implementation rather than a replacement that quietly drops it. */
    if (awake) {
        SDL_DisableScreenSaver();
    } else {
        SDL_EnableScreenSaver();
    }

    /* Subscribe on the FIRST call whatever its value, not on the first stream.
     * app.c already calls this with false during startup, so the subscription is live
     * from launch and the app's behaviour does not depend on whether a stream has
     * happened yet. Subscribing while idle costs one luna call and changes nothing --
     * we answer "allowed", which is what the TV would have done unasked. The earlier
     * stream-only version also made the whole path unobservable while idle, and the
     * idle case is the ONLY one that can be exercised on demand: LG's screensaver does
     * not fire during recognised fullscreen playback at all, so a healthy stream never
     * produces a request to test with. */
    screensaver_subscribe();
    atomic_store(&wakelock_held, awake);
}

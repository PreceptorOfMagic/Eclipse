#include "pin_autoentry.h"

#include "app.h"
#include "app_launch.h"
#include "app_settings.h"
#include "backend/pcmanager.h"
#include "backend/types.h"
#include "stream/session.h"
#include "stream/input/keyboard_input.h"
#include "ui/launcher/apps.controller.h"
#include "logging.h"
#include "stream/input/pin_keys.h"

#include <Limelight.h>

#include "lvgl.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* ARMED HOSTS, NOT ONE ARMED HOST. A co-op launch can wake both PCs, and with a
 * single slot the second arm overwrote the first: one machine logged in, the other
 * sat on its lock screen streaming a black half. Four is room to spare -- co-op is
 * two -- and the array is tiny enough that a linear scan is the whole lookup. */
#define PIN_ARMED_MAX 4
static uuidstr_t g_armed[PIN_ARMED_MAX];
static int g_armed_count = 0;

/* THE DECISION HAS TO BE READABLE FROM THE TV, and commons_log is not -- the same
 * reason wol.c writes its wake here. Without this a passing test cannot tell "the lock
 * gate ran and correctly skipped" from "nothing was ever armed", which are the two
 * explanations for the same silence and only one of them means the fix works. */
static void pin_diag(const char *fmt, ...) {
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

static int armed_index(const uuidstr_t *uuid) {
    if (uuid == NULL) {
        return -1;
    }
    for (int i = 0; i < g_armed_count; ++i) {
        if (uuidstr_t_equals_t(uuid, &g_armed[i])) {
            return i;
        }
    }
    return -1;
}

/* WAS THIS HOST ACTUALLY ASLEEP OR OFF, or was it merely armed? Arming is unconditional
 * for both co-op machines, so it no longer distinguishes the two -- and the difference is
 * the only thing that makes it SAFE to sign in to a host that cannot report its own lock
 * state. Nobody is sitting at a keyboard playing a game on a machine that was not
 * answering the network sixty seconds ago. Parallel to g_armed[], same index. */
static bool g_armed_woken[PIN_ARMED_MAX];

static void armed_remove(int index) {
    if (index < 0 || index >= g_armed_count) {
        return;
    }
    for (int i = index; i < g_armed_count - 1; ++i) {
        g_armed[i] = g_armed[i + 1];
        g_armed_woken[i] = g_armed_woken[i + 1];
    }
    g_armed_count--;
}

typedef struct {
    app_t *global;          /* session fetched via global->session each tick (nulled on teardown) */
    char text[64];          /* the credential itself: PIN digits or a password */
    int text_len;
    bool is_password;       /* passwords always need Enter; Hello submits a PIN itself */
    int step;               /* 0 = dismiss lock screen, 1 = type + submit, 2 = HDR reconnect */
} pin_seq_t;

void pin_autoentry_arm(const uuidstr_t *uuid) {
    if (uuid == NULL || app_configuration == NULL || !app_configuration->pin_autoentry) {
        return;
    }
    /* Arm as long as auto-entry is enabled; the actual PIN (per-host, else the
     * global fallback) is resolved when the stream opens. */
    if (armed_index(uuid) >= 0) {
        return;                                 /* already armed; re-arming is a no-op */
    }
    if (g_armed_count >= PIN_ARMED_MAX) {
        /* Drop the OLDEST, not this one: the newest arm is the wake that just
         * happened and is the one about to be needed. */
        armed_remove(0);
    }
    g_armed_woken[g_armed_count] = false;
    g_armed[g_armed_count++] = *uuid;
    commons_log_info("PIN", "auto-entry armed for host after wake (%d armed)", g_armed_count);
}

void pin_autoentry_mark_woken(const uuidstr_t *uuid) {
    const int armed = armed_index(uuid);
    if (armed >= 0) {
        g_armed_woken[armed] = true;
    }
}

bool pin_autoentry_was_woken(const uuidstr_t *uuid) {
    const int armed = armed_index(uuid);
    return armed >= 0 && g_armed_woken[armed];
}

bool pin_autoentry_take(const uuidstr_t *uuid) {
    const int armed = armed_index(uuid);
    if (armed < 0) {
        return false;
    }
    armed_remove(armed);
    return true;
}

/* Resolve the PIN for the armed host: per-host pclist_t.pin_code, else the global
 * app_configuration->pin_code. Copies into buf (NUL-terminated). Returns buf, or
 * NULL if no PIN is configured for this host. */
static const char *resolve_pin(const uuidstr_t *uuid, char *buf, size_t buflen,
                               bool *is_password) {
    *is_password = false;
    /* Read on the LVGL/UI thread, same unlocked convention as server.context_menu.c;
     * copy out immediately so we never hold the node pointer past this call. */
    const pclist_t *node = pcmanager_node(pcmanager, uuid);
    if (node != NULL && node->pin_code != NULL && node->pin_code[0] != '\0') {
        snprintf(buf, buflen, "%s", node->pin_code);
        *is_password = node->pin_is_password;
        return buf;
    }
    if (app_configuration->pin_code != NULL && app_configuration->pin_code[0] != '\0') {
        snprintf(buf, buflen, "%s", app_configuration->pin_code);
        return buf;
    }
    return NULL;
}

static void send_key_tap(stream_input_t *input, short vk) {
    keyboard_modifier_state_t mods = {0};
    bool sys = app_configuration->syskey_capture;
    keyboard_input_send_key(input, vk, true, &mods, sys);
    keyboard_input_send_key(input, vk, false, &mods, sys);
}

/* One character, with Shift held around it when the key needs it. Shift is sent as a
 * real key down/up rather than only as the modifier flag: the lock screen is a secure
 * desktop and this is the sequence a physical keyboard produces. */
static void send_char(stream_input_t *input, char c) {
    short vk;
    bool shift;
    if (!pin_key_for_char(c, &vk, &shift)) {
        return;
    }
    keyboard_modifier_state_t mods = {0};
    bool sys = app_configuration->syskey_capture;
    if (shift) {
        keyboard_input_send_key(input, PIN_VK_SHIFT, true, &mods, sys);
    }
    keyboard_input_send_key(input, vk, true, &mods, sys);
    keyboard_input_send_key(input, vk, false, &mods, sys);
    if (shift) {
        keyboard_input_send_key(input, PIN_VK_SHIFT, false, &mods, sys);
    }
}

static void pin_seq_timer_cb(lv_timer_t *timer) {
    pin_seq_t *ctx = (pin_seq_t *) timer->user_data;
    /* global->session is set to NULL by app_session_destroy on teardown, so this
     * cannot use-after-free if the user disconnects mid-sequence. */
    session_t *session = ctx->global != NULL ? ctx->global->session : NULL;

    if (ctx->step == 2) {
        /* Login is done; reconnect once so the host re-negotiates in the now
         * logged-in session and brings HDR up (it can't be enabled on the
         * lock-screen/secure desktop the initial stream came up on). */
        apps_controller_reconnect_running_app(ctx->global);
        lv_timer_del(timer);
        free(ctx);
        return;
    }

    stream_input_t *input = session != NULL ? session_get_input(session) : NULL;
    if (input == NULL || !keyboard_input_available(input)) {
        lv_timer_del(timer);
        free(ctx);
        return;
    }

    if (ctx->step == 0) {
        /* Windows cold-boots to the lock screen ("swipe up") in front of the PIN
         * box; a key press dismisses it. Then wait for the credential screen. */
        send_key_tap(input, PIN_VK_RETURN);
        ctx->step = 1;
        lv_timer_set_period(timer, app_configuration->pin_dismiss_ms > 0
                                       ? app_configuration->pin_dismiss_ms
                                       : 100);
        return;
    }

    /* step 1: type it, then submit. A PASSWORD ALWAYS NEEDS ENTER -- Windows Hello
     * submits a PIN on the last digit, but nothing submits a password, so pin_submit
     * is treated as forced on for one. Without that, a correct password sits in the
     * box and the session looks, from here, exactly like a wrong one.
     *
     * AS TEXT BY DEFAULT, which is what makes this work on a host whose keyboard is
     * not US: the characters go over as characters and Apollo injects them with
     * KEYEVENTF_UNICODE, so no layout is involved. The key-code path is the fallback
     * for a host that does not implement the text channel. */
    if (app_configuration->pin_type_as_text) {
        LiSendUtf8TextEvent(ctx->text, (unsigned int) ctx->text_len);
    } else {
        for (int i = 0; i < ctx->text_len; i++) {
            send_char(input, ctx->text[i]);
        }
    }
    const bool submit = ctx->is_password || app_configuration->pin_submit;
    if (submit) {
        send_key_tap(input, PIN_VK_RETURN);
    }
    commons_log_info("PIN", "auto-entry: typed %d char(s) as %s via %s%s", ctx->text_len,
                     ctx->is_password ? "password" : "PIN",
                     app_configuration->pin_type_as_text ? "text" : "US key codes",
                     submit ? " + submit" : "");

    /* The step-2 reconnect exists only to bring HDR up (Apollo re-runs its HDR
     * display setup only on a fresh launch). Skip it when HDR is off -- there is
     * nothing to fix -- or when the user has turned the restart off, which is a
     * setting of its own because it costs a visible reconnect and not everyone
     * wants to pay it for HDR.
     *
     * AND NEVER IN CO-OP: IT KILLS THE OTHER PANE'S SIGN-IN BEFORE IT HAPPENS.
     *
     * The two hosts sign in from two different processes, and the secondary's sequence is
     * necessarily a little behind -- it cannot even start until the coordinator has
     * spawned it. Measured 2026-08-12, with role= now in the run banner so the processes
     * are distinguishable:
     *
     *   19:49:30  secondary pid=6090 starts, arms its credential
     *   19:49:38  secondary: stream live after 7750 ms, waiting the 4000 ms pre-delay
     *   19:49:42  (it should have typed here -- no line was ever written)
     *   19:49:48  secondary pid=8627: a NEW secondary process
     *
     * The primary signed in, waited pin_reconnect_delay_ms, and reconnected for HDR --
     * tearing down the session and taking the secondary with it, three seconds before it
     * was due to type. PC1 sat on its credential screen for the rest of the session. The
     * counters reset at the same instant, which is what produced the wrapped COOP-RATE
     * sample in session_video.c.
     *
     * A co-op launch negotiates its own half-height mode anyway, so the HDR renegotiation
     * this step buys is worth strictly less here than the sign-in it destroys. */
    if (!app_configuration->hdr || !app_configuration->pin_hdr_relaunch ||
        app_launch_coop_requested()) {
        lv_timer_del(timer);
        free(ctx);
        return;
    }

    /* After login, wait for the desktop to come up, then quit+relaunch once for HDR. */
    ctx->step = 2;
    lv_timer_set_period(timer, app_configuration->pin_reconnect_delay_ms > 0
                                   ? app_configuration->pin_reconnect_delay_ms
                                   : 4000);
}

/* HOW LONG TO KEEP ASKING WHETHER THE HOST IS LOCKED, AND WHY IT HAS TO ASK MORE
 * THAN ONCE.
 *
 * The gate used to read the cached status once, the instant the stream opened, and
 * treat that single answer as final. On 2026-08-12 that read `locked=no` for a PC2
 * that was sitting at its sign-in screen, and the sign-in was skipped: the machine had
 * COLD-BOOTED (it had dropped out of S3 into a hard power-off, so the magic packet
 * powered it on rather than resuming it), its Apollo had been running for about five
 * seconds when the gate asked, and that first answer is the least trustworthy one the
 * host will ever give -- `is_user_session_locked()` returns false on every failure
 * path, and early in boot the console session it queries may not be in its final state.
 * Ten seconds later the same host reported LOCK correctly, measured directly through
 * WTSQuerySessionInformationW.
 *
 * Nothing re-queried it, either: the co-op wake loop only refreshes hosts that are
 * still OFFLINE, so the poll that flipped this one online was also the last poll it
 * ever got.
 *
 * So: ask repeatedly, for a bounded time, and act on the first LOCKED answer. The
 * safety property is unchanged -- only a positive "locked" starts the sequence, so a
 * host with somebody sitting at it still never gets a credential typed into it. All
 * that changes is how many chances the host has to say so. */
#define PIN_LOCK_WAIT_MS 20000
#define PIN_LOCK_POLL_MS 2000

typedef struct {
    app_t *global;
    uuidstr_t uuid;
    int waited_ms;
    bool woken;             /* this host was not answering the network when we started */
} pin_wait_t;

/* HAS THIS HOST ANSWERED SINCE THE STREAM OPENED? Needed because "the host did not report
 * a lock state" is only meaningful once the host has actually been asked and replied --
 * before that it is just the old cached record, which says nothing. Module-static because
 * this gate runs for one host at a time in this process, and because the completion
 * arrives later on the main loop and must not hold a pointer to a freed wait context. */
static uuidstr_t g_pin_fresh_uuid;
static bool g_pin_fresh;

static void pin_status_done(int result, const char *error, const uuidstr_t *uuid, void *userdata) {
    (void) result;
    (void) error;
    (void) userdata;
    if (uuid != NULL && uuidstr_t_equals_t(uuid, &g_pin_fresh_uuid)) {
        g_pin_fresh = true;
    }
}

static void pin_sequence_start(app_t *global, const uuidstr_t *uuid);

static void pin_lock_wait_cb(lv_timer_t *timer) {
    pin_wait_t *w = (pin_wait_t *) timer->user_data;
    const pclist_t *node = pcmanager_node(pcmanager, &w->uuid);
    const bool locked = node != NULL && node->server != NULL && node->server->sessionLocked;
    if (locked) {
        pin_diag("[PIN] host reported LOCKED after %d ms -> signing in", w->waited_ms);
        app_t *global = w->global;
        uuidstr_t uuid = w->uuid;
        lv_timer_del(timer);
        free(w);
        pin_sequence_start(global, &uuid);
        return;
    }
    /* DECIDE AS SOON AS THE HOST HAS ANSWERED -- DO NOT SERVE OUT THE REST OF THE BUDGET.
     *
     * An Apollo built before the SessionLocked patch never emits the field at all, so it
     * can NEVER say "locked" no matter how long this waits. Evaluating the carve-out only
     * at the 20 s mark therefore made that wait mandatory for such a host: PC2 typed its
     * credential a guaranteed 20 s later than PC1, purely waiting for an answer that was
     * never coming. From the couch that is the two panes signing in one at a time.
     *
     * One fresh answer settles it. The wait that remains is only for hosts that CAN
     * report and have not yet said "locked" -- there, more polling genuinely can change
     * the answer, so the patience is still worth paying.
     *
     * The carve-out is unchanged in substance: never reported at all AND woken from
     * offline by this very launch -- a machine that was not answering the network a
     * minute ago has nobody sitting at it, so the thing this gate exists to prevent (a
     * credential typed into a running game) cannot be what is on screen. */
    const bool known = node != NULL && node->server != NULL && node->server->sessionLockedKnown;
    const bool silent_for_good = g_pin_fresh && !known;

    w->waited_ms += PIN_LOCK_POLL_MS;
    if (silent_for_good || w->waited_ms >= PIN_LOCK_WAIT_MS) {
        if (!known && w->woken) {
            pin_diag("[PIN] host does not report a lock state at all (host predates the "
                     "SessionLocked patch) but it was WOKEN from offline -> signing in "
                     "now, after %d ms", w->waited_ms);
            app_t *global = w->global;
            uuidstr_t uuid = w->uuid;
            lv_timer_del(timer);
            free(w);
            pin_sequence_start(global, &uuid);
            return;
        }
        pin_diag("[PIN] host did not report a lock screen in %d ms (reported=%s, woken=%s) "
                 "-> SKIPPING sign-in and the HDR relaunch", w->waited_ms,
                 known ? "yes" : "no", w->woken ? "yes" : "no");
        commons_log_info("PIN", "auto-entry armed but the host never reported a lock screen "
                                "-- skipping sign-in and the HDR relaunch");
        lv_timer_del(timer);
        free(w);
        return;
    }
    /* ASK THE HOST AGAIN, don't just re-read what we already have. SessionLocked only
     * rides along on a fresh PAIRED status query; re-reading the node in a loop would
     * spin on the same stale answer forever. */
    pcmanager_request_update(pcmanager, &w->uuid, pin_status_done, NULL);
}

void pin_autoentry_on_stream_open(app_t *global, const uuidstr_t *uuid) {
    /* Only fire for a host the wake was armed for; a stream for any other host
     * leaves the arms pending so none is consumed by the wrong PC. */
    const int armed = armed_index(uuid);
    if (armed < 0) {
        return;
    }
    const bool woken = g_armed_woken[armed];   /* read BEFORE the arm is consumed */
    armed_remove(armed); /* one shot per wake, per host */
    if (global == NULL) {
        return;
    }

    /* IS THIS HOST ACTUALLY SHOWING A LOCK SCREEN -- ASKED FROM HERE ON, REPEATEDLY,
     * WHICH IS THE WHOLE POINT.
     *
     * Arming is only a CANDIDATE list. wake_begin() cannot know the answer: a host it is
     * about to wake is not answering anything yet, and a host that is already up may be
     * locked or may have somebody sitting at it. So it arms both and defers the decision
     * to now, when the stream has just opened.
     *
     * WHAT THIS STOPS. Before Apollo reported the field there was no way to tell, so the
     * sequence ran blind at every host: on an already-unlocked PC that meant a stray
     * Enter and the PIN typed into whatever had focus, followed by a quit + relaunch --
     * the "disconnect and reconnect firing when nothing was asleep" the user reported.
     *
     * ABSENCE STILL READS AS UNLOCKED, deliberately, all the way down the chain (client.h
     * sessionLocked, and Apollo's is_user_session_locked() which returns false on every
     * failure path). Skipping a sign-in that was needed costs a lock screen somebody
     * dismisses by hand; running one that was not costs a password typed into a game.
     * The wait above only decides how long absence is allowed to be provisional. */
    pin_wait_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        return;
    }
    w->global = global;
    w->uuid = *uuid;
    w->woken = woken;
    /* Arm the freshness flag BEFORE asking, so a completion that lands between the two
     * cannot be credited to a question we had not yet put. */
    g_pin_fresh_uuid = *uuid;
    g_pin_fresh = false;
    pin_diag("[PIN] armed host reached stream open: asking whether it is locked "
             "(up to %d ms, but deciding as soon as it answers)", PIN_LOCK_WAIT_MS);
    pcmanager_request_update(pcmanager, uuid, pin_status_done, NULL);
    lv_timer_create(pin_lock_wait_cb, PIN_LOCK_POLL_MS, w);
}

static void pin_sequence_start(app_t *global, const uuidstr_t *uuid) {
    char pin_buf[64];
    bool is_password = false;
    const char *code = resolve_pin(uuid, pin_buf, sizeof(pin_buf), &is_password);
    if (code == NULL || code[0] == '\0') {
        commons_log_info("PIN", "auto-entry armed but no PIN configured for host");
        return;
    }

    pin_seq_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return;
    }
    ctx->global = global;
    ctx->is_password = is_password;
    int n = 0;
    for (const char *p = code; *p != '\0' && n < (int) sizeof(ctx->text); p++) {
        short vk;
        bool shift;
        /* REFUSE a credential the key-code path cannot type, rather than typing the
         * rest of it: a partial password is indistinguishable from a wrong one at the
         * lock screen. Only a concern on that path -- as text, any character goes. */
        if (!app_configuration->pin_type_as_text && !pin_key_for_char(*p, &vk, &shift)) {
            commons_log_warn("PIN", "auto-entry: credential has a character that cannot be "
                                    "typed as US key codes -- not attempting sign-in. Turn "
                                    "\"Send the credential as text\" on.");
            free(ctx);
            return;
        }
        ctx->text[n++] = *p;
    }
    ctx->text_len = n;
    if (n == 0) {
        free(ctx);
        return;
    }
    ctx->step = app_configuration->pin_dismiss ? 0 : 1;
    int period = app_configuration->pin_predelay_ms > 0 ? app_configuration->pin_predelay_ms : 3000;
    lv_timer_create(pin_seq_timer_cb, period, ctx);
    commons_log_info("PIN", "auto-entry sequence scheduled: %d char(s) as %s, predelay %d ms", n,
                     is_password ? "password" : "PIN", period);
}

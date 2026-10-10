#include "coop_secondary.h"
#include "stream/input/coop_input.h"
#include "stream/input/pin_keys.h"
#include "app_launch.h"
#include "coop_ipc.h"
#include "coop_session.h"

#include <Limelight.h>
#include "app.h"
#include "app_session.h"
#include "backend/pcmanager.h"
#include "stream/session.h"
#include "client.h"
#include "errors.h"
#include "xml.h"
#include "uuidstr.h"

#include <errno.h>
#include <execinfo.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Consecutive reconnects with ZERO frames forwarded before the secondary gives
 * up. 3 is ~ a few seconds of proving it is dead, short enough that a closed app
 * stops hammering the other host promptly, long enough that a genuine transient
 * drop still recovers. */
#define COOP_DEAD_CYCLES 3

static const char *coop_mode_socket = NULL;
static intptr_t coop_fd = -1;
static unsigned char *coop_buf = NULL;
static size_t coop_buf_cap = 0;
static _Atomic unsigned coop_fwd = 0;
static _Atomic unsigned coop_drop = 0;
/* The video callback and startup/error callbacks can run on different threads.
 * Serialise every write so a one-byte READY/FAIL frame can never be interleaved
 * with a compressed access unit on Windows' stream transport. */
static pthread_mutex_t coop_send_lock = PTHREAD_MUTEX_INITIALIZER;
/* Protected by coop_send_lock. A failed framed write may have placed only a
 * prefix on the stream, so the connection is poisoned until the control loop
 * shuts it down, closes it, and reconnects. */
static bool coop_link_failed = false;
enum coop_secondary_start_state {
    COOP_SECONDARY_PENDING = 0,
    COOP_SECONDARY_READY,
    COOP_SECONDARY_FAILED,
};
static enum coop_secondary_start_state coop_start_state = COOP_SECONDARY_PENDING;

static void coop_secondary_report_start_failure(uint8_t code) {
    pthread_mutex_lock(&coop_send_lock);
    if (coop_start_state == COOP_SECONDARY_PENDING) {
        /* FAILED is terminal even when the write itself loses a race with a
         * broken connection. A decode callback already in flight must never
         * follow an explicit FAIL with READY and make startup look healthy. */
        coop_start_state = COOP_SECONDARY_FAILED;
    }
    if (coop_start_state == COOP_SECONDARY_FAILED && coop_fd >= 0 && !coop_link_failed) {
        (void) coop_ipc_send_ctl(coop_fd, code);
    }
    pthread_mutex_unlock(&coop_send_lock);
}

void coop_secondary_launch_failed(const char *gs_error) {
    /* Apollo's words for a host busy with a DIFFERENT app (nvhttp: "An app is already running on
     * this host"); anything else is a plain refusal. Either way the primary hears it now instead of
     * waiting out its 30 s and blaming the connection (issue #33). */
    const bool busy = gs_error != NULL && strstr(gs_error, "already running") != NULL;
    coop_secondary_report_start_failure(busy ? COOP_IPC_CTL_FAIL_BUSY : COOP_IPC_CTL_FAIL_LAUNCH);
}

void coop_secondary_parse_args(int argc, char *argv[]) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] != NULL && strcmp(argv[i], "--coop-secondary") == 0) {
            coop_mode_socket = argv[i + 1];
            /* The secondary is a SEPARATE PROCESS and never sees the launch payload, so
             * `coop:true` never reached it -- only the coordinator got the flag. That
             * left the primary asking its host for 3840x1080 while the secondary still
             * asked for 3840x2160, which cannot be spliced and showed up as a collapsed
             * pair rate. Being started with --coop-secondary IS proof of co-op, so set
             * the flag from the argument itself. */
            app_launch_set_coop_requested(true);
            return;
        }
    }
}

const char *coop_secondary_mode(void) { return coop_mode_socket; }

/* The child was dying silently: no exit line, no core, and webOS exposes no
 * crash log to the developer user, so "the secondary stopped" was
 * indistinguishable from "it never sent anything". Report our own death. */
static void coop_fatal_signal(int sig) {
    FILE *f = fopen(AURORA_SECONDARY_LOG, "a");
    if (f != NULL) {
        fprintf(f, "!!! secondary FATAL signal=%d (forwarded=%u)\n", sig, coop_fwd);
        void *frames[24];
        int n = backtrace(frames, 24);
        char **syms = backtrace_symbols(frames, n);
        for (int i = 0; syms != NULL && i < n; ++i) {
            fprintf(f, "    #%d %s\n", i, syms[i]);
        }
        fflush(f);
        fclose(f);
    }
    _exit(128 + sig);
}

bool coop_secondary_init(const char *socket_path) {
    signal(SIGSEGV, coop_fatal_signal);
    signal(SIGABRT, coop_fatal_signal);
    signal(SIGBUS, coop_fatal_signal);
    signal(SIGFPE, coop_fatal_signal);
    signal(SIGILL, coop_fatal_signal);
    signal(SIGTERM, coop_fatal_signal);
    coop_fd = coop_ipc_connect(socket_path);
    if (coop_fd < 0) {
        return false;
    }
    /* Non-blocking: a stalled coordinator must never back-pressure this
     * session's network thread into dropping its own packets. We would rather
     * drop a pane frame than damage reception. */
    if (!coop_ipc_set_nonblocking(coop_fd)) {
        close(coop_fd);
        coop_fd = -1;
        return false;
    }
    coop_start_state = COOP_SECONDARY_PENDING;
    coop_link_failed = false;
    /* Request the SAME slices-per-frame the primary asks its host for. Without
     * this the secondary advertised only DIRECT_SUBMIT, so PC2 encoded ONE slice
     * per picture and the splice could never find a second slice to take --
     * paired stayed 0 no matter what else was fixed. Same sentinel as the
     * primary so both hosts stay in step without a rebuild. */
    unsigned slices = 2;
    FILE *sf = fopen("/tmp/coop-slices", "r");
    if (sf != NULL) {
        unsigned v = 0;
        if (fscanf(sf, "%u", &v) == 1 && v >= 1 && v <= 8) { slices = v; }
        fclose(sf);
    }
    coop_secondary_callbacks.capabilities |= CAPABILITY_SLICES_PER_FRAME(slices);

    coop_buf_cap = 4u * 1024u * 1024u;   /* 4K IDRs run to megabytes */
    coop_buf = malloc(coop_buf_cap);
    if (coop_buf == NULL) {
        close(coop_fd);
        coop_fd = -1;
        coop_buf_cap = 0;
        return false;
    }
    return true;
}

void coop_secondary_shutdown(void) {
    pthread_mutex_lock(&coop_send_lock);
    coop_link_failed = true;
    if (coop_fd >= 0) {
        shutdown(coop_fd, SHUT_RDWR);
        close(coop_fd);
        coop_fd = -1;
    }
    pthread_mutex_unlock(&coop_send_lock);
    free(coop_buf);
    coop_buf = NULL;
    coop_buf_cap = 0;
}

/* ---------------------------------------------------- signing in this host in
 *
 * WHY THE SECONDARY DOES ITS OWN LOGIN. A co-op launch can wake BOTH PCs, and a
 * machine that was asleep comes back to a lock screen -- a co-op half showing a lock
 * screen is as useless as no half at all. The primary's pin_autoentry lives in the
 * UI layer (streaming.controller.c), and this process is deliberately headless: no
 * window, no LVGL, no fragments, so none of that code ever runs here. What it does
 * have is its own connection to its own host, which is all a login needs.
 *
 * IT DOES NOT DO THE HDR QUIT-AND-RELAUNCH that the primary's step 2 does. Quitting
 * is a whole-session decision and the primary owns it; a secondary that quit as well
 * would tear down a session the primary was in the middle of rebuilding.
 */

typedef struct {
    char text[64];
    int text_len;
    int predelay_ms, dismiss_ms;
    bool dismiss, submit, as_text;
    uuidstr_t uuid;         /* to ask the host afterwards whether it actually unlocked */
} pin_login_t;

/* CONFIRM, THEN RETRY. A live video stream does not prove the host can take input yet.
 *
 * Measured 2026-08-12: PC1 resumed from S3 at 18:23:16, the stream went live 8250 ms
 * later, the credential was typed at 18:23:32 -- and PC1's Security log recorded NO
 * logon attempt of any kind. Not a rejection: nothing. Sixteen seconds out of sleep its
 * lock screen was not yet taking injected input, and the machine sat on that lock screen
 * for the rest of the session. The identical sequence had worked earlier the same day on
 * a host that had been locked and awake for a while, so no fixed pre-delay is right for
 * both cases.
 *
 * Rather than guess a bigger constant, ask the host. It reports SessionLocked, so the
 * sequence can check whether the credential landed and type again if it did not. The
 * retry is gated on the host STILL REPORTING LOCKED, which is what keeps it safe -- a
 * host that unlocked, or that cannot answer, is never typed at twice. */
#define PIN_ATTEMPTS 3
#define PIN_RETRY_WAIT_MS 6000

static void pin_tap(short vk) {
    LiSendKeyboardEvent(vk, KEY_ACTION_DOWN, 0);
    LiSendKeyboardEvent(vk, KEY_ACTION_UP, 0);
}

/* Shift held around the key when it needs it, as a real down/up -- the lock screen is
 * a secure desktop and this is what a physical keyboard produces. */
static void pin_type(char c) {
    short vk;
    bool shift;
    if (!pin_key_for_char(c, &vk, &shift)) {
        return;
    }
    if (shift) {
        LiSendKeyboardEvent(PIN_VK_SHIFT, KEY_ACTION_DOWN, MODIFIER_SHIFT);
    }
    LiSendKeyboardEvent(vk, KEY_ACTION_DOWN, shift ? MODIFIER_SHIFT : 0);
    LiSendKeyboardEvent(vk, KEY_ACTION_UP, shift ? MODIFIER_SHIFT : 0);
    if (shift) {
        LiSendKeyboardEvent(PIN_VK_SHIFT, KEY_ACTION_UP, 0);
    }
}

static void pin_sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long) (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* STAMPED WITH THE WALL CLOCK, unlike the tick lines around it. Everything this log has
 * to be lined up against -- Apollo's "CLIENT CONNECTED", Windows' Security log, the
 * coordinator's own diag -- is wall-clock, and correlating them by counting lines is how
 * a four-second race went unnoticed. */
static void pin_log(const char *fmt, ...) {
    FILE *f = fopen(AURORA_SECONDARY_LOG, "a");
    if (f == NULL) {
        return;
    }
    char stamp[32];
    const time_t now = time(NULL);
    struct tm tm_buf;
    if (localtime_r(&now, &tm_buf) != NULL && strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm_buf) > 0) {
        fprintf(f, "%s  ", stamp);
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* How long to wait for this host's stream to actually come up before giving up on
 * typing anything into it. Generous: the host may have just cold-booted. */
#define PIN_LIVE_WAIT_MS 60000
#define PIN_LIVE_POLL_MS 250

/** Ask the host whether it is still showing a lock screen.
 *  @return 1 locked, 0 unlocked, -1 it did not tell us (old Apollo, or query failed). */
static int pin_host_locked(const uuidstr_t *uuid) {
    const pclist_t *node = pcmanager_node(pcmanager, uuid);
    if (node == NULL || node->server == NULL || app_configuration == NULL) {
        return -1;
    }
    GS_CLIENT gs = gs_new(app_configuration->key_dir);
    if (gs == NULL) {
        return -1;
    }
    SERVER_DATA probe;
    memset(&probe, 0, sizeof(probe));
    const char *addr = node->server->serverInfo.address;
    const uint16_t port = node->server->extPort ? node->server->extPort : 47989;
    const int st = gs_get_status(gs, &probe, strdup(addr), port, true);
    const int result = st != 0 ? -1 : (probe.sessionLockedKnown ? (probe.sessionLocked ? 1 : 0) : -1);
    gs_destroy(gs);
    return result;
}

/* Type the credential once. Split out so the retry does not duplicate it. */
static void pin_type_credential(const pin_login_t *pin) {
    if (pin->dismiss) {
        pin_tap(PIN_VK_RETURN);        /* the "swipe up" cover in front of the PIN box */
        pin_sleep_ms(pin->dismiss_ms);
    }
    /* Text by default -- layout-independent; see pin_keys.h and app_settings.h. */
    if (pin->as_text) {
        LiSendUtf8TextEvent(pin->text, (unsigned int) pin->text_len);
    } else {
        for (int i = 0; i < pin->text_len; ++i) {
            pin_type(pin->text[i]);
        }
    }
    if (pin->submit) {
        pin_tap(PIN_VK_RETURN);
    }
}

/* ON ITS OWN THREAD, not folded into the loop below: that loop blocks in
 * coop_ipc_recv_au waiting on the coordinator, so timing anything by it would make
 * the login's delays depend on how busy the peer's video link is. */
static void *pin_login_thread(void *arg) {
    pin_login_t *pin = arg;

    /* WAIT FOR A CONNECTION THAT EXISTS, THEN start the pre-delay.
     *
     * This thread used to begin its pre-delay the moment session_create() RETURNED --
     * and session_create only spawns the session worker, so it returns while the
     * connection is still being negotiated. On 2026-08-12 that raced and lost: the
     * secondary logged "typed 4 char(s) via text + submit", and the host's own Security
     * log recorded NO logon attempt of any kind for the entire window -- no 4625, no
     * 4801. The keystrokes went into a connection that did not exist yet and were
     * dropped on the floor, and the PC streamed its lock screen for the rest of the
     * session. Apollo logged CLIENT CONNECTED about four seconds AFTER the credential
     * had already been "typed".
     *
     * A forwarded frame is the unambiguous proof: video is arriving from this host and
     * being handed to the coordinator, so the connection is up in both directions. No
     * new state and no new signal -- coop_fwd is the counter the tick log already
     * prints. */
    int waited = 0;
    while (coop_fwd == 0 && waited < PIN_LIVE_WAIT_MS) {
        pin_sleep_ms(PIN_LIVE_POLL_MS);
        waited += PIN_LIVE_POLL_MS;
    }
    if (coop_fwd == 0) {
        pin_log("pin auto-entry: ABANDONED -- no frame forwarded in %d ms, so there is no "
                "live connection to type into", PIN_LIVE_WAIT_MS);
        free(pin);
        return NULL;
    }
    pin_log("pin auto-entry: stream live after %d ms, waiting the %d ms pre-delay",
            waited, pin->predelay_ms);
    pin_sleep_ms(pin->predelay_ms);
    pin_type_credential(pin);
    /* The wait is logged because it is the number that tells a failed sign-in
     * ("typed into nothing") from a wrong credential ("typed, host said no"). */
    pin_log("pin auto-entry: typed %d char(s) via %s%s (stream went live after %d ms)",
            pin->text_len, pin->as_text ? "text" : "US key codes",
            pin->submit ? " + submit" : "", waited);

    /* Did it land? See the comment on PIN_ATTEMPTS -- a host fresh out of S3 can be
     * streaming video several seconds before its lock screen will accept injected
     * input, and the credential simply vanishes. Only a host that still says LOCKED is
     * typed at again. */
    for (int attempt = 2; attempt <= PIN_ATTEMPTS; ++attempt) {
        pin_sleep_ms(PIN_RETRY_WAIT_MS);
        const int state = pin_host_locked(&pin->uuid);
        if (state == 0) {
            pin_log("pin auto-entry: host reports UNLOCKED -- signed in");
            break;
        }
        if (state < 0) {
            pin_log("pin auto-entry: host will not report its lock state -- not retrying");
            break;
        }
        pin_log("pin auto-entry: host STILL LOCKED -- credential did not land, retyping "
                "(attempt %d of %d)", attempt, PIN_ATTEMPTS);
        pin_type_credential(pin);
        if (attempt == PIN_ATTEMPTS) {
            pin_sleep_ms(PIN_RETRY_WAIT_MS);
            const int final_state = pin_host_locked(&pin->uuid);
            pin_log("pin auto-entry: after %d attempts the host reports %s", PIN_ATTEMPTS,
                    final_state == 0 ? "UNLOCKED" : final_state == 1 ? "STILL LOCKED"
                                                                     : "nothing");
        }
    }
    free(pin);
    return NULL;
}

static void coop_secondary_autologin(const uuidstr_t *uuid, bool locked, bool locked_known) {
    /* Set by the launch path when it woke this host. Absent means the machine was
     * already up, and typing a PIN into a logged-in desktop -- or a running game --
     * is exactly what this flag exists to prevent. */
    /* EVERY EXIT FROM HERE IS LOGGED. It used to return silently on five different
     * conditions, so "the second PC sat on its lock screen" and "auto-entry was never
     * meant to run" produced exactly the same empty log -- and the only way to tell
     * them apart was to read this function. */
    const char *armed = getenv("COOP_PIN_AUTOENTRY");
    if (armed == NULL || armed[0] != '1' || app_configuration == NULL ||
        !app_configuration->pin_autoentry) {
        pin_log("pin auto-entry: not running (COOP_PIN_AUTOENTRY=%s, feature=%s)",
                armed != NULL ? armed : "unset",
                app_configuration != NULL && app_configuration->pin_autoentry ? "on" : "off");
        return;
    }

    /* THE SAME LOCK GATE THE PRIMARY PANE HAS. This path used to type whenever it was
     * armed, and arming became unconditional for both co-op machines -- so a secondary
     * host that was already up, signed in and playing something would get a credential
     * typed into it. The primary got a gate for exactly that; this is its other half,
     * built from the status query this process already makes a few lines above, so it
     * costs no extra round trip.
     *
     * A host that reports "not locked" is believed. A host that reports NOTHING (an
     * Apollo older than the SessionLocked patch) is signed in to only when it was woken
     * from offline by this launch, because a machine that was not on the network cannot
     * have somebody sitting at it. */
    const bool woken = getenv("COOP_PIN_WOKEN") != NULL && getenv("COOP_PIN_WOKEN")[0] == '1';
    if (locked_known && !locked) {
        pin_log("pin auto-entry: host reports it is NOT locked -- skipping sign-in");
        return;
    }
    if (!locked_known && !woken) {
        pin_log("pin auto-entry: host never reported a lock state (host predates the "
                "SessionLocked patch) and was not woken from offline -- skipping sign-in");
        return;
    }
    pin_log("pin auto-entry: proceeding (locked=%s reported=%s woken=%s)",
            locked ? "yes" : "no", locked_known ? "yes" : "no", woken ? "yes" : "no");
    const pclist_t *node = pcmanager_node(pcmanager, uuid);
    /* WHOSE credential, said out loud. A host with no per-host pin_code falls back to
     * the global one, which is right for a one-PC setup and wrong for two machines with
     * different accounts -- PC2 has no pin_code of its own, so it gets PC1's. That is a
     * configuration gap, not a code one, but it is invisible unless the log names it. */
    const bool per_host = node != NULL && node->pin_code != NULL && node->pin_code[0] != '\0';
    const char *code = per_host ? node->pin_code : app_configuration->pin_code;
    if (code == NULL || code[0] == '\0') {
        pin_log("pin auto-entry: no credential configured for this host, and no global one");
        return;
    }
    pin_log("pin auto-entry: armed, using the %s credential",
            per_host ? "per-host" : "GLOBAL fallback");
    pin_login_t *pin = calloc(1, sizeof(*pin));
    if (pin == NULL) {
        return;
    }
    for (const char *p = code; *p != '\0' && pin->text_len < (int) sizeof(pin->text); ++p) {
        short vk;
        bool shift;
        /* Refuse rather than type a mangled credential -- see pin_keys.h. Only the
         * key-code path has that problem; as text, any character goes. */
        if (!app_configuration->pin_type_as_text && !pin_key_for_char(*p, &vk, &shift)) {
            free(pin);
            return;
        }
        pin->text[pin->text_len++] = *p;
    }
    if (pin->text_len == 0) {
        free(pin);
        return;
    }
    pin->predelay_ms = app_configuration->pin_predelay_ms > 0
                       ? app_configuration->pin_predelay_ms : 3000;
    pin->dismiss_ms = app_configuration->pin_dismiss_ms > 0
                      ? app_configuration->pin_dismiss_ms : 100;
    pin->as_text = app_configuration->pin_type_as_text;
    pin->uuid = *uuid;      /* so the thread can ask this host whether it unlocked */
    pin->dismiss = app_configuration->pin_dismiss;
    /* A password is never auto-submitted by Windows, so Enter is forced for one. */
    pin->submit = node != NULL && node->pin_is_password ? true : app_configuration->pin_submit;

    pthread_t tid;
    if (pthread_create(&tid, NULL, pin_login_thread, pin) != 0) {
        free(pin);
        return;
    }
    pthread_detach(tid);
}

static bool coop_secondary_apply_stream_geometry(struct app_t *app, FILE *log) {
    const char *width_text = getenv("COOP_STREAM_WIDTH");
    const char *height_text = getenv("COOP_STREAM_HEIGHT");
    char *width_end = NULL;
    char *height_end = NULL;
    errno = 0;
    const long width = width_text != NULL ? strtol(width_text, &width_end, 10) : 0;
    const bool width_valid = errno == 0 && width_text != NULL && width_end != width_text &&
                             width_end != NULL && *width_end == '\0' &&
                             width > 0 && width <= INT_MAX;
    errno = 0;
    const long height = height_text != NULL ? strtol(height_text, &height_end, 10) : 0;
    const bool height_valid = errno == 0 && height_text != NULL && height_end != height_text &&
                              height_end != NULL && *height_end == '\0' &&
                              height > 0 && height <= INT_MAX;
    if (!width_valid || !height_valid || app_configuration == NULL) {
        if (log != NULL) {
            fprintf(log, "invalid inherited stream geometry width=%s height=%s\n",
                    width_text != NULL ? width_text : "<unset>",
                    height_text != NULL ? height_text : "<unset>");
            fflush(log);
        }
        return false;
    }

    /* The helper deliberately never initializes SDL video or creates a window, so the
     * desktop fallback in settings_initialize() cannot be replaced from a local display.
     * Inherit the already-resolved full-frame geometry from the visible primary process;
     * session_config_init() will then apply the same co-op split to both roles. */
    app_configuration->stream.width = (int) width;
    app_configuration->stream.height = (int) height;
    settings_refresh_auto_bitrate(app_configuration, &app->ss4s.video_cap);
    if (log != NULL) {
        fprintf(log, "inherited primary stream geometry %ldx%ld\n", width, height);
        fflush(log);
    }
    return true;
}

int coop_secondary_run_headless(struct app_t *app) {
    FILE *log = fopen(AURORA_SECONDARY_LOG, "a");
    if (log != NULL) {
        fprintf(log, "=== secondary headless start (pid=%d) ===\n", (int) getpid());
        fflush(log);
    }

    if (!coop_secondary_apply_stream_geometry(app, log)) {
        if (log != NULL) { fclose(log); }
        coop_secondary_report_start_failure(COOP_IPC_CTL_FAIL_CONFIG);
        return COOP_IPC_CTL_FAIL_CONFIG;
    }

    /* Start the second session without any UI. session_create only reads id and
     * name from the app entry, so a synthesized one is enough -- we deliberately
     * do NOT fetch the host's app list, which would need another round trip and
     * add a failure mode for no benefit. */
    const char *uuid_str = getenv("COOP_HOST_UUID");
    const char *app_id_str = getenv("COOP_APP_ID");
    if (uuid_str != NULL && app_id_str != NULL) {
        uuidstr_t uuid;
        uuidstr_fromchars(&uuid, strlen(uuid_str), uuid_str);
        /* Populate SERVER_DATA ourselves rather than going through pcmanager.
         * pcmanager dispatches its query completion on the app's MAIN LOOP, which
         * this process deliberately never runs (running it would create a window
         * and steal the foreground from the coordinator). gs_get_status is
         * synchronous and self-contained, which is what a headless process needs.
         *
         * The address comes from the host list entry we were pointed at; only its
         * address is trustworthy without a query, which is exactly what we fix here.
         */
        const pclist_t *node = pcmanager_node(pcmanager, &uuid);
        if (node == NULL) {
            if (log != NULL) { fprintf(log, "host %s not in hosts.ini\n", uuid_str); fclose(log); }
            coop_secondary_report_start_failure(COOP_IPC_CTL_FAIL_HOST);
            return COOP_IPC_CTL_FAIL_HOST;
        }
        SERVER_DATA server;
        memset(&server, 0, sizeof(server));
        GS_CLIENT gs = gs_new(app_configuration->key_dir);
        int st = -1;
        if (gs != NULL) {
            const char *addr = node->server->serverInfo.address;
            uint16_t port = node->server->extPort ? node->server->extPort : 47989;
            /* unsupported=true: libgamestream validates the requested mode against
             * server->modes, and PC2 advertises NO DisplayMode entries at all, so
             * the list is empty and the check can never pass -- it failed
             * identically at 165fps, 60fps, HDR on and HDR off. The flag exists
             * for exactly this case. */
            st = gs_get_status(gs, &server, strdup(addr), port, true);
            if (log != NULL) {
                fprintf(log, "gs_get_status(%s:%u) -> %d  paired=%d appid=%d\n", addr,
                        (unsigned) port, st, server.paired, server.currentGame);
                fflush(log);
            }
        } else if (log != NULL) {
            fprintf(log, "gs_new failed (keydir=%s)\n",
                    app_configuration->key_dir ? app_configuration->key_dir : "(null)");
            fflush(log);
        }
        if (st != 0) {
            if (log != NULL) { fprintf(log, "aborting: host query failed\n"); fclose(log); }
            if (gs != NULL) { gs_destroy(gs); }
            coop_secondary_report_start_failure(COOP_IPC_CTL_FAIL_QUERY);
            return COOP_IPC_CTL_FAIL_QUERY;
        }
        /* Do not assume that an Apollo app id copied from the primary identifies the
         * same entry on the peer. IDs are host-owned configuration, so a stale id can
         * exist while naming Desktop or an unrelated game. The UI resolved the exact
         * Co-op Pane entry on this host; verify that snapshot again immediately before
         * launch so a host-side edit cannot turn co-op into a recursive desktop capture. */
        char *app_id_end = NULL;
        const long requested_app_id = strtol(app_id_str, &app_id_end, 10);
        PAPP_LIST apps = NULL;
        const int apps_status = app_id_end != app_id_str && app_id_end != NULL && *app_id_end == '\0' &&
                                requested_app_id > 0 && requested_app_id <= INT_MAX
                                ? gs_applist(gs, &server, &apps) : GS_INVALID;
        bool app_found = false;
        for (PAPP_LIST item = apps; item != NULL; item = item->next) {
            if (item->id == (int) requested_app_id && item->name != NULL &&
                strcmp(item->name, COOP_SESSION_APP_NAME) == 0) {
                app_found = true;
                break;
            }
        }
        while (apps != NULL) {
            PAPP_LIST next = apps->next;
            free(apps->name);
            free(apps);
            apps = next;
        }
        if (apps_status != GS_OK || !app_found) {
            if (log != NULL) {
                fprintf(log, "aborting: app id %s is not the exact '%s' entry on peer (applist=%d)\n",
                        app_id_str, COOP_SESSION_APP_NAME, apps_status);
                fclose(log);
            }
            gs_destroy(gs);
            coop_secondary_report_start_failure(COOP_IPC_CTL_FAIL_APP);
            return COOP_IPC_CTL_FAIL_APP;
        }
        /* session_create directly: app_session_begin would re-fetch the stub node
         * from pcmanager and discard the server data we just queried. */
        APP_LIST entry = {.name = (char *) "coop", .id = (int) requested_app_id, .hdr = 0, .next = NULL};
        app_t *a = (app_t *) app;
        a->session = session_create(a, app_configuration, &server, &entry);
        int rc = a->session != NULL ? 0 : -1;
        gs_destroy(gs);
        if (log != NULL) {
            fprintf(log, "session_begin(host=%s app=%s) -> %d\n", uuid_str, app_id_str, rc);
            fflush(log);
        }
        if (rc == 0) {
            coop_secondary_autologin(&uuid, server.sessionLocked, server.sessionLockedKnown);
        } else {
            coop_secondary_report_start_failure(COOP_IPC_CTL_FAIL_SESSION);
            if (log != NULL) {
                fclose(log);
            }
            return COOP_IPC_CTL_FAIL_SESSION;
        }
    } else {
        if (log != NULL) {
            fprintf(log, "no COOP_HOST_UUID/COOP_APP_ID: refusing to start\n");
            fclose(log);
        }
        coop_secondary_report_start_failure(COOP_IPC_CTL_FAIL_CONFIG);
        return COOP_IPC_CTL_FAIL_CONFIG;
    }

    /* Exit as soon as the coordinator goes away, so a stale child cannot linger
     * holding a session -- leaked processes have wedged this TV before. */
    unsigned char probe[8];
    unsigned ticks = 0;
    unsigned idr_asks = 0;
    /* Watchdog state for the dead-cycle check at the reconnect site below. */
    unsigned fwd_at_last_loss = 0;
    int dead_cycles = 0;
    for (;;) {
        uint32_t ctl_frame = 0;
        pthread_mutex_lock(&coop_send_lock);
        long n = coop_fd >= 0 && !coop_link_failed
                 ? coop_ipc_recv_au(coop_fd, probe, sizeof(probe), &ctl_frame, NULL)
                 : -1;
        pthread_mutex_unlock(&coop_send_lock);
        if (n > 0 && ctl_frame == COOP_IPC_CTL_FRAME) {
            if (probe[0] == COOP_IPC_CTL_SHUTDOWN) {
                if (log != NULL) {
                    fprintf(log, "coordinator requested an orderly shutdown\n");
                    fflush(log);
                }
                break;
            }
            if (probe[0] == COOP_IPC_CTL_REQUEST_IDR) {
                /* Our host IRAP'd, so the composite just overwrote this host's pane.
                 * Only an IDR from THIS connection can hand the pane back. */
                LiRequestIdrFrame();
                if (log != NULL) {
                    fprintf(log, "peer asked for an IDR (#%u)\n", ++idr_asks);
                    fflush(log);
                }
            }
        }
        if (n < 0) {
            /* RECONNECT rather than exit. The link has been observed dropping
             * mid-session (coordinator reads EOF, we then get EPIPE) and the whole
             * pane feed died with it, leaving the composite showing our own half in
             * both panes. The coordinator already loops waiting for a new peer, so
             * re-dialling restores the feed; exiting could never recover. */
            /* Do not close a descriptor while the video callback is part-way
             * through a framed write. A freshly connected socket commonly
             * reuses the same descriptor/handle; without this lock the tail of
             * the old access unit can land at byte zero of the new stream. */
            pthread_mutex_lock(&coop_send_lock);
            coop_link_failed = true;
            if (coop_fd >= 0) {
                close(coop_fd);
                coop_fd = -1;
            }
            pthread_mutex_unlock(&coop_send_lock);
            if (log != NULL) {
                fprintf(log, "link lost after %u forwarded -- reconnecting\n", coop_fwd);
                fflush(log);
            }
            /* DEAD-CYCLE WATCHDOG. "Reconnect rather than exit" recovers a transient
             * drop, but its only exit is "the coordinator is gone" -- and after the app
             * is closed the coordinator process LINGERS with its listener open, so the
             * re-dial always succeeds and this loop runs forever. Observed live: 123
             * link-loss cycles, forwarded stuck at 0, both processes surviving
             * `ares-launch --close`, and the still-open session cycling PC2's Apollo
             * (start -> pause -> virtual display removed) indefinitely. The coordinator
             * cannot exit while the child lives, so nothing breaks the tie.
             *
             * A LIVE secondary forwards frames. One that reconnects repeatedly without
             * forwarding any is doing nothing but holding a session open on the other
             * host, so give up and let the process die, which tears that session down.
             * Transient drops are unaffected: any forward progress resets the count. */
            if (coop_fwd == fwd_at_last_loss) {
                if (++dead_cycles >= COOP_DEAD_CYCLES) {
                    if (log != NULL) {
                        fprintf(log, "no frames forwarded across %d reconnects -- giving up\n",
                                dead_cycles);
                        fflush(log);
                    }
                    break;
                }
            } else {
                dead_cycles = 0;
            }
            fwd_at_last_loss = coop_fwd;
            intptr_t reconnected_fd = -1;
            for (int attempt = 0; attempt < 100 && reconnected_fd < 0; ++attempt) {
                usleep(200000);
                reconnected_fd = coop_ipc_connect(coop_mode_socket);
            }
            if (reconnected_fd < 0) {
                break;      /* coordinator really is gone */
            }
            if (!coop_ipc_set_nonblocking(reconnected_fd)) {
                close(reconnected_fd);
                break;
            }
            pthread_mutex_lock(&coop_send_lock);
            coop_fd = reconnected_fd;
            coop_link_failed = false;
            pthread_mutex_unlock(&coop_send_lock);
            if (log != NULL) { fprintf(log, "reconnected\n"); fflush(log); }
            continue;
        }
        usleep(200000);
        /* Report unconditionally on a slow tick: gating on a frame count meant
         * "no line" was ambiguous between "not forwarding" and "fewer than 300". */
        if (log != NULL && (++ticks % 10) == 0) {
            fprintf(log, "tick: forwarded=%u dropped=%u\n", coop_fwd, coop_drop);
            fflush(log);
        }
    }
    /* Tear the second session down PROPERLY instead of letting the process die with
     * the socket open -- but PAUSE this host's pane, never quit it from here.
     *
     * session_destroy() raises STREAMING_INTERRUPT_QUIT, and with `quitappafter` set
     * session_interrupt() turns that into an app quit. So a co-op DISCONNECT (or the TV
     * going to its Home screen) ended this host's Co-op Pane while the primary's only
     * paused, and there was no pair left to resume (seen 2026-10-09: PC2 ran its
     * session-end commands 4.5 s after pausing). Whether the panes end is the USER's
     * Disconnect/Quit choice, and the primary already carries it out: on Quit it quits
     * this host's pane itself (session_worker.c, "CO-OP: QUIT ENDS BOTH PANES").
     * The first interrupt wins, so this pins quitapp=false before session_destroy()
     * raises its own. */
    {
        app_t *a = (app_t *) app;
        if (a->session != NULL) {
            if (log != NULL) { fprintf(log, "destroying secondary session (pause; Quit is the primary's)\n"); fflush(log); }
            session_interrupt(a->session, false, STREAMING_INTERRUPT_USER);
            session_destroy(a->session);
            a->session = NULL;
        }
    }
    if (log != NULL) {
        fprintf(log, "=== secondary headless exit (forwarded=%u dropped=%u) ===\n",
                coop_fwd, coop_drop);
        fclose(log);
    }
    return 0;
}

unsigned coop_secondary_forwarded(void) { return coop_fwd; }
unsigned coop_secondary_dropped(void) { return coop_drop; }


static int coop_setup(int videoFormat, int width, int height, int redrawRate,
                      void *context, int drFlags) {
    (void) videoFormat; (void) width; (void) height; (void) redrawRate;
    (void) context; (void) drFlags;
    /* Nothing to set up: no decoder, no player, no surface. */
    return 0;
}

static void coop_cleanup(void) {
    /* Connection lifetime is owned by init/shutdown, not the session. */
}

static int coop_submit(PDECODE_UNIT decodeUnit) {
    if (coop_buf == NULL) {
        return DR_OK;
    }
    if ((size_t) decodeUnit->fullLength > coop_buf_cap) {
        size_t want = (size_t) decodeUnit->fullLength + (64u * 1024u);
        unsigned char *grown = realloc(coop_buf, want);
        if (grown == NULL) {
            coop_drop++;
            return DR_OK;
        }
        coop_buf = grown;
        coop_buf_cap = want;
    }
    /* Reassemble the access unit exactly as the primary path does. */
    size_t len = 0;
    for (PLENTRY entry = decodeUnit->bufferList; entry != NULL; entry = entry->next) {
        if (len + (size_t) entry->length > coop_buf_cap) {
            coop_drop++;
            return DR_OK;
        }
        memcpy(coop_buf + len, entry->data, (size_t) entry->length);
        len += (size_t) entry->length;
    }
    /* CARRY THE PEER'S OWN SOURCE IDENTITY AND ARRIVAL TIMES ACROSS THE LINK -- passover 07
     * §3.1. These are read from the DU here, at the only place they exist: moonlight frees the
     * decode unit after this callback returns, and the coordinator process never sees it at all.
     * Provenance is flagged rather than cleaned, because Limelight.h:151 makes a zero
     * host-processing latency ambiguous between "not supplied" and "repeated frame", and
     * passover 07 §3.1 requires substituted values be identified, not averaged in. */
    coop_ipc_meta_t meta = {
            .pts_ms = (uint32_t) decodeUnit->presentationTimeMs,
            .recv_ms = (uint32_t) decodeUnit->receiveTimeMs,
            .reasm_ms = (uint32_t) decodeUnit->enqueueTimeMs,
            .host_10us = decodeUnit->frameHostProcessingLatency,
            .flags = (uint8_t) ((decodeUnit->frameHostProcessingLatency == 0
                                         ? COOP_IPC_F_HOST_MS_ABSENT : 0u) |
                                (decodeUnit->presentationTimeMs == 0
                                         ? COOP_IPC_F_PTS_ZERO : 0u)),
    };
    pthread_mutex_lock(&coop_send_lock);
    if (coop_fd < 0 || coop_link_failed) {
        pthread_mutex_unlock(&coop_send_lock);
        return DR_OK;
    }
    const bool sent = coop_ipc_send_au(coop_fd, (uint32_t) decodeUnit->frameNumber,
                                       decodeUnit->frameType == FRAME_TYPE_IDR,
                                       coop_buf, len, &meta);
    if (sent && coop_start_state == COOP_SECONDARY_PENDING &&
        coop_ipc_send_ctl(coop_fd, COOP_IPC_CTL_READY)) {
        /* READY means more than "CreateProcess worked": one complete live frame
         * from the requested peer was forwarded successfully. */
        coop_start_state = COOP_SECONDARY_READY;
    }
    if (!sent) {
        /* Never append another frame after a partial timed-out write. shutdown()
         * wakes the control loop; that loop alone closes and replaces the
         * descriptor under this same lifetime lock. */
        coop_link_failed = true;
        shutdown(coop_fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&coop_send_lock);
    if (!sent) {
        /* Record WHY. At 4K the link kept dying with a handful of drops and the
         * cause was indistinguishable between a closed peer, a full buffer and an
         * oversized unit. Only the first few, so a broken link cannot flood. */
        if (coop_drop < 5) {
            int e = errno;
            FILE *f = fopen(AURORA_SECONDARY_LOG, "a");
            if (f != NULL) {
                fprintf(f, "send FAILED errno=%d (%s) len=%zu frame=%u idr=%d\n",
                        e, strerror(e), len, (unsigned) decodeUnit->frameNumber,
                        decodeUnit->frameType == FRAME_TYPE_IDR);
                fclose(f);
            }
        }
        coop_drop++;
        return DR_OK;   /* never tear the session down over a pane frame */
    }
    coop_fwd++;
    return DR_OK;
}

/* The secondary MUST NOT use the app's connection listener. Those callbacks
 * assume a UI, a player and input state that a headless process does not have:
 * connectionStatusUpdate -> streaming_notice_show (UI), setHdrMode ->
 * streaming_set_hdr (touches session->player, which is NULL here), rumble ->
 * app->input. With HDR on, setHdrMode fires a few seconds into every session --
 * which is exactly when the secondary was dying of SIGSEGV having forwarded
 * nothing. Everything here only logs; the rest is left NULL and moonlight-common-c
 * fills in its no-op fakes. */
static void coop_conn_log(const char *what, int a, int b) {
    FILE *f = fopen(AURORA_SECONDARY_LOG, "a");
    if (f != NULL) {
        fprintf(f, "conn: %s %d %d\n", what, a, b);
        fclose(f);
    }
}

static void coop_conn_terminated(int errorCode) {
    coop_conn_log("terminated errorCode", errorCode, 0);
    coop_secondary_report_start_failure(COOP_IPC_CTL_FAIL_CONNECTION);
}

static void coop_conn_stage_failed(int stage, int errorCode) {
    coop_conn_log("stage_failed", stage, errorCode);
    coop_secondary_report_start_failure(COOP_IPC_CTL_FAIL_CONNECTION);
}

static void coop_conn_status_update(int status) { coop_conn_log("status", status, 0); }

/* RUMBLE COMES BACK. The peer's game rumbles a pad this process does not own --
 * SDL lives in the coordinator -- so hand it over the input link instead of
 * dropping it. Without this a pad routed to the peer feels dead. */
static void coop_conn_rumble(unsigned short controller, unsigned short low, unsigned short high) {
    coop_input_secondary_rumble(controller, low, high);
}

static void coop_conn_rumble_triggers(unsigned short controller, unsigned short left,
                                      unsigned short right) {
    coop_input_secondary_rumble_triggers(controller, left, right);
}

static void coop_conn_log_message(const char *format, ...) {
    FILE *f = fopen(AURORA_SECONDARY_LOG, "a");
    if (f == NULL) {
        return;
    }

    va_list ap;
    va_start(ap, format);
    vfprintf(f, format, ap);
    va_end(ap);
    fclose(f);
}

CONNECTION_LISTENER_CALLBACKS coop_secondary_conn_callbacks = {
        .logMessage = coop_conn_log_message,
        .rumble = coop_conn_rumble,
        .rumbleTriggers = coop_conn_rumble_triggers,
        .stageFailed = coop_conn_stage_failed,
        .connectionTerminated = coop_conn_terminated,
        .connectionStatusUpdate = coop_conn_status_update,
};

DECODER_RENDERER_CALLBACKS coop_secondary_callbacks = {
        .setup = coop_setup,
        .cleanup = coop_cleanup,
        .submitDecodeUnit = coop_submit,
        .capabilities = CAPABILITY_DIRECT_SUBMIT,
};

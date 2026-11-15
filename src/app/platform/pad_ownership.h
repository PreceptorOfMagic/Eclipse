#pragma once

/**
 * CONTROLLER OWNERSHIP when this PC is one of the co-op hosts (docs/device-passthrough-design.md).
 *
 * A controller assigned to the OTHER PC must stop reaching this PC's own game, Steam and Game Bar,
 * while Eclipse keeps reading it to forward it. The platform layer does three things:
 *   hide    -- no program opened from now on can see the controller (Eclipse excepted);
 *   scan    -- which programs ALREADY hold it open (hiding cannot take a handle away);
 *   evict   -- close and reopen the ones we know how to, so they come back blind to it.
 * And undoes it all at session end, or at the next start after a crash.
 *
 * Windows: HidHide (signed driver) + a handle scan + close/reopen. Desktop Linux: the kernel's exclusive grab of
 * each evdev node (platform/linux/pad_ownership_linux.c -- hide and evict in one step, no admin; programs reading a
 * pad's hidraw node are reported, not cut off). Other platforms: not supported; the stubs below say so, so the
 * shared UI can tell the user instead of pretending.
 */

#include <stdbool.h>
#include <stddef.h>

#include <SDL.h>

#define PAD_OWN_MAX_PADS 8
#define PAD_OWN_MAX_HOLDERS 16

typedef enum pad_own_kind_t {
    PAD_OWN_KIND_HID = 0,      /**< an ordinary HID game controller (DS4, Switch Pro, ...) */
    PAD_OWN_KIND_XBOX,         /**< XInput HID (IG_ in its id): its device must NEVER be restarted while held */
    PAD_OWN_KIND_STEAM_SLOT,   /**< one Steam Controller puck slot (steam_triton.c) */
} pad_own_kind_t;

typedef struct pad_own_pad_t {
    char name[96];             /**< what the user calls it, for the popup */
    char instance[320];        /**< Windows device instance id, e.g. HID\VID_057E&PID_2009\... */
    pad_own_kind_t kind;
} pad_own_pad_t;

typedef struct pad_own_holder_t {
    unsigned long pid;
    char exe[64];              /**< "steam.exe" */
    char display[96];          /**< "Steam", or a service's display name */
    bool closable;             /**< we know how to close it and bring it back (or restart it, for a service) */
    bool admin;                /**< acting on it needs administrator rights: a service or an elevated program */
    char service[128];         /**< the Windows service to restart ("" = not a restartable service) */
} pad_own_holder_t;

typedef struct pad_own_scan_t {
    pad_own_holder_t holders[PAD_OWN_MAX_HOLDERS];
    int count;
    int unscanned;             /**< processes this process may not inspect (services, administrator programs) */
    bool elevated;             /**< the scan ran with administrator rights: services and admin programs are named */
    bool complete;             /**< false = the scan stopped early; the list may be short */
    int routes;                /**< third-party filter drivers on a hidden pad's stack with a control device of their
                                    own: programs read the pad through them, below HidHide (e.g. Steam's Xbox driver) */
    char route[4][96];         /**< their names, for the popup */
} pad_own_scan_t;

/** What pad_own_evict_admin() did. */
typedef enum pad_own_admin_result_t {
    PAD_OWN_ADMIN_DONE = 0,    /**< the admin step ran (its report says what it found and restarted) */
    PAD_OWN_ADMIN_DECLINED,    /**< the user said no to Windows' administrator prompt */
    PAD_OWN_ADMIN_FAILED,      /**< it could not run (the report says why) */
} pad_own_admin_result_t;

#if defined(_WIN32) || defined(ECLIPSE_PAD_OWN_LINUX)

/** Can this PC hide controllers at all? false + a user-facing reason (e.g. HidHide not installed). */
bool pad_own_supported(char *why, size_t why_len);

/** The physical device behind an open SDL controller. false = cannot be identified (cannot be hidden). */
bool pad_own_resolve(SDL_JoystickID id, const char *name, pad_own_pad_t *out);

/** Hide @p pads from everything but Eclipse, until pad_own_session_end(). Idempotent. */
bool pad_own_hide(const pad_own_pad_t *pads, int count, char *err, size_t err_len);

/** The pads the last pad_own_hide() was given, kept after the session ends (a resume hides them again).
 *  @return how many were copied to @p out (at most @p max). */
int pad_own_last_hidden(pad_own_pad_t *out, int max);

/** Which programs already hold the hidden pads. Slow (up to ~1-2 s): call it off the UI thread. */
void pad_own_scan(pad_own_scan_t *out);

/** Close and reopen every closable holder in @p scan that needs no administrator rights. Slow (Steam takes
 *  seconds): off the UI thread. @return how many were brought back; remembered, so session end can refresh them. */
int pad_own_evict(const pad_own_scan_t *scan, char *report, size_t report_len);

/** Does this Eclipse run with administrator rights (no prompt needed)? */
bool pad_own_elevated(void);

/** Would the admin step have anything to do: services/admin programs this process could not inspect, or
 *  admin holders the scan named? */
bool pad_own_admin_needed(const pad_own_scan_t *scan);

/** THE ADMIN STEP: find the services and administrator programs holding the hidden pads, and restart the
 *  services that can be (the only service in their process, and accepting a stop). Elevated: done here.
 *  Not elevated: a short-lived elevated copy of Eclipse does it -- Windows shows its administrator prompt --
 *  and stays until session end to restart them again, so this PC gets the pads back. Slow: off the UI thread. */
pad_own_admin_result_t pad_own_evict_admin(char *report, size_t report_len);

/** The elevated copy's entry point: true when @p argv asks for it (it has then run and @p exit_code is set). */
bool pad_own_admin_main(int argc, char *argv[], int *exit_code);

/** A LOCAL-PANE SESSION'S ADMIN (user 2026-11-12: always ask, saying why, for a session where this PC is both
 *  host and client): admin prompts appear on the normal desktop -- in this PC's pane -- for the session, and the
 *  elevated copy stays to do the controller step later without a second prompt. Elevated: done here. Ends with
 *  pad_own_session_end(). Blocks while Windows' prompt is up: off the UI thread. */
pad_own_admin_result_t pad_own_session_admin_begin(char *report, size_t report_len);

/** Can the admin step run without a prompt (this Eclipse is elevated, or a session's elevated copy runs)? */
bool pad_own_admin_ready(void);

/** Is anything hidden right now (by this process)? */
bool pad_own_active(void);

/** Undo everything: reveal the pads, restore HidHide's previous state, and let the apps that were
 *  reopened see the pads again. Safe to call any time. Slow when it reopens Steam: worker thread. */
void pad_own_session_end(void);

/** At startup: undo what a previous Eclipse left hidden if it crashed or was killed. */
void pad_own_startup_recover(void);

#else

static inline bool pad_own_supported(char *why, size_t why_len) {
    if (why != NULL && why_len > 0) {
        SDL_strlcpy(why, "Keeping a controller away from this PC is not available on this system yet.", why_len);
    }
    return false;
}
static inline bool pad_own_resolve(SDL_JoystickID id, const char *name, pad_own_pad_t *out) {
    (void) id; (void) name; (void) out;
    return false;
}
static inline bool pad_own_hide(const pad_own_pad_t *pads, int count, char *err, size_t err_len) {
    (void) pads; (void) count; (void) err; (void) err_len;
    return false;
}
static inline int pad_own_last_hidden(pad_own_pad_t *out, int max) {
    (void) out; (void) max;
    return 0;
}
static inline void pad_own_scan(pad_own_scan_t *out) {
    SDL_zerop(out);
}
static inline int pad_own_evict(const pad_own_scan_t *scan, char *report, size_t report_len) {
    (void) scan; (void) report; (void) report_len;
    return 0;
}
static inline bool pad_own_elevated(void) { return false; }
static inline bool pad_own_admin_needed(const pad_own_scan_t *scan) {
    (void) scan;
    return false;
}
static inline pad_own_admin_result_t pad_own_evict_admin(char *report, size_t report_len) {
    (void) report; (void) report_len;
    return PAD_OWN_ADMIN_FAILED;
}
static inline bool pad_own_admin_main(int argc, char *argv[], int *exit_code) {
    (void) argc; (void) argv; (void) exit_code;
    return false;
}
static inline pad_own_admin_result_t pad_own_session_admin_begin(char *report, size_t report_len) {
    (void) report; (void) report_len;
    return PAD_OWN_ADMIN_FAILED;
}
static inline bool pad_own_admin_ready(void) { return false; }
static inline bool pad_own_active(void) { return false; }
static inline void pad_own_session_end(void) {}
static inline void pad_own_startup_recover(void) {}

#endif

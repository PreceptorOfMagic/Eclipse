/*
 * Co-op split-screen: pointer routing. See coop_pointer.h for the design and the
 * rules this path obeys.
 */
#include "coop_pointer.h"

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include <Limelight.h>

#include "coop_input.h"
/* Relative, so tests/coop/test_coop_pointer.c still builds this file with no src/app include path. */
#include "../../platform/local_input.h"

enum { PANE_PRIMARY = 0, PANE_PEER = 1 };

/* A macro ONLY so tests/coop/test_coop_pointer.c can point the module at a scratch
 * file: writing a real mode selector to check the geometry would be arming a mode as
 * a side effect of a unit test, which is exactly the trap docs/coop-modes.md is about. */
#ifndef COOP_PTR_VMIRROR_SENTINEL
#define COOP_PTR_VMIRROR_SENTINEL "/tmp/coop-vmirror"
#endif

/** Latched at coordinator start so the pointer and the composite cannot disagree. */
static bool split_active = false;
/** True when the divider is a VERTICAL line, i.e. the panes are left | right. */
static bool split_vertical = false;

static int cur_pane = PANE_PRIMARY;
/* PANE ORDER, read from the SAME sentinel the composition reads (/tmp/coop-peer-top,
 * session_video.c). When set, the PEER's pane is the FIRST half (top/left) and ours is the
 * second. Pointer routing has to follow the picture or the cursor is handed to the wrong PC:
 * position selects a pane by which half of the screen it is in, so if the halves swap and this
 * does not, every crossing is inverted. */
static bool peer_first = false;
/** Buttons currently down, one bit per Limelight BUTTON_*. Non-zero pins the pane. */
static unsigned held = 0;
static unsigned crossings = 0;
/** One line the first time the pointer reaches each side: enough to prove the routing
 *  engaged, bounded so the input thread never pays for a file write per crossing. */
static bool logged_peer = false, logged_primary = false;
/** The pane whose host is this machine (coop_pointer_set_local_pane), or -1: requested before
 *  the session, latched at start so it cannot change under a live split. */
static int local_pane_request = -1;
static int local_pane = -1;
/** An ASSIGNED mouse / keyboard (coop_pointer_set_pins), or -1 when it follows the pointer. */
static int mouse_pin_request = -1, keys_pin_request = -1;
static int mouse_pin = -1, keys_pin = -1;
/** The last pane-local position: where a click hands a taken-back assigned mouse over again. */
static int last_lx = 0, last_ly = 0, last_lw = 1, last_lh = 1;
/** High-resolution wheel steps not yet sent to the peer as a whole notch (coop_pointer_scroll_high_res). */
static int peer_scroll_hires = 0;

static void coop_pointer_log(const char *fmt, ...) {
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

/*
 * WHERE THE LINE BETWEEN THE TWO PCs ACTUALLY IS.
 *
 * The midpoint, and it is derived rather than assumed: session.c asks BOTH hosts for
 * exactly half of the same configured stream size -- half the width under
 * `/tmp/coop-vmirror` (session.c:420), half the height otherwise (session.c:429) --
 * and refuses to compose panes that disagree, so the two are equal by construction.
 * The composite is then stretched across the whole video area (the webOS sink is
 * handed the full display rect, ndl_video.c SetDisplayArea), which keeps the seam at
 * the midpoint of the panel even in horizontal, where 2176 coded rows are shown on
 * 2160.
 *
 * If the split ever stops being even, this is the one line to change.
 */
static int divider(int span) {
    return span / 2;
}

static int clamp_to(int v, int hi) {
    if (v < 0) {
        return 0;
    }
    return v > hi ? hi : v;
}

/** Re-express a window position as a position inside @p pane, against that pane's own
 *  reference size -- which is what makes the host put its cursor in the matching spot. */
static void map_into_pane(int pane, int x, int y, int ref_w, int ref_h,
                          int *out_x, int *out_y, int *out_w, int *out_h) {
    int w = ref_w, h = ref_h, lx = x, ly = y;
    /* Is THIS pane the second half? The second half is the one that needs its origin shifted
     * back to zero for the host. Which pane that is flips with `peer_first`. */
    const bool second_half = peer_first ? (pane == PANE_PRIMARY) : (pane == PANE_PEER);
    if (split_vertical) {
        const int split = divider(ref_w);
        if (second_half) {
            w = ref_w - split;
            lx = x - split;
        } else {
            w = split;
        }
    } else {
        const int split = divider(ref_h);
        if (second_half) {
            h = ref_h - split;
            ly = y - split;
        } else {
            h = split;
        }
    }
    *out_w = w < 1 ? 1 : w;
    *out_h = h < 1 ? 1 : h;
    /* Only reachable while a held button pins the pane; an unpinned position is
     * already inside the pane it selected. */
    *out_x = clamp_to(lx, *out_w - 1);
    *out_y = clamp_to(ly, *out_h - 1);
}

void coop_pointer_start(void) {
    /* THE SAME SENTINEL THE COMPOSITION READS (session_video.c:2516). Reading the
     * setting instead would let a test run -- which writes the sentinels directly and
     * is expected to override the UI -- compose one way and point the other. */
    split_vertical = access(COOP_PTR_VMIRROR_SENTINEL, F_OK) == 0;
    peer_first = false;
    {
        FILE *pt = fopen("/tmp/coop-peer-top", "r");
        if (pt != NULL) {
            int v = 0;
            if (fscanf(pt, "%d", &v) == 1 && v != 0) { peer_first = true; }
            fclose(pt);
        }
    }
    cur_pane = PANE_PRIMARY;
    held = 0;
    crossings = 0;
    peer_scroll_hires = 0;
    logged_peer = logged_primary = false;
    local_pane = local_pane_request;
    mouse_pin = mouse_pin_request;
    keys_pin = keys_pin_request;
    split_active = true;
    coop_pointer_log("[COOP-PTR] pointer split armed: %s divider, primary pane %s (peer_first=%d), this PC's pane %s",
                     split_vertical ? "vertical" : "horizontal",
                     peer_first ? (split_vertical ? "RIGHT" : "BOTTOM")
                                : (split_vertical ? "LEFT" : "TOP"),
                     peer_first ? 1 : 0,
                     local_pane == PANE_PRIMARY ? "PRIMARY" : (local_pane == PANE_PEER ? "PEER" : "none"));
    if (mouse_pin >= 0 || keys_pin >= 0) {
        coop_pointer_log("[COOP-PTR] assigned: mouse %s, keyboard %s",
                         mouse_pin == PANE_PRIMARY ? "PRIMARY" : (mouse_pin == PANE_PEER ? "PEER" : "follows the pointer"),
                         keys_pin == PANE_PRIMARY ? "PRIMARY" : (keys_pin == PANE_PEER ? "PEER" : "follows the pointer"));
    }
    if (local_pane >= 0) {
        /* The second half is the one map_into_pane shifts; which pane that is flips with peer_first. */
        const bool local_second = peer_first ? (local_pane == PANE_PRIMARY) : (local_pane == PANE_PEER);
        const int keys = keys_pin < 0 ? LOCAL_INPUT_KEYS_FOLLOW
                                      : (keys_pin == local_pane ? LOCAL_INPUT_KEYS_HERE : LOCAL_INPUT_KEYS_ELSEWHERE);
        local_input_arm(split_vertical, !local_second, mouse_pin == local_pane, keys);
    }
}

void coop_pointer_set_local_pane(int pane) {
    local_pane_request = (pane == PANE_PRIMARY || pane == PANE_PEER) ? pane : -1;
}

void coop_pointer_set_pins(int mouse_pane, int keys_pane) {
    mouse_pin_request = (mouse_pane == PANE_PRIMARY || mouse_pane == PANE_PEER) ? mouse_pane : -1;
    keys_pin_request = (keys_pane == PANE_PRIMARY || keys_pane == PANE_PEER) ? keys_pane : -1;
}

void coop_pointer_stop(void) {
    if (split_active) {
        coop_pointer_log("[COOP-PTR] pointer split off (crossings=%u)", crossings);
    }
    split_active = false;
    cur_pane = PANE_PRIMARY;
    held = 0;
    if (local_pane >= 0) {
        local_input_disarm();
    }
    local_pane = -1;
}

bool coop_pointer_active(void) { return split_active; }

unsigned coop_pointer_crossings(void) { return crossings; }

bool coop_pointer_split_vertical(void) { return split_vertical; }

void coop_pointer_pane_centre(int ref_w, int ref_h, int *x, int *y) {
    *x = ref_w / 2;
    *y = ref_h / 2;
    if (!split_active) {
        return;
    }
    /* The same halves map_into_pane uses: the second one is the peer's unless peer_first. */
    const bool second_half = peer_first ? (cur_pane == PANE_PRIMARY) : (cur_pane == PANE_PEER);
    if (split_vertical) {
        const int split = divider(ref_w);
        *x = second_half ? split + (ref_w - split) / 2 : split / 2;
    } else {
        const int split = divider(ref_h);
        *y = second_half ? split + (ref_h - split) / 2 : split / 2;
    }
}

static void send_position(int pane, int x, int y, int w, int h) {
    if (pane == PANE_PEER) {
        coop_input_send_mouse_position(x, y, w, h);
    } else {
        LiSendMousePositionEvent((short) x, (short) y, (short) w, (short) h);
    }
}

static void send_scroll_local(int vertical, int horizontal) {
    if (vertical != 0) {
        LiSendScrollEvent((signed char) vertical);
    }
    if (horizontal != 0) {
        LiSendHScrollEvent((signed char) horizontal);
    }
}

/* NEVER ROUTE INPUT TO A PEER THAT IS NOT THERE.
 *
 * coord_send() drops silently when no secondary is attached, which is the right trade
 * mid-session -- a lost button beats a stalled input thread -- but it is the worst
 * possible behaviour when there is no peer at all: the events go nowhere and the user has
 * no control, with nothing on screen to say why.
 *
 * That is not hypothetical. Until 2026-08-12 co-op mode leaked (see launcher_launch_game),
 * so an ordinary single-host stream came up with this split armed. Everything in the
 * bottom half of the screen was addressed to a secondary process that did not exist, and
 * because cur_pane only changes when the divider is crossed, once the cursor drifted low
 * EVERY click vanished until it crossed back up -- reported as "wasn't able to do any
 * inputs until I switched to the other pc and back" (switching re-armed the split, which
 * resets cur_pane to PRIMARY).
 *
 * The leak is fixed, but the failure mode was silent, and silence is what made it hard.
 * So the split now degrades to the local host whenever the peer link is not actually up.
 * During the brief window before a real secondary attaches this sends the peer's pointer
 * to the primary instead, which is visibly wrong for a moment and self-corrects -- far
 * better than input that does nothing. */
static bool peer_reachable(void) {
    return coop_input_peer_attached();
}

void coop_pointer_position(int x, int y, int ref_w, int ref_h) {
    if (!split_active || ref_w <= 1 || ref_h <= 1 || !peer_reachable()) {
        LiSendMousePositionEvent((short) x, (short) y, (short) ref_w, (short) ref_h);
        return;
    }
    if (local_pane >= 0 && !local_input_pointer_is_real()) {
        /* The real cursor is on this PC's virtual display: this is SDL's synthetic "left the
         * window" motion, clamped to the window's corner -- sent on, it parked the other PC's
         * cursor in its bottom-right corner at every hand-over (issue #35, 2026-09-27). */
        return;
    }

    int pane = cur_pane;
    if (mouse_pin >= 0) {
        /* ASSIGNED to one PC: it never crosses, and map_into_pane clamps it into that pane. */
        pane = mouse_pin;
    } else if (held == 0) {
        /* Straight geometry, evaluated on every event: there is no hysteresis and no
         * dwell time, so the switch happens on the first sample past the line. */
        const int coord = split_vertical ? x : y;
        const int split = divider(split_vertical ? ref_w : ref_h);
        pane = (coord >= split) ? (peer_first ? PANE_PRIMARY : PANE_PEER)
                                : (peer_first ? PANE_PEER : PANE_PRIMARY);
    }

    if (pane != cur_pane) {
        cur_pane = pane;
        crossings++;
        if (pane == PANE_PEER && !logged_peer) {
            logged_peer = true;
            coop_pointer_log("[COOP-PTR] crossed to the PEER's pane");
        } else if (pane == PANE_PRIMARY && !logged_primary) {
            logged_primary = true;
            coop_pointer_log("[COOP-PTR] crossed back to the PRIMARY's pane");
        }
    }

    int lx, ly, lw, lh;
    map_into_pane(pane, x, y, ref_w, ref_h, &lx, &ly, &lw, &lh);
    last_lx = lx;
    last_ly = ly;
    last_lw = lw;
    last_lh = lh;
    if (pane == local_pane) {
        /* THIS PC's pane: anything sent would loop back through this desktop. The real cursor
         * goes over to the host's virtual display instead, and the game reads it natively. */
        local_input_hand_off(lx, ly, lw, lh);
        return;
    }
    local_input_pointer_seen();
    send_position(pane, lx, ly, lw, lh);
}

void coop_pointer_button(int action, int button) {
    if (!split_active || !peer_reachable()) {
        LiSendMouseButtonEvent((char) action, button);
        return;
    }
    if (cur_pane == local_pane) {
        /* THIS PC's pane: never sent (it would loop back through this desktop). A press here
         * means an assigned mouse was taken back with Ctrl+Alt+Shift+Z: hand it over again. */
        if (action == BUTTON_ACTION_PRESS) {
            local_input_click(last_lx, last_ly, last_lw, last_lh);
        }
        return;
    }
    /* Routed BEFORE the held set is updated, so a release always reaches the pane the
     * press pinned rather than whichever pane the pointer has since moved over. */
    if (cur_pane == PANE_PEER) {
        coop_input_send_mouse_button(action, button);
    } else {
        LiSendMouseButtonEvent((char) action, button);
    }
    if (button > 0 && button < 32) {
        if (action == BUTTON_ACTION_PRESS) {
            held |= 1u << button;
        } else {
            held &= ~(1u << button);
        }
    }
}

bool coop_pointer_keys_to_peer(void) {
    if (!split_active || !peer_reachable()) {
        return false;
    }
    /* An ASSIGNED keyboard goes to its pane wherever the pointer is. */
    return (keys_pin >= 0 ? keys_pin : cur_pane) == PANE_PEER;
}

void coop_pointer_scroll_high_res(int amount) {
    if (!split_active || cur_pane != PANE_PEER || !peer_reachable()) {
        peer_scroll_hires = 0;
        if (amount != 0) {
            LiSendHighResScrollEvent((short) amount);
        }
        return;
    }
    peer_scroll_hires += amount;
    const int notches = peer_scroll_hires / 120;
    if (notches != 0) {
        peer_scroll_hires -= notches * 120;
        coop_input_send_mouse_scroll(notches, 0);
    }
}

void coop_pointer_scroll(int vertical, int horizontal) {
    if (!split_active || cur_pane != PANE_PEER || !peer_reachable()) {
        send_scroll_local(vertical, horizontal);
        return;
    }
    coop_input_send_mouse_scroll(vertical, horizontal);
}

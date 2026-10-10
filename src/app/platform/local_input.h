#pragma once

#include <stdbool.h>

/**
 * THIS PC AS A CO-OP HOST: THE MOUSE AND KEYBOARD HAND-OVER (Windows; the design and its
 * reasons are in platform/windows/local_input_win.c). Input for a host that is this very
 * machine cannot go through the stream -- it would loop back through this desktop, which is
 * why local_host_win.c blocks it -- so when the pointer enters that host's pane the REAL
 * cursor is moved onto the host's virtual display and the window there gets the keyboard;
 * the game reads both natively. Elsewhere the client is never a host and every call is a no-op.
 *
 * All calls are made on the UI thread, where coop_pointer.c runs.
 */
/** Where the keyboard goes while this PC is one of the hosts (the co-op setting "assign mouse and
 *  keyboard like controllers", issue #37). */
enum {
    LOCAL_INPUT_KEYS_FOLLOW = 0,     /**< with the pointer: this PC's game while the cursor is there */
    LOCAL_INPUT_KEYS_HERE = 1,       /**< assigned to this PC: its game keeps the keyboard */
    LOCAL_INPUT_KEYS_ELSEWHERE = 2,  /**< assigned to the other PC: the client keeps the keyboard */
};

#ifdef _WIN32
/** A co-op session in which one pane is this PC. @p vertical: the panes are side by side;
 *  @p local_first: this PC's pane is the left (vertical) or top (horizontal) one;
 *  @p mouse_here: the mouse is ASSIGNED to this PC, so the divider does not bring it back (only
 *  Ctrl+Alt+Shift+Z does); @p keys: a LOCAL_INPUT_KEYS_* value. */
void local_input_arm(bool vertical, bool local_first, bool mouse_here, int keys);

/** The session is over: the cursor comes back if it is still away, and everything is undone. */
void local_input_disarm(void);

/** The pointer is in this PC's pane at pane-local (@p x, @p y) of a @p w x @p h pane: put the
 *  real cursor on the same spot of the virtual display and give that window the keyboard.
 *  Does nothing while the cursor is already there. @return true when the cursor is there. */
bool local_input_hand_off(int x, int y, int w, int h);

/** True while the real cursor is on this PC's virtual display. */
bool local_input_handed_off(void);

/** A pointer event reached the client window outside this PC's pane: if the cursor has just
 *  come back and the keyboard has not, take the keyboard back now. */
void local_input_pointer_seen(void);

/** A mouse button went down over this PC's pane on the client window -- which only happens after
 *  Ctrl+Alt+Shift+Z took an ASSIGNED mouse back: hand it over again at pane-local (@p x, @p y). */
void local_input_click(int x, int y, int w, int h);

/** The client window got the focus: with the keyboard assigned to this PC, give it back to the game. */
void local_input_focus_gained(void);

/** Is the pointer event being handled a REAL one -- is the real cursor over the client window?
 *  SDL synthesizes motions when the cursor leaves the window, clamped to its corner. */
bool local_input_pointer_is_real(void);
#else
static inline void local_input_arm(bool vertical, bool local_first, bool mouse_here, int keys) {
    (void) vertical;
    (void) local_first;
    (void) mouse_here;
    (void) keys;
}

static inline void local_input_disarm(void) {}

static inline bool local_input_hand_off(int x, int y, int w, int h) {
    (void) x;
    (void) y;
    (void) w;
    (void) h;
    return false;
}

static inline bool local_input_handed_off(void) {
    return false;
}

static inline void local_input_pointer_seen(void) {}

static inline void local_input_click(int x, int y, int w, int h) {
    (void) x;
    (void) y;
    (void) w;
    (void) h;
}

static inline void local_input_focus_gained(void) {}

static inline bool local_input_pointer_is_real(void) {
    return true;
}
#endif

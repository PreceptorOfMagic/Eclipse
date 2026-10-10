#include "session_pointer_gesture.h"

#include "app.h"
#include "stream/input/coop_pointer.h"
#include "stream/input/session_input.h"
#include "stream/session_priv.h"

#include <Limelight.h>
#include <SDL.h>

#define GESTURE_LONG_PRESS_MS 1000
#define GESTURE_DRAG_SLOP_PX  10

static bool pointer_gesture_available(const stream_input_t *input) {
    return !input->view_only;
}

/* WHICH DEVICE THE GESTURE IS FOR, AND WHY THE PEN/TOUCH FLAG IS NOT CHECKED HERE ALONE.
 *
 * This used to reject EVERY pointer whenever the host advertised LI_FF_PEN_TOUCH_EVENTS, so on
 * a host that takes pen/touch natively -- which Apollo does -- long-press-to-right-click was
 * dead for the LG MAGIC REMOTE too, and the remote is not a touch device at all. Nothing logged
 * it; the feature simply never fired. That is the whole reason the user believed it did not
 * exist.
 *
 * The rest of the input path already states the rule correctly: session_mouse.c suppresses
 * events only when `which == SDL_TOUCH_MOUSEID` AND the host takes pen/touch, i.e. "don't send
 * mouse events FROM TOUCH DEVICES if the host handles them natively". A real touch is forwarded
 * by that path and the gesture would double up; an absolute pointer is not, so it keeps the
 * gesture. Same test, applied per event instead of per session. */
static bool pointer_gesture_eligible(const stream_input_t *input, int which) {
    (void) input;
    if (which == SDL_TOUCH_MOUSEID) {
        return (LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) == 0;
    }
#if defined(TARGET_WEBOS)
    /* The magic remote is an ABSOLUTE pointer; relative mode means a captured mouse instead. */
    if (!app_get_mouse_relative()) {
        return true;
    }
#else
    (void) which;
#endif
    return false;
}

static void pointer_gesture_send_position(const stream_input_t *input, int x, int y) {
    coop_pointer_position(x, y, input->session->display_width,
                          input->session->display_height);
}

static void pointer_gesture_begin_drag(stream_input_t *input, int x, int y) {
    input->pointerGestureDragging = true;
    pointer_gesture_send_position(input, x, y);
    coop_pointer_button(BUTTON_ACTION_PRESS, BUTTON_LEFT);
    input->pointerGestureLeftDown = true;
}

static int pointer_gesture_distance_sq(int x0, int y0, int x1, int y1) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    return dx * dx + dy * dy;
}

void pointer_gesture_reset(stream_input_t *input) {
    if (input->started && input->pointerGestureLeftDown) {
        coop_pointer_button(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
    }
    input->pointerGestureActive = false;
    input->pointerGestureDragging = false;
    input->pointerGestureLeftDown = false;
    input->pointerGesturePressTime = 0;
    input->pointerGestureStartX = 0;
    input->pointerGestureStartY = 0;
}

bool pointer_gesture_handle_mbutton(stream_input_t *input, const SDL_MouseButtonEvent *event) {
    if (!pointer_gesture_available(input)) {
        return false;
    }

    if (event->button != SDL_BUTTON_LEFT) {
        if (event->state == SDL_PRESSED && input->pointerGestureActive) {
            pointer_gesture_reset(input);
        }
        return false;
    }

    if (!pointer_gesture_eligible(input, event->which)) {
        if (input->pointerGestureActive) {
            pointer_gesture_reset(input);
        }
        return false;
    }

    if (event->state == SDL_PRESSED) {
        input->pointerGestureActive = true;
        input->pointerGestureDragging = false;
        input->pointerGestureLeftDown = false;
        input->pointerGesturePressTime = event->timestamp != 0 ? event->timestamp : SDL_GetTicks();
        input->pointerGestureStartX = event->x;
        input->pointerGestureStartY = event->y;
        return true;
    }

    if (!input->pointerGestureActive) {
        return false;
    }

    uint32_t releasedAt = event->timestamp != 0 ? event->timestamp : SDL_GetTicks();
    uint32_t heldMs = releasedAt - input->pointerGesturePressTime;

    pointer_gesture_send_position(input, event->x, event->y);

    /* SAY WHICH BRANCH RAN. This gesture was disabled by a host feature flag for an unknown
     * length of time and nothing anywhere recorded that it had not fired -- one line per click
     * is a cheap price for never having to guess again. */
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[PTR-GESTURE] %s after %ums (which=%d)",
                input->pointerGestureDragging ? "drag-end"
                                              : (heldMs >= GESTURE_LONG_PRESS_MS ? "RIGHT-CLICK"
                                                                                 : "left-click"),
                (unsigned) heldMs, (int) event->which);

    if (input->pointerGestureDragging) {
        if (input->pointerGestureLeftDown) {
            coop_pointer_button(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
        }
    } else if (heldMs >= GESTURE_LONG_PRESS_MS) {
        coop_pointer_button(BUTTON_ACTION_PRESS, BUTTON_RIGHT);
        coop_pointer_button(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
    } else {
        coop_pointer_button(BUTTON_ACTION_PRESS, BUTTON_LEFT);
        coop_pointer_button(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
    }

    input->pointerGestureActive = false;
    input->pointerGestureDragging = false;
    input->pointerGestureLeftDown = false;
    input->pointerGesturePressTime = 0;
    return true;
}

bool pointer_gesture_handle_mmotion(stream_input_t *input, const SDL_MouseMotionEvent *event) {
    if (!input->pointerGestureActive || input->view_only) {
        return false;
    }
    /* The press already passed pointer_gesture_eligible(), so the device is one the gesture
     * owns; re-testing the host flag here would abandon a gesture mid-way on exactly the
     * hosts where it was just accepted. */

    if (!input->pointerGestureDragging) {
        int slopSq = GESTURE_DRAG_SLOP_PX * GESTURE_DRAG_SLOP_PX;
        if (pointer_gesture_distance_sq(input->pointerGestureStartX, input->pointerGestureStartY,
                                        event->x, event->y) >= slopSq) {
            pointer_gesture_begin_drag(input, event->x, event->y);
            return true;
        }
        return false;
    }

    if (app_get_mouse_relative() && event->which != SDL_TOUCH_MOUSEID) {
        LiSendMouseMoveEvent((short) event->xrel, (short) event->yrel);
    } else {
        pointer_gesture_send_position(input, event->x, event->y);
    }
    return true;
}

#include "stream/input/session_input.h"

#include "app.h"

#include "stream/input/coop_pointer.h"
#include "stream/input/session_pointer_gesture.h"
#include "stream/session.h"
#include "stream/session_priv.h"

#include <Limelight.h>
#include <SDL.h>

#if !defined(TARGET_WEBOS)
/* The click that took the mouse back after Ctrl+Alt+Shift+Z: its release must not reach the host either. */
static bool swallow_left_release = false;
#endif

void stream_input_handle_mbutton(stream_input_t *input, const SDL_MouseButtonEvent *event) {
#if !defined(TARGET_WEBOS)
    if (input->input->capture_released) {
        /* Released to the desktop: nothing goes to the host, and a left click in the stream takes the mouse
         * back without being sent (moonlight-qt does the same). */
        if (event->state == SDL_PRESSED && event->button == SDL_BUTTON_LEFT) {
            app_input_set_capture(input->input, true);
            swallow_left_release = true;
        }
        return;
    }
    if (swallow_left_release && event->state == SDL_RELEASED && event->button == SDL_BUTTON_LEFT) {
        swallow_left_release = false;
        return;
    }
#endif
    if (pointer_gesture_handle_mbutton(input, event)) {
        return;
    }

    int button;
    switch (event->button) {
        case SDL_BUTTON_LEFT:
            button = BUTTON_LEFT;
            break;
        case SDL_BUTTON_MIDDLE:
            button = BUTTON_MIDDLE;
            break;
        case SDL_BUTTON_RIGHT:
            button = BUTTON_RIGHT;
            break;
        case SDL_BUTTON_X1:
            button = BUTTON_X1;
            break;
        case SDL_BUTTON_X2:
            button = BUTTON_X2;
            break;
        default:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Unhandled button event: %d",
                        event->button);
            return;
    }
    if (event->which == SDL_TOUCH_MOUSEID) {
        if (LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) {
            // Don't send mouse events from touch devices if the host supports pen/touch events
            return;
        }
        coop_pointer_position(event->x, event->y, input->session->display_width,
                              input->session->display_height);
    }
    coop_pointer_button(event->state == SDL_PRESSED ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE,
                        button);
}

void stream_input_handle_mwheel(stream_input_t *input, const SDL_MouseWheelEvent *event) {
#if !defined(TARGET_WEBOS)
    if (input->input->capture_released) {
        return;
    }
#else
    (void) input;
#endif
    if (event->which == SDL_TOUCH_MOUSEID && LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) {
        // Don't send mouse events from touch devices if the host supports pen/touch events
        return;
    }
    coop_pointer_scroll(event->y, event->x);
}

void stream_input_handle_mmotion(stream_input_t *input, const SDL_MouseMotionEvent *event, bool hw_mouse) {
    if (input->view_only) {
        return;
    }
#if !defined(TARGET_WEBOS)
    if (input->input->capture_released) {
        return;
    }
#endif
    if (event->which == SDL_TOUCH_MOUSEID && LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) {
        // Don't send mouse events from touch devices if the host supports pen/touch events
        return;
    }
    if (pointer_gesture_handle_mmotion(input, event)) {
        return;
    }
    if (input->no_sdl_mouse) {
        if (!hw_mouse) {
            return;
        }
        LiSendMouseMoveEvent((short) event->xrel, (short) event->yrel);
    } else if (app_get_mouse_relative() && event->which != SDL_TOUCH_MOUSEID) {
        LiSendMouseMoveEvent((short) event->xrel, (short) event->yrel);
    } else {
        /* THE MAGIC REMOTE LANDS HERE: an absolute position, which is the only kind of
         * event that can say which PC on the split screen it is aiming at. */
        coop_pointer_position(event->x, event->y, input->session->display_width,
                              input->session->display_height);
    }
}

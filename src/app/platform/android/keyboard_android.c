/*
 * Android TV remote keys during a stream: the Android counterpart of platform/webos/keyboard_webos.c, with
 * the same meanings wherever the remotes share a key.
 *
 *   Back            closes the on-screen keyboard if it is open; otherwise a tap is Escape on the host and
 *                   a hold opens the stream overlay (session_keyboard.c, stream_input_handle_remote_back)
 *   d-pad centre    Enter on the host; held, a right click (session_keyboard.c, as the webOS OK key)
 *   red / info /    open the stream overlay (webOS: RED and EXIT). Android TV remotes have no EXIT key and
 *   guide / menu*   many have no MENU; EclipseActivity forwards MENU here only from a remote, never from a
 *                   full keyboard, whose Menu key stays the host's Apps key
 *   blue            opens the on-screen keyboard (webOS: BLUE)
 *   channel +/-     Page Up / Page Down on the host (webOS: CH+/CH-)
 *   green / yellow  nothing in a stream (as on webOS)
 */
#include "android_platform.h"

#include "app.h"
#include "ui/root.h"
#include "ui/streaming/streaming.controller.h"
#include "stream/input/session_input.h"
#include "stream/input/vk.h"
#include "stream/session.h"
#include "stream/session_priv.h"

#include "util/bus.h"
#include "util/user_event.h"

#include <SDL.h>

bool stream_input_android_intercept_remote_keys(stream_input_t *input, const SDL_KeyboardEvent *event,
                                                short *keyCode) {
    session_t *session = input->session;
    switch ((unsigned int) event->keysym.scancode) {
        case ANDROID_SCANCODE_OVERLAY:
        case ANDROID_SCANCODE_MENU:
        case ANDROID_SCANCODE_RED:
            if (event->state == SDL_PRESSED) {
                bus_pushevent(USER_OPEN_OVERLAY, NULL, NULL);
            }
            return true;
        case SDL_SCANCODE_AC_BACK:
            if (streaming_soft_keyboard_shown()) {
                bus_pushevent(USER_CLOSE_SOFT_KEYBOARD, NULL, NULL);
                return true;
            }
            return false;
        case ANDROID_SCANCODE_CH_UP:
            if (session == NULL || ui_should_block_input()) {
                return true;
            }
            *keyCode = VK_PRIOR /* SDL_SCANCODE_PAGEUP */;
            return false;
        case ANDROID_SCANCODE_CH_DOWN:
            if (session == NULL || ui_should_block_input()) {
                return true;
            }
            *keyCode = VK_NEXT /* SDL_SCANCODE_PAGEDOWN */;
            return false;
        case ANDROID_SCANCODE_BLUE:
            if (input->view_only) {
                return true;
            }
            if (event->state == SDL_PRESSED) {
                bus_pushevent(USER_OPEN_SOFT_KEYBOARD, NULL, NULL);
            }
            return true;
        case ANDROID_SCANCODE_GREEN:
        case ANDROID_SCANCODE_YELLOW:
            return true;
        default:
            return false;
    }
}

/* Android TV remote keys in the UI: the counterpart of platform/webos/navkey_sdl_webos.c. */
#include "android_platform.h"
#include "platform/sdl/navkey_sdl.h"

NAVKEY navkey_from_sdl_android(const SDL_Event *ev) {
    switch (ev->type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            switch ((unsigned int) ev->key.keysym.scancode) {
                case ANDROID_SCANCODE_RED:
                    return NAVKEY_NEGATIVE;
                case ANDROID_SCANCODE_YELLOW:
                case ANDROID_SCANCODE_MENU:
                    return NAVKEY_MENU;
                case ANDROID_SCANCODE_BLUE:
                    return NAVKEY_ALTERNATIVE;
                case SDL_SCANCODE_AC_BACK:
                    return NAVKEY_CANCEL;
                case SDL_SCANCODE_SELECT:   /* the d-pad centre (KEYCODE_DPAD_CENTER) */
                    return NAVKEY_CONFIRM;
                default:
                    break;
            }
            break;
        }
        default:
            break;
    }
    return NAVKEY_UNKNOWN;
}

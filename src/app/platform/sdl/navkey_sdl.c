#include "navkey_sdl.h"
#include "app.h"
#include "input/input_gamepad.h"

#if TARGET_WEBOS

NAVKEY navkey_from_sdl_webos(const SDL_Event *ev);

#elif TARGET_ANDROID

NAVKEY navkey_from_sdl_android(const SDL_Event *ev);

#endif

static NAVKEY navkey_gamepad_map(SDL_JoystickID which, Uint8 button);

static NAVKEY navkey_axis_map(const SDL_ControllerAxisEvent *axis, bool *pressed);

/* ANALOG STICKS NAVIGATE TOO.
 *
 * Only the d-pad and the face buttons used to reach the UI at all -- an axis event
 * fell through to NAVKEY_UNKNOWN -- so a controller with sticks could not drive a
 * menu, which is the first thing anyone tries. This turns the LEFT stick into the
 * same four directions.
 *
 * LEFT STICK ONLY, DELIBERATELY. The right stick is conventionally a camera, and a
 * game pad resting in someone's lap would wander through a menu on its own.
 *
 * A stick is continuous and a key press is not, so the crossing has HYSTERESIS: it
 * takes 60% to engage and stays engaged until it falls back under 35%. One threshold
 * would chatter -- a stick held near the line emits a stream of press/release pairs.
 * Holding a direction reports one long press, so LVGL's own key repeat applies and a
 * held stick repeats exactly like a held d-pad.
 */
#define NAVKEY_AXIS_ENGAGE 19660    /* ~60% of 32767 */
#define NAVKEY_AXIS_RELEASE 11469   /* ~35%: must fall back this far to let go */
#define NAVKEY_AXIS_SLOTS 8

typedef struct {
    SDL_JoystickID which;
    bool used;
    /* Current latched direction per axis, as a NAVKEY (0 = centred). */
    NAVKEY x, y;
} navkey_axis_state_t;

static navkey_axis_state_t axis_state[NAVKEY_AXIS_SLOTS];

static navkey_axis_state_t *axis_state_for(SDL_JoystickID which) {
    navkey_axis_state_t *free_slot = NULL;
    for (int i = 0; i < NAVKEY_AXIS_SLOTS; ++i) {
        if (axis_state[i].used && axis_state[i].which == which) {
            return &axis_state[i];
        }
        if (!axis_state[i].used && free_slot == NULL) {
            free_slot = &axis_state[i];
        }
    }
    if (free_slot != NULL) {
        free_slot->used = true;
        free_slot->which = which;
        free_slot->x = free_slot->y = NAVKEY_UNKNOWN;
    }
    return free_slot;
}

NAVKEY navkey_from_sdl(const SDL_Event *ev, bool *pressed) {
    switch (ev->type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            *pressed = ev->key.state == SDL_PRESSED;
            switch (ev->key.keysym.sym) {
                case SDLK_UP:
                    return NAVKEY_UP;
                case SDLK_DOWN:
                    return NAVKEY_DOWN;
                case SDLK_LEFT:
                    return NAVKEY_LEFT;
                case SDLK_RIGHT:
                    return NAVKEY_RIGHT;
                case SDLK_RETURN:
                    return NAVKEY_CONFIRM;
                case SDLK_ESCAPE:
                    return NAVKEY_CANCEL;
                case SDLK_BACKSPACE:
                    return NAVKEY_NEGATIVE;
                case SDLK_MENU:
                case SDLK_APPLICATION:
                    return NAVKEY_MENU;
                default: {
#if TARGET_WEBOS
                    NAVKEY navkey;
                    if ((navkey = navkey_from_sdl_webos(ev)) != NAVKEY_UNKNOWN) {
                        return navkey;
                    }
#elif TARGET_ANDROID
                    NAVKEY navkey;
                    if ((navkey = navkey_from_sdl_android(ev)) != NAVKEY_UNKNOWN) {
                        return navkey;
                    }
#endif
                    return NAVKEY_UNKNOWN;
                }
            }
        }
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            *pressed = ev->cbutton.state == SDL_PRESSED;
            return navkey_gamepad_map(ev->cbutton.which, ev->cbutton.button);
        }
        case SDL_CONTROLLERAXISMOTION:
            return navkey_axis_map(&ev->caxis, pressed);
        default:
            return NAVKEY_UNKNOWN;
    }
}

NAVKEY navkey_axis_map(const SDL_ControllerAxisEvent *axis, bool *pressed) {
    NAVKEY neg, pos;
    switch (axis->axis) {
        case SDL_CONTROLLER_AXIS_LEFTX:
            neg = NAVKEY_LEFT;
            pos = NAVKEY_RIGHT;
            break;
        case SDL_CONTROLLER_AXIS_LEFTY:
            neg = NAVKEY_UP;             /* SDL's Y grows DOWNWARD */
            pos = NAVKEY_DOWN;
            break;
        default:
            return NAVKEY_UNKNOWN;       /* right stick and triggers stay out of the UI */
    }
    navkey_axis_state_t *st = axis_state_for(axis->which);
    if (st == NULL) {
        return NAVKEY_UNKNOWN;
    }
    NAVKEY *held = axis->axis == SDL_CONTROLLER_AXIS_LEFTX ? &st->x : &st->y;

    const int v = axis->value;
    const int mag = v < 0 ? -v : v;
    NAVKEY want = *held;
    if (*held == NAVKEY_UNKNOWN) {
        if (mag >= NAVKEY_AXIS_ENGAGE) {
            want = v < 0 ? neg : pos;
        }
    } else if (mag < NAVKEY_AXIS_RELEASE) {
        want = NAVKEY_UNKNOWN;
    }
    if (want == *held) {
        return NAVKEY_UNKNOWN;           /* no crossing: the common case, ignore it */
    }
    if (want == NAVKEY_UNKNOWN) {
        /* Let go. Report the release of what WAS held, not of where the stick is. */
        NAVKEY released = *held;
        *held = NAVKEY_UNKNOWN;
        *pressed = false;
        return released;
    }
    if (*held != NAVKEY_UNKNOWN) {
        /* Flicked straight across without passing through centre. Release the old
         * direction first and leave the state centred: the stream of axis events a
         * moving stick produces delivers the new press on the very next one, and a
         * press whose release never arrives would stick a direction down forever. */
        NAVKEY released = *held;
        *held = NAVKEY_UNKNOWN;
        *pressed = false;
        return released;
    }
    *held = want;
    *pressed = true;
    return want;
}

NAVKEY navkey_gamepad_map(SDL_JoystickID which, Uint8 button) {
    /* The same per-controller A/B X/Y choice as in a stream (input_gamepad.c). */
    const bool swap_abxy = app_input_gamepad_swaps_abxy(SDL_GameControllerFromInstanceID(which));
    switch (button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            return NAVKEY_UP;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            return NAVKEY_DOWN;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            return NAVKEY_LEFT;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            return NAVKEY_RIGHT;
        case SDL_CONTROLLER_BUTTON_A:
            return swap_abxy ? NAVKEY_CANCEL : NAVKEY_CONFIRM;
        case SDL_CONTROLLER_BUTTON_B:
            return swap_abxy ? NAVKEY_CONFIRM : NAVKEY_CANCEL;
        case SDL_CONTROLLER_BUTTON_X:
            return swap_abxy ? NAVKEY_ALTERNATIVE : NAVKEY_NEGATIVE;
        case SDL_CONTROLLER_BUTTON_Y:
            return swap_abxy ? NAVKEY_NEGATIVE : NAVKEY_ALTERNATIVE;
        case SDL_CONTROLLER_BUTTON_BACK:
            return NAVKEY_MENU;
        case SDL_CONTROLLER_BUTTON_START:
            return NAVKEY_START;
        default:
            return NAVKEY_UNKNOWN;
    }
}
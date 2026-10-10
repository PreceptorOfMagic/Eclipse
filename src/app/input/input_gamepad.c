#include "input_gamepad.h"

#include <stdbool.h>
#include <SDL_version.h>
#include <Limelight.h>
#include <assert.h>
#include <ctype.h>
#include <string.h>

#include "logging.h"
#include "app_input.h"
#include "app.h"
#include "app_settings.h"

static int new_gamepad_state_index(app_input_t *input, SDL_GameController *controller);

static short next_gamepad_gs_id(app_input_t *input, bool remote_like);

static bool is_same_gamepad(const app_gamepad_state_t *state, SDL_GameController *controller);

static bool str_contains_ci(const char *haystack, const char *needle);

static bool gamepad_is_remote_like(const char *name);

#ifdef TARGET_WEBOS
static bool webos_name_is_non_gamepad(const char *name);
static bool webos_should_ignore_controller_like_device(const char *name, const char *guidstr, SDL_Joystick *joystick);
#endif

bool app_input_init_gamepad(app_input_t *input, int device_index) {
    SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(device_index);
    char guidstr[33];
    SDL_JoystickGetGUIDString(guid, guidstr, 33);
    const char *name = SDL_JoystickNameForIndex(device_index);
    if (SDL_IsGameController(device_index)) {
        SDL_GameController *controller = SDL_GameControllerOpen(device_index);
        if (!controller) {
            commons_log_error("Input", "Could not open gamecontroller %i. GUID: %s, error: %s", device_index, guidstr,
                              SDL_GetError());
            return false;
        }
        SDL_Joystick *joystick = SDL_GameControllerGetJoystick(controller);
#ifdef TARGET_WEBOS
        if (webos_should_ignore_controller_like_device(name, guidstr, joystick)) {
            SDL_GameControllerClose(controller);
            return false;
        }
#endif

        app_gamepad_state_t *state = app_input_gamepad_state_init(input, controller);
        if (state == NULL) {
            SDL_GameControllerClose(controller);
            return false;
        }
        /* SEEN ONCE IS ENOUGH TO BE ASSIGNABLE. Co-op defaults are set per PC in the
         * settings, which has to work with the pad switched off -- so the list of
         * controllers to choose from is built here, as they turn up, rather than from
         * whatever happens to be plugged in when the screen is opened. */
        if (app_configuration != NULL) {
            const char *pad_name = SDL_GameControllerName(controller);
            if (settings_remember_pad(app_configuration, pad_name != NULL ? pad_name : name)) {
                settings_save(app_configuration);
            }
        }
        assert(state->gs_id >= 0);
        input->activeGamepadMask |= 1 << state->gs_id;
        input->gamepads_count++;
        return true;
    } else {
        commons_log_warn("Input", "Unrecognized game controller %s. GUID: %s", name, guidstr);
    }
    return false;
}

void app_input_close_gamepad(app_input_t *input, SDL_JoystickID sdl_id) {
    app_gamepad_state_t *state = app_input_gamepad_state_by_instance_id(input, sdl_id);
    if (!state) {
        return;
    }
    assert(state->gs_id >= 0);
    if (!state->controller) {
        commons_log_warn("Input", "Could not find gamecontroller %i: %s", sdl_id, SDL_GetError());
        return;
    }
    // Reduce number of connected gamepads
    input->gamepads_count--;
    // Remove gamepad mask
    input->activeGamepadMask &= ~(1 << state->gs_id);
#if !SDL_VERSION_ATLEAST(2, 0, 9)
    if (state->haptic) {
        SDL_HapticClose(state->haptic);
    }
#endif
    SDL_GameControllerClose(state->controller);
    commons_log_info("Input", "Controller #%d disconnected, sdl_id: %d", state->gs_id, sdl_id);
    app_input_gamepad_state_deinit(state);
}

app_gamepad_state_t *app_input_gamepad_state_init(app_input_t *input, SDL_GameController *controller) {
    int index = new_gamepad_state_index(input, controller);
    if (index < 0) {
        return NULL;
    }
    app_gamepad_state_t *state = &input->gamepads[index];
    if (state->gs_id < 0) {
        short gsId = next_gamepad_gs_id(input, gamepad_is_remote_like(SDL_GameControllerName(controller)));
        if (gsId < 0) {
            return NULL;
        }
        state->gs_id = gsId;
    }
    SDL_Joystick *joystick = SDL_GameControllerGetJoystick(controller);
    SDL_JoystickID sdl_id = SDL_JoystickInstanceID(joystick);

    SDL_Haptic *haptic = SDL_HapticOpenFromJoystick(joystick);
    unsigned int haptic_bits = SDL_HapticQuery(haptic);
    commons_log_debug("Input", "Controller #%d has supported haptic bits: %x", state->gs_id, haptic_bits);
    if (haptic && (haptic_bits & SDL_HAPTIC_LEFTRIGHT) == 0) {
        SDL_HapticClose(haptic);
        haptic = NULL;
    }
    state->instance_id = sdl_id;
    state->controller = controller;
    state->guid = SDL_JoystickGetGUID(joystick);
#if SDL_VERSION_ATLEAST(2, 0, 14)
    const char *serial = SDL_JoystickGetSerial(joystick);
    state->serial_crc = serial != NULL ? SDL_crc32(0, (const void *) serial, strlen(serial)) : 0;
#endif
#if SDL_VERSION_ATLEAST(2, 0, 12)
    SDL_GameControllerSetPlayerIndex(controller, state->gs_id);
#endif
#if !SDL_VERSION_ATLEAST(2, 0, 9)
    state->haptic = haptic;
    state->haptic_effect_id = -1;
#endif
    commons_log_info("Input", "Controller #%d (%s) connected%s%s", state->gs_id,
                     SDL_JoystickName(joystick),
                     app_input_gamepad_is_nintendo(controller) ? " -- Nintendo pad, A/B X/Y " : "",
                     app_input_gamepad_is_nintendo(controller)
                     ? (app_input_gamepad_swaps_abxy(controller) ? "swapped (Xbox positions)" : "as printed")
                     : "");
    return state;
}

void app_input_gamepad_state_deinit(app_gamepad_state_t *state) {
    short gsId = state->gs_id;
    SDL_JoystickGUID guid = state->guid;
    uint32_t serialCrc = state->serial_crc;
    memset(state, 0, sizeof(app_gamepad_state_t));
    // Set to invalid ID so it can be reused
    state->instance_id = -1;
    // Restore the ID so if the same controller is reconnected it will be assigned the same ID
    state->gs_id = gsId;
    state->guid = guid;
    state->serial_crc = serialCrc;
}

app_gamepad_state_t *app_input_gamepad_state_by_index(app_input_t *input, int index) {
    if (index < 0 || index >= input->max_num_gamepads || input->gamepads[index].instance_id == -1) {
        return NULL;
    }
    return &input->gamepads[index];
}

app_gamepad_state_t *app_input_gamepad_state_by_instance_id(app_input_t *input, SDL_JoystickID instance_id) {
    for (short i = 0; i < (short) input->max_num_gamepads; i++) {
        app_gamepad_state_t *gamepad = &input->gamepads[i];
        if (gamepad->instance_id == instance_id) {
            return gamepad;
        }
    }
    return NULL;
}

int app_input_get_gamepads_count(app_input_t *input) {
    return (int) input->gamepads_count;
}

short app_input_get_max_gamepads(app_input_t *input) {
    return (short) input->max_num_gamepads;
}

short app_input_gamepads_mask(app_input_t *input) {
    return input->activeGamepadMask;
}

void app_input_gamepad_rumble(app_input_t *input, unsigned short controller_id,
                              unsigned short low_freq_motor, unsigned short high_freq_motor) {
    if (controller_id >= app_input_get_max_gamepads(input)) {
        return;
    }

    app_gamepad_state_t *state = &input->gamepads[controller_id];

#if SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_GameControllerRumble(state->controller, low_freq_motor, high_freq_motor, SDL_HAPTIC_INFINITY);
#else
    SDL_Haptic *haptic = state->haptic;
    if (!haptic) {
        return;
    }

    if (state->haptic_effect_id >= 0) {
        SDL_HapticDestroyEffect(haptic, state->haptic_effect_id);
    }

    if (low_freq_motor == 0 && high_freq_motor == 0) {
        return;
    }

    SDL_HapticEffect effect;
    SDL_memset(&effect, 0, sizeof(effect));
    effect.type = SDL_HAPTIC_LEFTRIGHT;
    effect.leftright.length = SDL_HAPTIC_INFINITY;

    // SDL haptics range from 0-32767 but XInput uses 0-65535, so divide by 2 to correct for SDL's scaling
    effect.leftright.large_magnitude = low_freq_motor / 2;
    effect.leftright.small_magnitude = high_freq_motor / 2;

    state->haptic_effect_id = SDL_HapticNewEffect(haptic, &effect);
    if (state->haptic_effect_id >= 0) {
        SDL_HapticRunEffect(haptic, state->haptic_effect_id, 1);
    }
#endif
}


void app_input_gamepad_rumble_triggers(app_input_t *input, unsigned short controllerNumber, unsigned short leftTrigger,
                                       unsigned short rightTrigger) {
#if SDL_VERSION_ATLEAST(2, 0, 14)
    SDL_GameControllerRumbleTriggers(input->gamepads[controllerNumber].controller, leftTrigger, rightTrigger,
                                     SDL_HAPTIC_INFINITY);
#endif
}

void app_input_gamepad_set_motion_event_state(app_input_t *input, unsigned short controllerNumber, uint8_t motionType,
                                              uint16_t reportRateHz) {
#if SDL_VERSION_ATLEAST(2, 0, 14)
    app_gamepad_state_t *gamepad = &input->gamepads[controllerNumber];
    SDL_SensorType sensor_type = SDL_SENSOR_INVALID;
    switch (motionType) {
        case LI_MOTION_TYPE_ACCEL:
            sensor_type = SDL_SENSOR_ACCEL;
            gamepad->accelState.periodMs = reportRateHz > 0 ? 1000 / reportRateHz : 0;
            break;
        case LI_MOTION_TYPE_GYRO:
            sensor_type = SDL_SENSOR_GYRO;
            gamepad->gyroState.periodMs = reportRateHz > 0 ? 1000 / reportRateHz : 0;
            break;
        default:
            break;
    }
    if (sensor_type == SDL_SENSOR_INVALID) {
        return;
    }
    SDL_GameControllerSetSensorEnabled(gamepad->controller, sensor_type, reportRateHz > 0 ? SDL_TRUE : SDL_FALSE);
    commons_log_info("Input", "Setting motion event state for controller %d, motionType: %d, reportRateHz: %d",
                     controllerNumber, motionType, reportRateHz);
#endif
}


void app_input_gamepad_set_controller_led(app_input_t *input, unsigned short controllerNumber, uint8_t r, uint8_t g,
                                          uint8_t b) {
#if SDL_VERSION_ATLEAST(2, 0, 14)
    SDL_GameControllerSetLED(input->gamepads[controllerNumber].controller, r, g, b);
#endif
}

int new_gamepad_state_index(app_input_t *input, SDL_GameController *controller) {
    int index = -1;
    for (short i = 0, j = app_input_get_max_gamepads(input); i < j; i++) {
        if (input->gamepads[i].instance_id != -1) {
            continue;
        }
        if (index == -1) {
            index = i;
        }
        if (is_same_gamepad(&input->gamepads[i], controller)) {
            return i;
        }
    }
    return index;
}

/* A REMOTE NEVER TAKES PLAYER 1. Slots go to devices in the order they connect, and many games only
 * listen to the first one (or the first four) -- so a TV/media remote that registers as a controller
 * ahead of the real pad left the pad as player 2 and dead in those games. Remote-like devices are
 * given slots from the TOP down; real controllers keep filling from slot 0. (webOS ignores remotes
 * entirely, below; elsewhere a remote with a controller mapping can still be used, just last.) */
static short next_gamepad_gs_id(app_input_t *input, bool remote_like) {
    const short count = app_input_get_max_gamepads(input);
    for (short n = 0; n < count; n++) {
        const short i = remote_like ? (short) (count - 1 - n) : n;
        if ((input->activeGamepadMask & (1 << i)) == 0) {
            return i;
        }
    }
    return -1;
}

static bool gamepad_is_remote_like(const char *name) {
    return str_contains_ci(name, "remote") || str_contains_ci(name, "rcu") ||
           str_contains_ci(name, "air mouse") || str_contains_ci(name, "airmouse") ||
           str_contains_ci(name, "media center") || str_contains_ci(name, "ir receiver");
}

bool app_input_gamepad_is_nintendo(SDL_GameController *controller) {
    if (controller == NULL) {
        return false;
    }
#if SDL_VERSION_ATLEAST(2, 0, 12)
    switch (SDL_GameControllerGetType(controller)) {
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
#if SDL_VERSION_ATLEAST(2, 24, 0)
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
#endif
            return true;
        default:
            break;
    }
#endif
#if SDL_VERSION_ATLEAST(2, 0, 6)
    return SDL_GameControllerGetVendor(controller) == 0x057e;   /* Nintendo */
#else
    return false;
#endif
}

bool app_input_gamepad_swaps_abxy(SDL_GameController *controller) {
    if (app_configuration == NULL) {
        return false;
    }
    if (app_configuration->nintendo_layout != NINTENDO_LAYOUT_OFF && app_input_gamepad_is_nintendo(controller)) {
        return app_configuration->nintendo_layout == NINTENDO_LAYOUT_XBOX;
    }
    return app_configuration->swap_abxy;
}

static bool is_same_gamepad(const app_gamepad_state_t *state, SDL_GameController *controller) {
    SDL_Joystick *joystick = SDL_GameControllerGetJoystick(controller);
    SDL_JoystickGUID guid = SDL_JoystickGetGUID(joystick);
    if (memcmp(&state->guid, &guid, sizeof(SDL_JoystickGUID)) != 0) {
        return false;
    }
#if SDL_VERSION_ATLEAST(2, 0, 14)
    const char *serial = SDL_JoystickGetSerial(joystick);
    if (serial == NULL) {
        return state->serial_crc == 0;
    }
    return state->serial_crc == SDL_crc32(0, (const void *) serial, strlen(serial));
#else
    return false;
#endif
}

static bool str_contains_ci(const char *haystack, const char *needle) {
    if (haystack == NULL || needle == NULL || *needle == '\0') {
        return false;
    }
    size_t needle_len = strlen(needle);
    for (const char *cur = haystack; *cur != '\0'; cur++) {
        size_t i = 0;
        while (i < needle_len && cur[i] != '\0' &&
               tolower((unsigned char) cur[i]) == tolower((unsigned char) needle[i])) {
            i++;
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

#ifdef TARGET_WEBOS

static bool webos_name_is_non_gamepad(const char *name) {
    return str_contains_ci(name, "remote") ||
           str_contains_ci(name, "keyboard") ||
           str_contains_ci(name, "mouse") ||
           str_contains_ci(name, "pointer") ||
           str_contains_ci(name, "touch") ||
           str_contains_ci(name, "air mouse") ||
           str_contains_ci(name, "webos") ||
           str_contains_ci(name, "lge") ||
           str_contains_ci(name, "lg electronics");
}

static bool webos_should_ignore_controller_like_device(const char *name, const char *guidstr, SDL_Joystick *joystick) {
    const char *display_name = name != NULL ? name : "Unknown";
    int axes = SDL_JoystickNumAxes(joystick);
    int buttons = SDL_JoystickNumButtons(joystick);
    int hats = SDL_JoystickNumHats(joystick);
    commons_log_info("Input", "Controller-like device candidate: %s. GUID: %s, axes: %d, buttons: %d, hats: %d",
                     display_name, guidstr, axes, buttons, hats);
    if (webos_name_is_non_gamepad(name)) {
        commons_log_info("Input", "Ignoring non-gamepad webOS input device by name: %s. GUID: %s", display_name,
                         guidstr);
        return true;
    }
    if (axes < 4 || buttons < 8) {
        commons_log_info("Input",
                         "Ignoring controller-like device with weak gamepad shape: %s. GUID: %s, axes: %d, buttons: %d, hats: %d",
                         display_name, guidstr, axes, buttons, hats);
        return true;
    }
    return false;
}

#endif
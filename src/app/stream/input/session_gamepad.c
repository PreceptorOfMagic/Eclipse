#include "app.h"

#include <Limelight.h>

#include "stream/input/session_input.h"

#include "util/bus.h"
#include "util/user_event.h"
#include "input/input_gamepad.h"
#include "stream/session.h"
#include "stream/input/session_virt_mouse.h"
#include "logging.h"
#include "coop_input.h"

#define QUIT_BUTTONS (PLAY_FLAG | BACK_FLAG | LB_FLAG | RB_FLAG)
#define GAMEPAD_COMBO_VMOUSE (LB_FLAG | RS_CLK_FLAG)
#define GAMEPAD_COMBO_KEYBOARD (RB_FLAG | RS_CLK_FLAG)
#define GAMEPAD_COMBO_STATS (LB_FLAG | LS_CLK_FLAG)
#if TARGET_ANDROID
/* VIEW + START IS THE WAY HOME ON ANDROID. The SHIELD TV turns a controller's Guide button into Home (its
 * window manager rewrites KEYCODE_BUTTON_MODE to HOME, 2026-10-16); with that taken off so that Guide can
 * reach the host, a controller has no other way back to the TV's home screen during a stream. */
#define GAMEPAD_COMBO_HOME (BACK_FLAG | PLAY_FLAG)
#include "platform/android/android_platform.h"
#endif

static bool quit_combo_pressed = false;
static bool vmouse_combo_pressed = false;
static bool keyboard_combo_pressed = false;
static bool stats_combo_pressed = false;
#if TARGET_ANDROID
static bool home_combo_pressed = false;
#endif

static void release_buttons(stream_input_t *input, app_gamepad_state_t *gamepad);

static bool gamepad_combo_check(int buttons, short combo);

static short host_buttons(const app_gamepad_state_t *gamepad);

static short wire_buttons(const app_gamepad_state_t *gamepad);

static bool sensor_state_needs_update(const app_gamepad_sensor_state_t *state, uint32_t timestamp,
                                      const float data[3]);

static bool vmouse_intercepted(stream_input_t *input, const app_gamepad_state_t *gamepad);

static bool filter_deadzone_2axis(stream_input_t *input, short *x, short *y);

static void send_primary_controller(stream_input_t *input, const app_gamepad_state_t *gamepad,
                                    short buttons, unsigned char left_trigger,
                                    unsigned char right_trigger, short lx, short ly,
                                    short rx, short ry);

static void stream_input_send_gamepad_remove(stream_input_t *input, app_gamepad_state_t *gamepad);

static void stream_input_send_unannounced_gamepads(stream_input_t *input);

static bool stream_input_moonlight_gamepad_enabled(const stream_input_t *input) {
    (void) input;
    return true;
}

void stream_input_handle_cbutton(stream_input_t *input, const SDL_ControllerButtonEvent *event) {
    app_gamepad_state_t *gamepad = app_input_gamepad_state_by_instance_id(input->input, event->which);
    if (gamepad == NULL) {
        return;
    }
    int button = 0;
    /* Per controller: a detected Nintendo pad has its own layout setting (input_gamepad.c). */
    const bool swap_abxy = app_input_gamepad_swaps_abxy(gamepad->controller);
    switch (event->button) {
        case SDL_CONTROLLER_BUTTON_A:
            button = swap_abxy ? B_FLAG : A_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_B:
            button = swap_abxy ? A_FLAG : B_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_Y:
            button = swap_abxy ? X_FLAG : Y_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_X:
            button = swap_abxy ? Y_FLAG : X_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            button = UP_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            button = DOWN_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            button = RIGHT_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            button = LEFT_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_BACK:
            button = BACK_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_START:
            button = PLAY_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_GUIDE:
            button = SPECIAL_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:
            button = LS_CLK_FLAG;
            vmouse_set_modifier(&input->vmouse, event->state == SDL_PRESSED);
            break;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
            button = RS_CLK_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            button = LB_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            button = RB_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_TOUCHPAD:
            button = TOUCHPAD_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_MISC1:
            button = MISC_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_PADDLE1:
            button = PADDLE1_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_PADDLE2:
            button = PADDLE2_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_PADDLE3:
            button = PADDLE3_FLAG;
            break;
        case SDL_CONTROLLER_BUTTON_PADDLE4:
            button = PADDLE4_FLAG;
            break;
        default:
            return;
    }
    if (event->type == SDL_CONTROLLERBUTTONDOWN) {
        gamepad->buttons |= button;
    } else {
        gamepad->buttons &= ~button;
    }

    if (gamepad_combo_check(gamepad->buttons, QUIT_BUTTONS)) {
        quit_combo_pressed = true;
#if TARGET_ANDROID
        home_combo_pressed = false;         /* View + Start on the way to the stream menu combo */
#endif
        return;
    } else if (gamepad_combo_check(gamepad->buttons, GAMEPAD_COMBO_VMOUSE)) {
        vmouse_combo_pressed = true;
        return;
    } else if (gamepad_combo_check(gamepad->buttons, GAMEPAD_COMBO_KEYBOARD)) {
        keyboard_combo_pressed = true;
        return;
    } else if (gamepad_combo_check(gamepad->buttons, GAMEPAD_COMBO_STATS)) {
        stats_combo_pressed = true;
        return;
    }
#if TARGET_ANDROID
    else if (gamepad_combo_check(gamepad->buttons, GAMEPAD_COMBO_HOME)) {
        if (!home_combo_pressed) {
            home_combo_pressed = true;
            /* Whichever of the two went first reached the host: take it back now. */
            if (!input->view_only && stream_input_moonlight_gamepad_enabled(input)) {
                send_primary_controller(input, gamepad, host_buttons(gamepad),
                                        (unsigned char) gamepad->leftTrigger,
                                        (unsigned char) gamepad->rightTrigger,
                                        gamepad->leftStickX, gamepad->leftStickY,
                                        gamepad->rightStickX, gamepad->rightStickY);
                coop_input_send_controller(gamepad->gs_id, (uint16_t) input->input->activeGamepadMask,
                                           host_buttons(gamepad), (unsigned char) gamepad->leftTrigger,
                                           (unsigned char) gamepad->rightTrigger,
                                           gamepad->leftStickX, gamepad->leftStickY,
                                           gamepad->rightStickX, gamepad->rightStickY);
            }
        }
        return;
    }
#endif
    if (gamepad->buttons == 0) {
        if (quit_combo_pressed) {
            quit_combo_pressed = false;
            release_buttons(input, gamepad);
            bus_pushevent(USER_OPEN_OVERLAY, NULL, NULL);
            return;
        } else if (vmouse_combo_pressed) {
            vmouse_combo_pressed = false;
            release_buttons(input, gamepad);
            session_toggle_vmouse(input->session);
            return;
        } else if (keyboard_combo_pressed) {
            keyboard_combo_pressed = false;
            release_buttons(input, gamepad);
            bus_pushevent(USER_OPEN_SOFT_KEYBOARD, NULL, NULL);
            return;
        } else if (stats_combo_pressed) {
            stats_combo_pressed = false;
            release_buttons(input, gamepad);
            bus_pushevent(USER_TOGGLE_STATS_PIN, NULL, NULL);
            return;
        }
#if TARGET_ANDROID
        else if (home_combo_pressed) {
            home_combo_pressed = false;
            release_buttons(input, gamepad);
            android_go_home();
            return;
        }
#endif
    }

    if (input->view_only || !stream_input_moonlight_gamepad_enabled(input)) {
        return;
    }
    send_primary_controller(input, gamepad, host_buttons(gamepad),
                            (unsigned char) gamepad->leftTrigger,
                            (unsigned char) gamepad->rightTrigger,
                            gamepad->leftStickX, gamepad->leftStickY,
                            gamepad->rightStickX, gamepad->rightStickY);
    coop_input_send_controller(gamepad->gs_id, (uint16_t) input->input->activeGamepadMask,
                               host_buttons(gamepad), (unsigned char) gamepad->leftTrigger,
                               (unsigned char) gamepad->rightTrigger,
                               gamepad->leftStickX, gamepad->leftStickY,
                               gamepad->rightStickX, gamepad->rightStickY);
}

void stream_input_handle_caxis(stream_input_t *input, const SDL_ControllerAxisEvent *event) {
    app_gamepad_state_t *gamepad = app_input_gamepad_state_by_instance_id(input->input, event->which);
    if (gamepad == NULL) {
        return;
    }
    switch (event->axis) {
        case SDL_CONTROLLER_AXIS_LEFTX: {
            gamepad->leftStickX = SDL_max(event->value, -32767);
            break;
        }
        case SDL_CONTROLLER_AXIS_LEFTY: {
            // Signed values have one more negative value than
            // positive value, so inverting the sign on -32768
            // could actually cause the value to overflow and
            // wrap around to be negative again. Avoid that by
            // capping the value at 32767.
            gamepad->leftStickY = (short) -SDL_max(event->value, (short) -32767);
            break;
        }
        case SDL_CONTROLLER_AXIS_RIGHTX: {
            gamepad->rightStickX = SDL_max(event->value, -32767);
            break;
        }
        case SDL_CONTROLLER_AXIS_RIGHTY: {
            gamepad->rightStickY = (short) -SDL_max(event->value, (short) -32767);
            break;
        }
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: {
            gamepad->leftTrigger = (char) (event->value * 255UL / 32767);
            break;
        }
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: {
            gamepad->rightTrigger = (char) (event->value * 255UL / 32767);
            break;
        }
        default:
            return;
    }
    filter_deadzone_2axis(input, &gamepad->leftStickX, &gamepad->leftStickY);
    filter_deadzone_2axis(input, &gamepad->rightStickX, &gamepad->rightStickY);

    if (vmouse_intercepted(input, gamepad)) {
        vmouse_set_vector(&input->vmouse, gamepad->rightStickX, gamepad->rightStickY);
        vmouse_set_trigger(&input->vmouse, gamepad->leftTrigger, gamepad->rightTrigger);
    }

    if (input->view_only || !stream_input_moonlight_gamepad_enabled(input)) {
        return;
    }

    if (vmouse_intercepted(input, gamepad)) {
        send_primary_controller(input, gamepad, host_buttons(gamepad), 0, 0,
                                gamepad->leftStickX, gamepad->leftStickY, 0, 0);
        coop_input_send_controller(gamepad->gs_id, (uint16_t) input->input->activeGamepadMask,
                                   host_buttons(gamepad), 0, 0,
                                   gamepad->leftStickX, gamepad->leftStickY, 0, 0);
    } else {
        send_primary_controller(input, gamepad, host_buttons(gamepad),
                                (unsigned char) gamepad->leftTrigger,
                                (unsigned char) gamepad->rightTrigger,
                                gamepad->leftStickX, gamepad->leftStickY,
                                gamepad->rightStickX, gamepad->rightStickY);
        coop_input_send_controller(gamepad->gs_id, (uint16_t) input->input->activeGamepadMask,
                                   host_buttons(gamepad), (unsigned char) gamepad->leftTrigger,
                                   (unsigned char) gamepad->rightTrigger,
                                   gamepad->leftStickX, gamepad->leftStickY,
                                   gamepad->rightStickX, gamepad->rightStickY);
    }
}

void stream_input_handle_csensor(stream_input_t *input, const SDL_ControllerSensorEvent *event) {
    app_gamepad_state_t *gamepad = app_input_gamepad_state_by_instance_id(input->input, event->which);
    if (gamepad == NULL) {
        return;
    }
    if (input->view_only || !stream_input_moonlight_gamepad_enabled(input)) {
        return;
    }
    switch (event->sensor) {
        case SDL_SENSOR_ACCEL: {
            if (sensor_state_needs_update(&gamepad->accelState, event->timestamp, event->data)) {
                gamepad->accelState.lastTimestamp = event->timestamp;
                memcpy(gamepad->accelState.data, event->data, sizeof(gamepad->accelState.data));
                coop_input_send_sensor(gamepad->gs_id, (uint16_t) input->input->activeGamepadMask,
                                       LI_MOTION_TYPE_ACCEL, event->timestamp,
                                       event->data[0], event->data[1], event->data[2]);
                const int primary_slot = coop_input_primary_slot(gamepad->gs_id);
                if (primary_slot >= 0) {
                    LiSendControllerMotionEvent((uint16_t) primary_slot, LI_MOTION_TYPE_ACCEL,
                                                event->data[0], event->data[1], event->data[2]);
                }
            }
            break;
        }
        case SDL_SENSOR_GYRO: {
            if (sensor_state_needs_update(&gamepad->gyroState, event->timestamp, event->data)) {
                gamepad->gyroState.lastTimestamp = event->timestamp;
                memcpy(gamepad->gyroState.data, event->data, sizeof(gamepad->gyroState.data));
                /* SDL reports rad/s; both Limelight connections require deg/s. */
                const float x = event->data[0] * 57.2957795f;
                const float y = event->data[1] * 57.2957795f;
                const float z = event->data[2] * 57.2957795f;
                coop_input_send_sensor(gamepad->gs_id, (uint16_t) input->input->activeGamepadMask,
                                       LI_MOTION_TYPE_GYRO, event->timestamp, x, y, z);
                const int primary_slot = coop_input_primary_slot(gamepad->gs_id);
                if (primary_slot >= 0) {
                    LiSendControllerMotionEvent((uint16_t) primary_slot,
                                                LI_MOTION_TYPE_GYRO, x, y, z);
                }
            }
            break;
        }
        default: {
            return;
        }
    }
}

void stream_input_handle_ctouchpad(stream_input_t *input, const SDL_ControllerTouchpadEvent *event) {
    app_gamepad_state_t *gamepad = app_input_gamepad_state_by_instance_id(input->input, event->which);
    if (gamepad == NULL) {
        return;
    }
    if (input->view_only || !stream_input_moonlight_gamepad_enabled(input)) {
        return;
    }
    if (event->touchpad != 0) {
        return;
    }
    uint8_t event_type;
    switch (event->type) {
        case SDL_CONTROLLERTOUCHPADUP: {
            event_type = LI_TOUCH_EVENT_UP;
            break;
        }
        case SDL_CONTROLLERTOUCHPADDOWN: {
            event_type = LI_TOUCH_EVENT_DOWN;
            break;
        }
        case SDL_CONTROLLERTOUCHPADMOTION: {
            event_type = LI_TOUCH_EVENT_MOVE;
            break;
        }
        default: {
            return;
        }
    }
    const int primary_slot = coop_input_primary_slot(gamepad->gs_id);
    if (primary_slot >= 0) {
        LiSendControllerTouchEvent((uint16_t) primary_slot, event_type,
                                   event->finger, event->x, event->y,
                                   event->pressure);
    }
    coop_input_send_touchpad(gamepad->gs_id, (uint16_t) input->input->activeGamepadMask,
                             event_type, event->finger, event->x, event->y,
                             event->pressure);
}

void stream_input_handle_cdevice(stream_input_t *input, const SDL_ControllerDeviceEvent *event) {
    if (event->type != SDL_CONTROLLERDEVICEREMOVED) {
        return;
    }
    app_gamepad_state_t *gamepad = app_input_gamepad_state_by_instance_id(input->input, event->which);
    if (gamepad == NULL) {
        return;
    }
    if (input->view_only || !stream_input_moonlight_gamepad_enabled(input)) {
        return;
    }
    stream_input_send_gamepad_remove(input, gamepad);
}

void stream_input_handle_jdevice(stream_input_t *input, const SDL_JoyDeviceEvent *event) {
    if (!stream_input_moonlight_gamepad_enabled(input)) {
        return;
    }
    app_gamepad_state_t *gamepad = NULL;
    if (event->type == SDL_JOYDEVICEADDED) {
#if SDL_VERSION_ATLEAST(2, 0, 6)
        SDL_JoystickID instance_id = SDL_JoystickGetDeviceInstanceID(event->which);
        if (instance_id < 0) {
            stream_input_send_unannounced_gamepads(input);
            return;
        }
        gamepad = app_input_gamepad_state_by_instance_id(input->input, instance_id);
        if (gamepad == NULL || input->view_only) {
            return;
        }
        stream_input_send_gamepad_arrive(input, gamepad);
#else
        stream_input_send_unannounced_gamepads(input);
#endif
    } else if (event->type == SDL_JOYDEVICEREMOVED) {
        gamepad = app_input_gamepad_state_by_instance_id(input->input, event->which);
        if (gamepad == NULL || input->view_only) {
            return;
        }
        stream_input_send_gamepad_remove(input, gamepad);
    }
}

void stream_input_send_gamepad_arrive(stream_input_t *input, app_gamepad_state_t *gamepad) {
    if (!stream_input_moonlight_gamepad_enabled(input)) {
        return;
    }
    if (input->announcedGamepadMask & (1 << gamepad->gs_id)) {
        return;
    }
    input->announcedGamepadMask |= 1 << gamepad->gs_id;
    uint8_t type = LI_CTYPE_XBOX;
    uint16_t capabilities = LI_CCAP_ANALOG_TRIGGERS;
    commons_log_info("Input", "Controller %d arrived. Name: %s", gamepad->gs_id,
                     SDL_GameControllerName(gamepad->controller));
    switch (SDL_GameControllerGetType(gamepad->controller)) {
#if SDL_VERSION_ATLEAST(2, 24, 0)
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
#endif
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: {
            type = LI_CTYPE_NINTENDO;
            capabilities &= ~LI_CCAP_ANALOG_TRIGGERS;
            break;
        }
        case SDL_CONTROLLER_TYPE_PS3: {
            type = LI_CTYPE_PS;
            break;
        }
        case SDL_CONTROLLER_TYPE_PS4:
        case SDL_CONTROLLER_TYPE_PS5: {
            type = LI_CTYPE_PS;
            capabilities |= LI_CCAP_TOUCHPAD;
            commons_log_info("Input", "  controller capability: touchpad");
            break;
        }
        default: {
            break;
        }
    }
#if SDL_VERSION_ATLEAST(2, 0, 18)
    if (SDL_GameControllerHasRumble(gamepad->controller)) {
        capabilities |= LI_CCAP_RUMBLE;
        commons_log_info("Input", "  controller capability: rumble");
    }
    if (SDL_GameControllerHasRumbleTriggers(gamepad->controller)) {
        capabilities |= LI_CCAP_TRIGGER_RUMBLE;
        commons_log_info("Input", "  controller capability: trigger rumble");
    }
#else
    capabilities |= LI_CCAP_RUMBLE;
#if SDL_VERSION_ATLEAST(2, 0, 14)
        capabilities |= LI_CCAP_TRIGGER_RUMBLE;
#endif
#endif
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (SDL_GameControllerHasSensor(gamepad->controller, SDL_SENSOR_ACCEL)) {
        capabilities |= LI_CCAP_ACCEL;
        commons_log_info("Input", "  controller capability: accelerometer");
    }
    if (SDL_GameControllerHasSensor(gamepad->controller, SDL_SENSOR_GYRO)) {
        capabilities |= LI_CCAP_GYRO;
        commons_log_info("Input", "  controller capability: gyroscope");
    }
    if (SDL_GameControllerHasLED(gamepad->controller)) {
        capabilities |= LI_CCAP_RGB_LED;
        commons_log_info("Input", "  controller capability: RGB LED");
    }
#endif
    const uint16_t active_mask = (uint16_t) input->input->activeGamepadMask;
    const int primary_slot = coop_input_primary_slot(gamepad->gs_id);
    if (primary_slot >= 0) {
        LiSendControllerArrivalEvent((uint16_t) primary_slot,
                                     coop_input_primary_mask(active_mask), type, 0xFFFFFFFF,
                                     capabilities);
    }
    coop_input_send_arrive(gamepad->gs_id, active_mask, type, capabilities, 0xFFFFFFFF);
}

static void stream_input_send_gamepad_remove(stream_input_t *input, app_gamepad_state_t *gamepad) {
    if (!stream_input_moonlight_gamepad_enabled(input)) {
        return;
    }
    if ((input->announcedGamepadMask & (1 << gamepad->gs_id)) == 0) {
        return;
    }
    input->announcedGamepadMask &= ~(1 << gamepad->gs_id);
    const uint16_t active_mask = (uint16_t) input->input->activeGamepadMask &
                                 (uint16_t) ~(1u << gamepad->gs_id);
    commons_log_info("Input", "Controller %d removed", gamepad->gs_id);
    const int primary_slot = coop_input_primary_slot(gamepad->gs_id);
    if (primary_slot >= 0) {
        LiSendMultiControllerEvent((uint16_t) primary_slot,
                                   coop_input_primary_mask(active_mask),
                                   0, 0, 0, 0, 0, 0, 0);
    }
    coop_input_send_remove(gamepad->gs_id, active_mask);
}

static void stream_input_send_unannounced_gamepads(stream_input_t *input) {
    if (input->view_only) {
        return;
    }
    for (int i = 0, j = app_input_get_max_gamepads(input->input); i < j; ++i) {
        app_gamepad_state_t *gamepad = app_input_gamepad_state_by_index(input->input, i);
        if (gamepad == NULL) {
            continue;
        }
        stream_input_send_gamepad_arrive(input, gamepad);
    }
}

static void release_buttons(stream_input_t *input, app_gamepad_state_t *gamepad) {
    gamepad->buttons = 0;
    gamepad->leftTrigger = 0;
    gamepad->rightTrigger = 0;
    gamepad->leftStickX = 0;
    gamepad->leftStickY = 0;
    gamepad->rightStickX = 0;
    gamepad->rightStickY = 0;
    if (!stream_input_moonlight_gamepad_enabled(input)) {
        return;
    }
    const uint16_t active_mask = (uint16_t) input->input->activeGamepadMask;
    send_primary_controller(input, gamepad, 0, 0, 0, 0, 0, 0, 0);
    coop_input_send_controller(gamepad->gs_id, active_mask, 0, 0, 0, 0, 0, 0, 0);
}

static void send_primary_controller(stream_input_t *input, const app_gamepad_state_t *gamepad,
                                    short buttons, unsigned char left_trigger,
                                    unsigned char right_trigger, short lx, short ly,
                                    short rx, short ry) {
    const int primary_slot = coop_input_primary_slot(gamepad->gs_id);
    if (primary_slot < 0) {
        return;
    }
    const uint16_t active_mask = (uint16_t) input->input->activeGamepadMask;
    LiSendMultiControllerEvent((uint16_t) primary_slot,
                               coop_input_primary_mask(active_mask), buttons,
                               left_trigger, right_trigger, lx, ly, rx, ry);
}


static bool gamepad_combo_check(int buttons, short combo) {
    return (buttons & combo) == combo;
}

static short host_buttons(const app_gamepad_state_t *gamepad) {
    short buttons = (short) gamepad->buttons;
#if TARGET_ANDROID
    /* The way home is not the host's View + Start: from the moment the combo forms until it is let go,
     * the host sees neither, whatever else changes meanwhile. */
    if (home_combo_pressed) {
        buttons = (short) (buttons & ~GAMEPAD_COMBO_HOME);
    }
#endif
    return buttons;
}

static bool sensor_state_needs_update(const app_gamepad_sensor_state_t *state, uint32_t timestamp,
                                      const float data[3]) {
    if (state->periodMs == 0 || !SDL_TICKS_PASSED(timestamp, state->lastTimestamp + state->periodMs)) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (state->data[i] != data[i]) {
            return true;
        }
    }
    return false;
}

static bool vmouse_intercepted(stream_input_t *input, const app_gamepad_state_t *gamepad) {
    if (!session_input_is_vmouse_active(&input->vmouse)) {
        return false;
    }
    return gamepad->rightStickX != 0 || gamepad->rightStickY != 0 || gamepad->leftTrigger != 0 ||
           gamepad->rightTrigger != 0;
}

static bool filter_deadzone_2axis(stream_input_t *input, short *x, short *y) {
    uint32_t magnitude_pow2 = (uint32_t) (*x) * (*x) + (uint32_t) (*y) * (*y);
    uint32_t threshold_sqrt = 32768 * input->stick_deadzone / 100;
    if (magnitude_pow2 < threshold_sqrt * threshold_sqrt) {
        *x = 0;
        *y = 0;
        return true;
    }
    return false;
}

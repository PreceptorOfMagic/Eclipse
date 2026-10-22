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
#include "start_wheel.h"
#include "stream/session_priv.h"

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

/*
 * HOLDING VIEW IS THE GUIDE BUTTON WHERE THE SYSTEM TAKES GUIDE, COPIED FROM SUNSHINE. The SHIELD TV swallows a
 * controller's Guide (Xbox) button and presses Home instead, and offers apps no way out (EclipseActivity.guideIsHome);
 * Steam Link and Moonlight leave it to the system as well. Sunshine's answer is a host setting, back_button_timeout
 * (Sunshine src/input.cpp): hold Back/View that long and the host lets go of View, presses Guide and lets go of it
 * 100 ms later, and View then reads as let go until it really is. This is that, in Eclipse, so it needs no host
 * setting: 2000 ms is the value Sunshine's configuration guide gives for it. Only where the system takes Guide.
 */
#define GUIDE_HOLD_MS 2000
#define GUIDE_TAP_MS 100
static void guide_hold_view(app_gamepad_state_t *gamepad, bool down);
static void guide_tap(app_gamepad_state_t *gamepad);
#endif

static bool quit_combo_pressed = false;
static bool vmouse_combo_pressed = false;
static bool keyboard_combo_pressed = false;
static bool stats_combo_pressed = false;
#if TARGET_ANDROID
static bool home_combo_pressed = false;
#endif

static void release_buttons(stream_input_t *input, app_gamepad_state_t *gamepad);

static bool gamepad_combo_check(const app_gamepad_state_t *gamepad, short combo);

static void shortcut_log(const app_gamepad_state_t *gamepad, const char *what);

static bool wheel_on_button(stream_input_t *input, app_gamepad_state_t *gamepad, int button, bool down,
                            uint32_t timestamp);

static bool wheel_on_axis(stream_input_t *input, app_gamepad_state_t *gamepad);

static void wheel_cancel(void);

static SDL_JoystickID wheel_owner(void);

/* CONTROLLER SHORTCUTS (Settings > Input; app_settings.h controller_shortcuts): the button combos, the Start wheel
 * (start_wheel.h), both or neither. */
static bool combos_enabled(void) {
    return app_configuration->controller_shortcuts == CONTROLLER_SHORTCUTS_ALL ||
           app_configuration->controller_shortcuts == CONTROLLER_SHORTCUTS_COMBOS;
}

static bool wheel_enabled(void) {
    return app_configuration->controller_shortcuts == CONTROLLER_SHORTCUTS_ALL ||
           app_configuration->controller_shortcuts == CONTROLLER_SHORTCUTS_WHEEL;
}

static short host_buttons(const app_gamepad_state_t *gamepad);

static void gamepad_arrival_info(const app_gamepad_state_t *gamepad, uint8_t *type_out, uint16_t *caps_out,
                                 bool log);

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
    if (wheel_on_button(input, gamepad, button, event->type == SDL_CONTROLLERBUTTONDOWN, event->timestamp)) {
        return;
    }
#if TARGET_ANDROID
    if (button == BACK_FLAG) {
        /* One of the button combos: off with them. */
        guide_hold_view(gamepad, event->type == SDL_CONTROLLERBUTTONDOWN && combos_enabled());
    }
#endif

    if (gamepad_combo_check(gamepad, QUIT_BUTTONS)) {
        quit_combo_pressed = true;
#if TARGET_ANDROID
        home_combo_pressed = false;         /* View + Start on the way to the stream menu combo */
#endif
        return;
    } else if (gamepad_combo_check(gamepad, GAMEPAD_COMBO_VMOUSE)) {
        vmouse_combo_pressed = true;
        return;
    } else if (gamepad_combo_check(gamepad, GAMEPAD_COMBO_KEYBOARD)) {
        keyboard_combo_pressed = true;
        return;
    } else if (gamepad_combo_check(gamepad, GAMEPAD_COMBO_STATS)) {
        stats_combo_pressed = true;
        return;
    }
#if TARGET_ANDROID
    else if (gamepad_combo_check(gamepad, GAMEPAD_COMBO_HOME)) {
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
            shortcut_log(gamepad, "stream menu");
            release_buttons(input, gamepad);
            bus_pushevent(USER_OPEN_OVERLAY, NULL, NULL);
            return;
        } else if (vmouse_combo_pressed) {
            vmouse_combo_pressed = false;
            shortcut_log(gamepad, "virtual mouse");
            release_buttons(input, gamepad);
            session_toggle_vmouse(input->session);
            return;
        } else if (keyboard_combo_pressed) {
            keyboard_combo_pressed = false;
            shortcut_log(gamepad, "keyboard");
            release_buttons(input, gamepad);
            bus_pushevent(USER_OPEN_SOFT_KEYBOARD, NULL, NULL);
            return;
        } else if (stats_combo_pressed) {
            stats_combo_pressed = false;
            shortcut_log(gamepad, "performance");
            release_buttons(input, gamepad);
            bus_pushevent(USER_TOGGLE_STATS_PIN, NULL, NULL);
            return;
        }
#if TARGET_ANDROID
        else if (home_combo_pressed) {
            home_combo_pressed = false;
            shortcut_log(gamepad, "Android home");
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

/* The controller whose stick or triggers last moved the virtual mouse, so that ITS release is the one passed on. */
static SDL_JoystickID vmouse_driver = -1;

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

    if (wheel_on_axis(input, gamepad)) {
        return;
    }

    if (vmouse_intercepted(input, gamepad)) {
        vmouse_set_vector(&input->vmouse, gamepad->rightStickX, gamepad->rightStickY);
        vmouse_set_trigger(&input->vmouse, gamepad->leftTrigger, gamepad->rightTrigger);
        vmouse_driver = gamepad->instance_id;
    } else if (session_input_is_vmouse_active(&input->vmouse) && gamepad->instance_id == vmouse_driver) {
        /* THE RELEASE MUST REACH THE VIRTUAL MOUSE TOO. vmouse_intercepted() is false once the stick is back inside
         * the deadzone (so exactly 0) and both triggers are up -- which is the release itself. A stick that springs
         * back from beyond the virtual mouse's threshold in one event left its last speed set, and the pointer kept
         * moving until the stick was touched again (user, SHIELD TV, 2026-10-21); a trigger that snaps up the same way
         * left the button held. Both calls act only on a change. Only the controller that was driving it: another
         * controller's idle stick must not stop the pointer under someone else's thumb. */
        vmouse_set_vector(&input->vmouse, 0, 0);
        vmouse_set_trigger(&input->vmouse, 0, 0);
        vmouse_driver = -1;
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
    commons_log_info("Input", "Controller %d arrived. Name: %s", gamepad->gs_id,
                     SDL_GameControllerName(gamepad->controller));
    uint8_t type;
    uint16_t capabilities;
    gamepad_arrival_info(gamepad, &type, &capabilities, true);
    const uint16_t active_mask = (uint16_t) input->input->activeGamepadMask;
    const int primary_slot = coop_input_primary_slot(gamepad->gs_id);
    if (primary_slot >= 0) {
        LiSendControllerArrivalEvent((uint16_t) primary_slot,
                                     coop_input_primary_mask(active_mask), type, 0xFFFFFFFF,
                                     capabilities);
    }
    coop_input_send_arrive(gamepad->gs_id, active_mask, type, capabilities, 0xFFFFFFFF);
}

#if TARGET_ANDROID
void stream_input_reannounce_peer_gamepads(stream_input_t *input) {
    /* The second co-op PC's controllers, announced again once its session is up. On Android the primary's
     * stream starts before the secondary is ready (session_video.c), so their first arrival can come while
     * the coordinator has no secondary to give it to, and is dropped: PC1 then made a default pad on the
     * first input, without the controller's type or features (2026-10-16). coop_input_send_arrive() sends
     * only the pads routed to the secondary. */
    if (input == NULL || input->view_only || !stream_input_moonlight_gamepad_enabled(input)) {
        return;
    }
    const uint16_t active_mask = (uint16_t) input->input->activeGamepadMask;
    for (size_t i = 0; i < input->input->gamepads_count; ++i) {
        app_gamepad_state_t *gamepad = &input->input->gamepads[i];
        if (gamepad->controller == NULL || gamepad->gs_id < 0 ||
            (input->announcedGamepadMask & (1 << gamepad->gs_id)) == 0) {
            continue;
        }
        uint8_t type;
        uint16_t capabilities;
        gamepad_arrival_info(gamepad, &type, &capabilities, false);
        coop_input_send_arrive(gamepad->gs_id, active_mask, type, capabilities, 0xFFFFFFFF);
    }
}
#endif

static void gamepad_arrival_info(const app_gamepad_state_t *gamepad, uint8_t *type_out, uint16_t *caps_out,
                                 bool log) {
    uint8_t type = LI_CTYPE_XBOX;
    uint16_t capabilities = LI_CCAP_ANALOG_TRIGGERS;
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
            if (log) commons_log_info("Input", "  controller capability: touchpad");
            break;
        }
        default: {
            break;
        }
    }
#if SDL_VERSION_ATLEAST(2, 0, 18)
    if (SDL_GameControllerHasRumble(gamepad->controller)) {
        capabilities |= LI_CCAP_RUMBLE;
        if (log) commons_log_info("Input", "  controller capability: rumble");
    }
    if (SDL_GameControllerHasRumbleTriggers(gamepad->controller)) {
        capabilities |= LI_CCAP_TRIGGER_RUMBLE;
        if (log) commons_log_info("Input", "  controller capability: trigger rumble");
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
        if (log) commons_log_info("Input", "  controller capability: accelerometer");
    }
    if (SDL_GameControllerHasSensor(gamepad->controller, SDL_SENSOR_GYRO)) {
        capabilities |= LI_CCAP_GYRO;
        if (log) commons_log_info("Input", "  controller capability: gyroscope");
    }
    if (SDL_GameControllerHasLED(gamepad->controller)) {
        capabilities |= LI_CCAP_RGB_LED;
        if (log) commons_log_info("Input", "  controller capability: RGB LED");
    }
#endif
    *type_out = type;
    *caps_out = capabilities;
}

static void stream_input_send_gamepad_remove(stream_input_t *input, app_gamepad_state_t *gamepad) {
    if (gamepad->instance_id == wheel_owner()) {
        wheel_cancel();
    }
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


static bool gamepad_combo_check(const app_gamepad_state_t *gamepad, short combo) {
    if (!combos_enabled() || (gamepad->buttons & combo) != combo) {
        return false;
    }
    /* A combo wins over this controller's Start wheel: View + Start + LB + RB is often begun with Start, and held.
     * Another player's wheel is theirs. */
    if (gamepad->instance_id == wheel_owner()) {
        wheel_cancel();
    }
    return true;
}

static void shortcut_log(const app_gamepad_state_t *gamepad, const char *what) {
    /* Which shortcut fired, from which controller: one that "does nothing" on some controller leaves a trace. */
    commons_log_info("Input", "Controller #%d (%s): shortcut %s", gamepad->gs_id,
                     SDL_GameControllerName(gamepad->controller), what);
}

static short host_buttons(const app_gamepad_state_t *gamepad) {
    short buttons = (short) gamepad->buttons;
#if TARGET_ANDROID
    /* The way home is not the host's View + Start: from the moment the combo forms until it is let go,
     * the host sees neither, whatever else changes meanwhile. */
    if (home_combo_pressed) {
        buttons = (short) (buttons & ~GAMEPAD_COMBO_HOME);
    }
    /* View held = Guide: View reads as let go once Guide has been sent, and Guide is down for its tap. */
    if (gamepad->view_masked) {
        buttons = (short) (buttons & ~BACK_FLAG);
    }
    if (gamepad->guide_down) {
        buttons = (short) (buttons | SPECIAL_FLAG);
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

/* THE START WHEEL (start_wheel.h): one at a time, owned by the controller whose Start opened it; every other
 * controller plays on. UI thread only, like the combos above. */
static struct {
    start_wheel_t state;
    session_t *session;     /* the stream it belongs to: a timer or a state from an earlier stream is stale */
    SDL_JoystickID owner;
    lv_timer_t *hold_timer;
} wheel = {.owner = -1};

static SDL_JoystickID wheel_owner(void) {
    return wheel.owner;
}

static start_wheel_pad_t wheel_pad(const app_gamepad_state_t *gamepad) {
    const start_wheel_pad_t pad = {gamepad->buttons, gamepad->leftStickX, gamepad->leftStickY, gamepad->rightStickX,
                                   gamepad->rightStickY};
    return pad;
}

static void wheel_stop_timer(void) {
    if (wheel.hold_timer != NULL) {
        lv_timer_del(wheel.hold_timer);
        wheel.hold_timer = NULL;
    }
}

/** The picture (ui/streaming/start_wheel.view.c): data1 is 0 to hide it, else 1 + the highlighted action. */
static void wheel_picture(bool visible) {
    bus_pushevent(USER_START_WHEEL, (void *) (intptr_t) (visible ? 1 + (int) wheel.state.selected : 0), NULL);
}

static void wheel_run(stream_input_t *input, const app_gamepad_state_t *gamepad, start_wheel_action_t action) {
    if (action == START_WHEEL_CONTINUE) {
        return;
    }
    commons_log_info("Input", "Controller #%d (%s): Start wheel %s", gamepad->gs_id,
                     SDL_GameControllerName(gamepad->controller), start_wheel_action_name(action));
    switch (action) {
        case START_WHEEL_MENU:
            bus_pushevent(USER_OPEN_OVERLAY, NULL, NULL);
            break;
        case START_WHEEL_MOUSE:
            session_toggle_vmouse(input->session);
            break;
        case START_WHEEL_KEYBOARD:
            bus_pushevent(USER_OPEN_SOFT_KEYBOARD, NULL, NULL);
            break;
        case START_WHEEL_PERFORMANCE:
            bus_pushevent(USER_TOGGLE_STATS_PIN, NULL, NULL);
            break;
        default:
            break;
    }
}

static void wheel_apply(stream_input_t *input, app_gamepad_state_t *gamepad, start_wheel_update_t update) {
    if (update.neutral) {
        /* The wheel has the controller: the host sees it at rest until it is let go (V+: sendNeutralState), and a
         * virtual mouse this stick was steering stops. */
        if (session_input_is_vmouse_active(&input->vmouse)) {
            vmouse_set_vector(&input->vmouse, 0, 0);
            vmouse_set_trigger(&input->vmouse, 0, 0);
        }
        if (!input->view_only && stream_input_moonlight_gamepad_enabled(input)) {
            send_primary_controller(input, gamepad, 0, 0, 0, 0, 0, 0, 0);
            coop_input_send_controller(gamepad->gs_id, (uint16_t) input->input->activeGamepadMask,
                                       0, 0, 0, 0, 0, 0, 0);
        }
    }
    if (update.show || update.changed) {
        wheel_picture(true);
    }
    if (update.hide) {
        wheel_picture(false);
    }
    if (wheel.state.state == START_WHEEL_IDLE) {
        wheel_stop_timer();
        wheel.owner = -1;
    }
    wheel_run(input, gamepad, update.run);
}

static void wheel_cancel(void) {
    wheel_stop_timer();
    if (wheel.state.state != START_WHEEL_IDLE && start_wheel_cancel(&wheel.state).hide) {
        wheel_picture(false);
    }
    wheel.owner = -1;
}

static void wheel_hold_timer_cb(lv_timer_t *timer) {
    LV_UNUSED(timer);
    wheel.hold_timer = NULL;        /* run once: LVGL deletes the timer when this returns */
    session_t *session = global != NULL ? global->session : NULL;
    app_gamepad_state_t *gamepad = session != NULL && session == wheel.session
                                   ? app_input_gamepad_state_by_instance_id(session->input.input, wheel.owner) : NULL;
    if (gamepad == NULL || !(gamepad->buttons & PLAY_FLAG)) {
        wheel_cancel();
        return;
    }
    /* This timer is the hold's clock: it is made at Start's press and deleted whenever the gesture ends. */
    wheel_apply(&session->input, gamepad,
                start_wheel_hold_elapsed(&wheel.state, wheel.state.start_down_ms + START_WHEEL_HOLD_MS));
}

/** Start opens the wheel; the owner's buttons then go to it first. True when the event is the wheel's alone. */
static bool wheel_on_button(stream_input_t *input, app_gamepad_state_t *gamepad, int button, bool down,
                            uint32_t timestamp) {
    if (wheel.session != input->session) {
        wheel_stop_timer();
        start_wheel_reset(&wheel.state);
        wheel.owner = -1;
        wheel.session = input->session;
    }
    if (wheel.state.state == START_WHEEL_IDLE) {
        if (button == PLAY_FLAG && down && wheel_enabled()) {
            wheel.owner = gamepad->instance_id;
            start_wheel_start_down(&wheel.state, timestamp);
            wheel_stop_timer();
            wheel.hold_timer = lv_timer_create(wheel_hold_timer_cb, START_WHEEL_HOLD_MS, NULL);
            lv_timer_set_repeat_count(wheel.hold_timer, 1);
        }
        return false;
    }
    if (gamepad->instance_id != wheel.owner) {
        return false;
    }
    const start_wheel_pad_t pad = wheel_pad(gamepad);
    const start_wheel_update_t update = start_wheel_input(&wheel.state, &pad);
    wheel_apply(input, gamepad, update);
    return update.consume;
}

/** The owner's sticks choose while the wheel shows. True when the event is the wheel's alone. */
static bool wheel_on_axis(stream_input_t *input, app_gamepad_state_t *gamepad) {
    if (wheel.session != input->session || wheel.state.state == START_WHEEL_IDLE ||
        gamepad->instance_id != wheel.owner) {
        return false;
    }
    const start_wheel_pad_t pad = wheel_pad(gamepad);
    const start_wheel_update_t update = start_wheel_input(&wheel.state, &pad);
    wheel_apply(input, gamepad, update);
    return update.consume;
}

void stream_input_cancel_start_wheel(void) {
    wheel_cancel();
}

#if TARGET_ANDROID
/** The stream's input, or NULL once the session the timer was started in has gone. */
static stream_input_t *guide_stream_input(void) {
    return global != NULL && global->session != NULL ? &global->session->input : NULL;
}

/** Send @p gamepad's state as the host should see it now (host_buttons), as a button event does. */
static void guide_send_state(stream_input_t *input, app_gamepad_state_t *gamepad) {
    if (input->view_only || !stream_input_moonlight_gamepad_enabled(input)) {
        return;
    }
    const bool vmouse = vmouse_intercepted(input, gamepad);
    const unsigned char lt = vmouse ? 0 : (unsigned char) gamepad->leftTrigger;
    const unsigned char rt = vmouse ? 0 : (unsigned char) gamepad->rightTrigger;
    const short rx = vmouse ? 0 : gamepad->rightStickX, ry = vmouse ? 0 : gamepad->rightStickY;
    send_primary_controller(input, gamepad, host_buttons(gamepad), lt, rt, gamepad->leftStickX, gamepad->leftStickY,
                            rx, ry);
    coop_input_send_controller(gamepad->gs_id, (uint16_t) input->input->activeGamepadMask, host_buttons(gamepad),
                               lt, rt, gamepad->leftStickX, gamepad->leftStickY, rx, ry);
}

static void guide_tap_timer_cb(lv_timer_t *timer) {
    app_gamepad_state_t *gamepad = timer->user_data;
    gamepad->guide_tap_timer = NULL;        /* run once: LVGL deletes the timer when this returns */
    gamepad->guide_down = false;
    stream_input_t *input = guide_stream_input();
    if (input != NULL && gamepad->instance_id == gamepad->guide_owner) {
        guide_send_state(input, gamepad);
    }
}

/** Press Guide now and let go of it GUIDE_TAP_MS later, as Sunshine does. */
static void guide_tap(app_gamepad_state_t *gamepad) {
    stream_input_t *input = guide_stream_input();
    if (input == NULL || gamepad->guide_tap_timer != NULL) {
        return;
    }
    gamepad->guide_owner = gamepad->instance_id;
    gamepad->guide_down = true;
    guide_send_state(input, gamepad);
    lv_timer_t *timer = lv_timer_create(guide_tap_timer_cb, GUIDE_TAP_MS, gamepad);
    lv_timer_set_repeat_count(timer, 1);
    gamepad->guide_tap_timer = timer;
}

static void guide_hold_timer_cb(lv_timer_t *timer) {
    app_gamepad_state_t *gamepad = timer->user_data;
    gamepad->guide_hold_timer = NULL;       /* run once: LVGL deletes the timer when this returns */
    stream_input_t *input = guide_stream_input();
    /* Not while View is part of one of Eclipse's own combos: the host never sees View as such then. */
    if (input == NULL || gamepad->instance_id != gamepad->guide_owner || !(gamepad->buttons & BACK_FLAG) ||
        quit_combo_pressed || vmouse_combo_pressed || keyboard_combo_pressed || stats_combo_pressed ||
        home_combo_pressed) {
        return;
    }
    commons_log_info("Input", "Controller #%d: View held %d ms, sending Guide", gamepad->gs_id, GUIDE_HOLD_MS);
    /* Sunshine: View up first, then Guide down; View stays up until it is let go. */
    gamepad->view_masked = true;
    guide_send_state(input, gamepad);
    guide_tap(gamepad);
}

static void guide_hold_view(app_gamepad_state_t *gamepad, bool down) {
    if (gamepad->guide_hold_timer != NULL) {
        lv_timer_del(gamepad->guide_hold_timer);
        gamepad->guide_hold_timer = NULL;
    }
    /* Pressed afresh or let go, View is View again (Sunshine: back_button_state back to NONE). */
    gamepad->view_masked = false;
    if (down && android_guide_is_home()) {
        gamepad->guide_owner = gamepad->instance_id;
        lv_timer_t *timer = lv_timer_create(guide_hold_timer_cb, GUIDE_HOLD_MS, gamepad);
        lv_timer_set_repeat_count(timer, 1);
        gamepad->guide_hold_timer = timer;
    }
}

void stream_input_send_guide(stream_input_t *input) {
    /* The first controller's Guide: the stream menu cannot tell which controller opened it. */
    for (int i = 0; i < app_input_get_max_gamepads(input->input); i++) {
        app_gamepad_state_t *gamepad = app_input_gamepad_state_by_index(input->input, i);
        if (gamepad != NULL && gamepad->gs_id >= 0) {
            guide_tap(gamepad);
            return;
        }
    }
    commons_log_info("Input", "Guide from the stream menu: no controller connected");
}
#endif

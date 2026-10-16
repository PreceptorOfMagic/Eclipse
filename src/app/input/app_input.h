#pragma once

#include <SDL_haptic.h>
#include <SDL_joystick.h>
#include <SDL_version.h>
#include "gamecontrollerdb_updater.h"

#include "lvgl.h"
#include "lvgl/input/lv_drv_sdl_key.h"

typedef struct app_t app_t;

typedef struct app_gamepad_sensor_state_t {
    uint32_t periodMs;
    uint32_t lastTimestamp;
    float data[3];
} app_gamepad_sensor_state_t;

typedef struct app_gamepad_state_t {
    SDL_JoystickID instance_id;
    SDL_GameController *controller;
    SDL_JoystickGUID guid;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    uint32_t serial_crc;
#endif
    short gs_id;
    char leftTrigger, rightTrigger;
    short leftStickX, leftStickY;
    short rightStickX, rightStickY;
    int buttons;
#if !SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_Haptic *haptic;
    int haptic_effect_id;
#endif
#if SDL_VERSION_ATLEAST(2, 0, 14)
    app_gamepad_sensor_state_t accelState;
    app_gamepad_sensor_state_t gyroState;
#endif
#if TARGET_ANDROID
    /* VIEW HELD = GUIDE (stream/input/session_gamepad.c): the hold and tap timers (lv_timer_t), the controller
     * they were started for, and what the host is shown meanwhile. */
    void *guide_hold_timer, *guide_tap_timer;
    SDL_JoystickID guide_owner;
    bool view_masked, guide_down;
#endif
} app_gamepad_state_t;

typedef struct app_input_t {
    struct app_t *app;
    commons_gcdb_updater_t gcdb_updater;
    char *gcdb_ca_path;
    SDL_Surface *blank_cursor_surface;
    size_t max_num_gamepads;
    app_gamepad_state_t gamepads[16];
    size_t gamepads_count;
    short activeGamepadMask;
    /* Stream-only state behind the moonlight-qt shortcuts (Ctrl+Alt+Shift+Z/M/C/L, desktop builds). Reset
     * when a stream opens and closes (app_input_reset_stream_overrides); never written to the settings. */
    bool grabbed;               /* the last app_set_mouse_grab() request */
    bool capture_released;      /* Z: mouse given back to the desktop until a click in the stream, or Z again */
    bool window_grab_saved;     /* Z: the window grab (L / system-key capture) to restore on re-capture */
    int8_t mouse_mode;          /* M: APP_MOUSE_MODE_* */
    bool show_local_cursor;     /* C: keep the local cursor visible over the stream */
} app_input_t;

enum {
    APP_MOUSE_MODE_SETTING = 0, /* the "Absolute mouse mode" setting decides */
    APP_MOUSE_MODE_RELATIVE,
    APP_MOUSE_MODE_ABSOLUTE,
};


void app_input_init(app_input_t *input, app_t *app);

void app_input_deinit(app_input_t *input);

void app_input_reset_stream_overrides(app_input_t *input);

/** Z: release the mouse to the desktop (captured = false) or take it back. */
void app_input_set_capture(app_input_t *input, bool captured);

/** M: flip between relative (game) and absolute (remote desktop) mouse. Returns true when now relative. */
bool app_input_toggle_mouse_mode(app_input_t *input);

/** C: show or hide the local cursor over the stream. Returns true when now shown. */
bool app_input_toggle_local_cursor(app_input_t *input);

/** L: confine the cursor to the window, or free it. Returns true when now confined. */
bool app_input_toggle_pointer_lock(app_input_t *input);

void app_input_handle_event(app_input_t *input, const SDL_Event *event);

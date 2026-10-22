#include "app_input.h"
#include "logging.h"
#include "app.h"
#include "input_gamepad_mapping.h"

#include "lvgl/lv_sdl_drv_input.h"

void app_input_init(app_input_t *input, app_t *app) {
    input->app = app;
    if (app->settings.condb_path != NULL) {
        app_input_copy_initial_gamepad_mapping(&app->settings);
#if SDL_VERSION_ATLEAST(2, 0, 10) && !TARGET_ANDROID
        SDL_SetHint(SDL_HINT_GAMECONTROLLERCONFIG_FILE, app->settings.condb_path);
#endif
    }
#ifdef SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS
    /* SDL's A is the button PRINTED A on a Nintendo pad (SDL's own default, pinned against an environment
     * override): nintendo_layout decides from there whether that pad is swapped (input_gamepad.c). */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "1");
#endif
    SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
#if TARGET_ANDROID
    /* The database is kept to choose from as each controller connects (input_gamepad_mapping.c). */
    app_input_load_gamepad_db(app->settings.condb_path);
#endif
    input->max_num_gamepads = 4;
    input->gamepads_count = 0;
    for (int i = 0; i < input->max_num_gamepads; i++) {
        input->gamepads[i].instance_id = -1;
        input->gamepads[i].gs_id = -1;
    }
    input->activeGamepadMask = 0;
    input->blank_cursor_surface = SDL_CreateRGBSurface(0, 16, 16, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    input->blank_cursor_surface->userdata = SDL_CreateColorCursor(input->blank_cursor_surface, 0, 0);
    if (input->blank_cursor_surface->userdata == NULL) {
        commons_log_warn("Input", "Failed to create blank cursor: %s", SDL_GetError());
    }
#if !SDL_VERSION_ATLEAST(2, 0, 10)
    SDL_GameControllerAddMappingsFromFile(app->settings.condb_path);
#endif
    app_input_init_gamepad_mapping(input, app->backend.executor, &app->settings);
}

void app_input_deinit(app_input_t *input) {
    app_input_deinit_gamepad_mapping(input);
    if (input->blank_cursor_surface->userdata != NULL) {
        SDL_FreeCursor(input->blank_cursor_surface->userdata);
    }
    SDL_FreeSurface(input->blank_cursor_surface);
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
}

void app_set_mouse_grab(app_input_t *input, bool grab) {
    input->grabbed = grab;
    bool absolute = app_configuration->absmouse;
#if !defined(TARGET_WEBOS)
    /* Ctrl+Alt+Shift+Z released the mouse: it stays released through focus changes until a click in the
     * stream or the combo again, as in moonlight-qt. */
    if (input->capture_released) {
        grab = false;
    }
    if (input->mouse_mode != APP_MOUSE_MODE_SETTING) {
        absolute = input->mouse_mode == APP_MOUSE_MODE_ABSOLUTE;
    }
#endif
    if (!app_configuration->hardware_mouse) {
        SDL_SetRelativeMouseMode(grab && !absolute ? SDL_TRUE : SDL_FALSE);
    }
#ifdef TARGET_WEBOS
    // Temporary workaround for webOS 9: https://github.com/mariotaku/moonlight-tv/issues/466
    if (input->app->os_info.version.major >= 9) {
        return;
    }
#endif
    SDL_ShowCursor(grab && !input->show_local_cursor ? SDL_FALSE : SDL_TRUE);
}

void app_input_reset_stream_overrides(app_input_t *input) {
    input->capture_released = false;
    input->window_grab_saved = false;
    input->mouse_mode = APP_MOUSE_MODE_SETTING;
    input->show_local_cursor = false;
}

#if !defined(TARGET_WEBOS)
void app_input_set_capture(app_input_t *input, bool captured) {
    SDL_Window *window = input->app->ui.window;
    if (!captured && !input->capture_released) {
        input->window_grab_saved = SDL_GetWindowGrab(window) == SDL_TRUE;
        SDL_SetWindowGrab(window, SDL_FALSE);
    } else if (captured && input->capture_released) {
        SDL_SetWindowGrab(window, input->window_grab_saved ? SDL_TRUE : SDL_FALSE);
    }
    input->capture_released = !captured;
    app_set_mouse_grab(input, captured ? true : input->grabbed);
}

bool app_input_toggle_mouse_mode(app_input_t *input) {
    /* Flip what is in force now, whichever of the setting or an earlier toggle decided it. */
    const bool relative = !(app_configuration->absmouse ? input->mouse_mode == APP_MOUSE_MODE_RELATIVE
                                                        : input->mouse_mode != APP_MOUSE_MODE_ABSOLUTE);
    input->mouse_mode = relative ? APP_MOUSE_MODE_RELATIVE : APP_MOUSE_MODE_ABSOLUTE;
    app_set_mouse_grab(input, input->grabbed);
    return relative;
}

bool app_input_toggle_local_cursor(app_input_t *input) {
    input->show_local_cursor = !input->show_local_cursor;
    app_set_mouse_grab(input, input->grabbed);
    return input->show_local_cursor;
}

bool app_input_toggle_pointer_lock(app_input_t *input) {
    SDL_Window *window = input->app->ui.window;
    const bool lock = SDL_GetWindowGrab(window) != SDL_TRUE;
    SDL_SetWindowGrab(window, lock ? SDL_TRUE : SDL_FALSE);
    return lock;
}
#endif

bool app_get_mouse_relative() {
    return SDL_GetRelativeMouseMode() == SDL_TRUE;
}

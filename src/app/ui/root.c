#include "app.h"
#include "root.h"
#include "res.h"

#include "lvgl/lv_disp_drv_app.h"
#include "lvgl/theme/lv_theme_moonlight.h"
#include "lvgl/theme/lv_theme_moonlight_colors.h"
#include "draw/sdl/lv_draw_sdl_utils.h"

#include "stream/session.h"
#include "stream/input/session_input.h"

#include "ui_hover.h"
#include "streaming/streaming.controller.h"
#include "launcher/launcher.controller.h"
#include "launcher/apps.controller.h"

#include "logging.h"
#include "util/bus.h"
#include "util/user_event.h"
#include "util/font.h"

#include <SDL.h>
#include <SDL_image.h>
#include <stdint.h>

#include "ss4s/hostrender.h"

#include "logging_ext_lvgl.h"
#ifdef _WIN32
#include "local_host_win.h"
#endif
#if TARGET_ANDROID
#include "platform/android/android_platform.h"
#endif
#include "fatal_error.h"
#include "app_error.h"
#include "util/i18n.h"
#include "lvgl/util/lv_app_utils.h"

#if FEATURE_WINDOW_FULLSCREEN_DESKTOP
#define APP_FULLSCREEN_FLAG SDL_WINDOW_FULLSCREEN_DESKTOP
#else
#define APP_FULLSCREEN_FLAG SDL_WINDOW_FULLSCREEN
#endif

static SDL_AssertState app_assertion_handler_ui(const SDL_AssertData *data, void *userdata);

static unsigned int last_pts = 0;

lv_img_dsc_t ui_logo_src_ = {.data_size = 0};

typedef struct render_frame_req_t {
    void *data;
    unsigned int pts;
} render_frame_req_t;

SDL_Window *app_ui_create_window(app_ui_t *ui);

static void session_error();

static void session_error_dialog_cb(lv_event_t *event);


#ifdef _WIN32
static void local_host_window_timer_cb(lv_timer_t *timer) {
    app_ui_t *ui = timer->user_data;
    local_host_window_tick(ui->window);
}
#endif

void app_ui_init(app_ui_t *ui, app_t *app) {
    ui->app = app;
    ui->window = app_ui_create_window(ui);
    lv_log_register_print_cb(commons_lv_log);
    lv_init();
    ui->img_decoder = lv_sdl_img_decoder_init(IMG_INIT_JPG | IMG_INIT_PNG);
    app_font_init(&ui->fonts, ui->dpi);
    lv_memset_00(&ui->theme, sizeof(lv_theme_t));
    lv_theme_moonlight_init(&ui->theme, &ui->fonts, app);
#ifdef _WIN32
    /* Co-op with this PC as a host changes the display layout under the window; keep the
     * window on its own monitor (local_host_win.c). Always running: home is learned while
     * nothing is happening. */
    lv_timer_create(local_host_window_timer_cb, LOCAL_HOST_WINDOW_POLL_MS, ui);
#endif
}

/* A MAXIMIZED window's position and size are the maximized client area. Saved as the normal
 * rectangle, they recreated a NORMAL window exactly filling the screen with its frame off the
 * edges -- no reachable title bar, and Windows shrank it at the first display change, which
 * letterboxed a local-host co-op stream (issue #6, run W1 2026-09-27). Save the flag instead
 * and keep the last normal rectangle. */
static void app_ui_save_window_state(SDL_Window *window) {
    window_state_t *state = &app_configuration->window_state;
    const Uint32 flags = SDL_GetWindowFlags(window);
    /* Minimized (closed from the taskbar) or in desktop fullscreen, the window's flags and
     * position describe neither the normal nor the maximized state: keep what was saved. */
    if (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN)) {
        return;
    }
    state->maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
    if (!state->maximized) {
        SDL_GetWindowPosition(window, &state->x, &state->y);
        SDL_GetWindowSize(window, &state->w, &state->h);
    }
}

void app_ui_deinit(app_ui_t *ui) {
    /* The window state is saved in app_ui_close, while the window still exists: app_deinit
     * closes (and destroys the window) BEFORE this runs, so a save here read a NULL window --
     * SDL returns nothing for one, and the exit save never recorded anything (2026-09-27). */
    lv_theme_moonlight_deinit(&ui->theme);
    app_font_deinit(&ui->fonts);
    lv_img_decoder_delete(ui->img_decoder);
}

void app_ui_open(app_ui_t *ui, bool open_launcher, const app_launch_params_t *params) {
    if (ui->disp != NULL) {
        return;
    }
    if (ui->window == NULL) {
        ui->window = app_ui_create_window(ui);
#ifdef TARGET_WEBOS
        /* Display warmup at first window creation - helps compositor be ready
         * for 120Hz when streaming starts. Reduces need to restart app 2-3x. */
        if (app_configuration->stream.fps >= 90) {
            for (int i = 0; i < 4; i++) {
                SDL_PumpEvents();
                SDL_Delay(25);  /* ~100ms at app startup */
            }
        }
#endif
    }
    lv_disp_drv_t *driver = lv_app_disp_drv_create(ui->window, ui->dpi);
    lv_disp_t *disp = lv_disp_drv_register(driver);
    disp->bg_color = lv_color_make(0, 0, 0);
    disp->bg_opa = 0;
    ui->disp = disp;

    lv_theme_t *parent_theme = lv_disp_get_theme(ui->disp);
    ui->theme.color_primary = ml_color_hex(ML_COLOR_PRIMARY);
    ui->theme.color_secondary = ml_color_hex(ML_COLOR_PRIMARY_DIM);
    (void) parent_theme;
    lv_theme_set_parent(&ui->theme, parent_theme);
    lv_disp_set_theme(ui->disp, &ui->theme);

    lv_group_t *group = lv_group_create();
    lv_group_set_editing(group, 0);
    lv_group_set_default(group);
    app_ui_input_init(&ui->input, ui);
    /* LVGL never sets LV_STATE_HOVERED itself, so nothing in this app lit up
     * under the magic remote's cursor. See ui_hover.c. */
    ui_hover_init(&ui->input);

    ui->container = lv_scr_act();
    lv_obj_clear_flag(ui->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ui->container, 0, 0);
    ui->fm = lv_fragment_manager_create(NULL);
    if (open_launcher) {
        launcher_fragment_args_t args = {.app = ui->app, .params = params};
        lv_fragment_t *fragment = lv_fragment_create(&launcher_controller_class, &args);
        lv_fragment_manager_push(ui->fm, fragment, &ui->container);
    }

    SDL_SetAssertionHandler(app_assertion_handler_ui, ui->app);

    app_set_keep_awake(ui->app, false);
}

void app_ui_close(app_ui_t *ui) {
    SDL_SetAssertionHandler(app_assertion_handler_abort, NULL);
    if (ui->disp == NULL) {
        return;
    }

    if (!app_configuration->fullscreen && ui->window != NULL) {
        app_ui_save_window_state(ui->window);
    }
    lv_fragment_manager_del(ui->fm);

    ui->fm = NULL;
    ui->container = NULL;

    ui_hover_deinit();
    app_ui_input_deinit(&ui->input);
    lv_theme_set_parent(&ui->theme, NULL);
    lv_disp_drv_t *driver = ui->disp->driver;
    lv_disp_set_default(NULL);
    lv_disp_remove(ui->disp);
    lv_app_disp_drv_deinit(driver);
    ui->disp = NULL;
    /* Withdraw it before it is destroyed: the video module reads this to find something to
     * draw into, and a stale pointer here is a crash rather than a clean failure. */
    SDL_SetHintWithPriority(SS4S_HINT_HOST_WINDOW, "", SDL_HINT_OVERRIDE);
    SDL_DestroyWindow(ui->window);
    ui->window = NULL;
}

bool app_ui_is_opened(const app_ui_t *ui) {
    return ui->disp != NULL;
}

/* The desktop video module renders into THIS window's renderer, and SDL_Renderer is not
 * thread safe, so it may only do so from this thread. It publishes its draw hook through
 * an SDL hint while a video instance is open; see third_party/ss4s/include/ss4s/hostrender.h
 * for why the handoff looks like this. On webOS there is never a hint: starfish owns a
 * hardware plane under the UI and nothing here draws video. */
static SS4S_HostRender *ui_host_render(void) {
    const char *hint = SDL_GetHint(SS4S_HINT_HOST_RENDER);
    if (hint == NULL || hint[0] == '\0') {
        return NULL;
    }
    SS4S_HostRender *render = NULL;
    if (sscanf(hint, "%p", (void **) &render) != 1) {
        return NULL;
    }
    return render;
}

bool ui_has_stream_renderer() {
    return ui_host_render() != NULL;
}

bool ui_stream_frame_pending() {
    const SS4S_HostRender *render = ui_host_render();
    return render != NULL && render->pending != NULL && render->pending(render->context);
}

/* Called from lv_app_redraw_now() with the window as the render target, before the UI
 * texture is alpha-blended on top: video underneath, UI over it, one present. Returning
 * true tells that function not to clear -- the module already filled the frame. */
bool ui_render_background() {
    const SS4S_HostRender *render = ui_host_render();
    return render != NULL && render->draw != NULL && render->draw(render->context);
}

bool ui_dispatch_userevent(app_t *app, int which, void *data1, void *data2) {
    bool handled = false;
    ui_userevent_t userdata = {data1, data2};
    handled |= app->ui.fm != NULL && lv_fragment_manager_send_event(app->ui.fm, which, &userdata);
    if (!handled) {
        switch (which) {
            case USER_STREAM_OPEN: {
#ifdef TARGET_WEBOS
                /* Display stabilization: webOS compositor may need time to switch to
                 * 120Hz. Multiple pump+delay cycles help avoid intermittent <60fps
                 * on first app launch (user sometimes needs to restart app 2-3x). */
                if (app_configuration->stream.fps >= 90) {
                    for (int i = 0; i < 5; i++) {
                        SDL_PumpEvents();
                        SDL_Delay(30);  /* ~150ms - compositor settle before first frame */
                    }
                }
#endif
                if (app->ss4s.video_cap.transform & SS4S_VIDEO_CAP_TRANSFORM_UI_EXCLUSIVE) {
                    SDL_ShowCursor(SDL_FALSE);
                } else {
                    lv_draw_sdl_drv_param_t *param = lv_disp_get_default()->driver->user_data;
                    // TODO: setup renderer
//                ui_stream_render_host_context.renderer = param->renderer;
//                ui_stream_render = decoder_get_render();
//                if (ui_stream_render) {
//                    ui_stream_render->renderSetup((PSTREAM_CONFIGURATION) data1, &ui_stream_render_host_context);
//                }
                }
                app_input_reset_stream_overrides(&app->input);
                if (session_start_input(app->session)) {
                    app_set_mouse_grab(&app->input, true);
                }
                app_set_keep_awake(app, true);
                streaming_enter_fullscreen(app->session);
                last_pts = 0;
                return true;
            }
            case USER_STREAM_CLOSE: {
                if (app->ss4s.video_cap.transform & SS4S_VIDEO_CAP_TRANSFORM_UI_EXCLUSIVE) {
                    SDL_ShowCursor(SDL_TRUE);
                } else {

                }
                // TODO: close renderer
//                if (ui_stream_render) {
//                    ui_stream_render->renderCleanup();
//                }
                app_set_keep_awake(app, false);
#if TARGET_ANDROID
                android_display_request_refresh(0);   /* back to the system's choice (set: session_worker.c) */
#endif
                app_input_reset_stream_overrides(&app->input);
                if (session_has_input(app->session)) {
                    app_set_mouse_grab(&app->input, false);
                    session_stop_input(app->session);
                }
//                ui_stream_render = NULL;
//                ui_stream_render_host_context.renderer = NULL;
                app_ui_open(&app->ui, true, NULL);
                return true;
            }
            case USER_STREAM_FINISHED: {
                if (streaming_errno != 0) {
                    /* A PIN HDR-reconnect launch that will be retried (e.g. Apollo's
                     * transient 503 during display/encoder reinit) — stay silent and
                     * let the retry run; only surface the dialog once we give up. */
                    if (!apps_controller_hdr_relaunch_will_retry()) {
                        session_error();
                    }
                    break;
                }
                return true;
            }
            case USER_OPEN_OVERLAY: {
                if (app->session != NULL && !app_ui_is_opened(&app->ui)) {
                    session_interrupt(app->session, false, STREAMING_INTERRUPT_USER);
                    return true;
                }
                return false;
            }
            default:
                break;
        }
    }
    return handled;
}

bool ui_should_block_input() {
    return streaming_overlay_shown() || streaming_soft_keyboard_shown();
}

void ui_display_size(app_ui_t *ui, int width, int height) {
    ui->width = width;
    ui->height = height;
    if (ui->dpi <= 0) {
        // For wide-screen (wider than 16:9), we use height instead of width to calculate DPI
        if (width * 9 > height * 16) {
            ui->dpi = height / 3;
        } else {
            ui->dpi = width / 6;
        }
    }
    commons_log_info("UI", "Display size changed to %d x %d", width, height);
}

bool ui_set_input_mode(app_ui_input_t *input, app_ui_input_mode_t mode) {
    if (input->mode == mode) {
        return false;
    }
    input->mode = mode;
    /* Whoever is driving owns the highlight -- see ui_hover.c. Done here, on the
     * transition, because this is called from the input driver's read callback, before
     * the press that caused it becomes an event. */
    ui_hover_input_mode_changed();
    return true;
}

app_ui_input_mode_t app_ui_get_input_mode(const app_ui_input_t *input) {
    return input->mode;
}

const lv_img_dsc_t *ui_logo_src() {
    if (ui_logo_src_.data_size == 0) {
        ui_logo_src_.header.w = LV_DPX(NAV_LOGO_SIZE);
        ui_logo_src_.header.h = LV_DPX(NAV_LOGO_SIZE);
        ui_logo_src_.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;

        ui_logo_src_.data_size = sizeof(lv_sdl_img_data_logo_96);
        ui_logo_src_.data = (const uint8_t *) &lv_sdl_img_data_logo_96;
    }
    return &ui_logo_src_;
}

static void handle_queued_frame(render_frame_req_t *req) {
    bool redraw = false;
    if (last_pts > req->pts) {
        commons_log_warn("Stream", "Ignore late frame pts: %d", req->pts);
        return;
    }
//    if (ui_stream_render && ui_stream_render->renderSubmit(req->data)) {
//        redraw = true;
//    }
    if (redraw) {
        lv_app_redraw_now(lv_disp_get_default()->driver);
    }
}

bool ui_render_queue_submit(void *data, unsigned int pts) {
//    commons_log_debug("Stream", "Submit frame. pts: %d", pts);
    render_frame_req_t req = {.data = data, .pts = pts};
    app_bus_post_sync(global, (bus_actionfunc) handle_queued_frame, &req);
    return true;
}

void ui_cb_destroy_fragment(lv_event_t *e) {
    lv_fragment_t *fragment = lv_event_get_user_data(e);
    if (fragment->cls->obj_deleted_cb) {
        fragment->cls->obj_deleted_cb(fragment, lv_event_get_target(e));
    }
    fragment->obj = NULL;
    lv_fragment_del(fragment);
}

static bool app_ui_display_usable_bounds(int display, SDL_Rect *bounds) {
    return SDL_GetDisplayUsableBounds(display, bounds) == 0 ||
           SDL_GetDisplayBounds(display, bounds) == 0;
}

static int64_t app_ui_rect_overlap_area(const window_state_t *state, const SDL_Rect *bounds) {
    const int64_t left = state->x > bounds->x ? state->x : bounds->x;
    const int64_t top = state->y > bounds->y ? state->y : bounds->y;
    const int64_t state_right = (int64_t) state->x + state->w;
    const int64_t state_bottom = (int64_t) state->y + state->h;
    const int64_t bounds_right = (int64_t) bounds->x + bounds->w;
    const int64_t bounds_bottom = (int64_t) bounds->y + bounds->h;
    const int64_t right = state_right < bounds_right ? state_right : bounds_right;
    const int64_t bottom = state_bottom < bounds_bottom ? state_bottom : bounds_bottom;
    return right > left && bottom > top ? (right - left) * (bottom - top) : 0;
}

static void app_ui_normalize_window_state(window_state_t *state, int fallback_display) {
    if (state == NULL) {
        return;
    }
    if (state->w < 640 || state->h < 480) {
        state->w = 1280;
        state->h = 720;
    }
    const int displays = SDL_GetNumVideoDisplays();
    if (displays <= 0) {
        return;
    }
    if (fallback_display < 0 || fallback_display >= displays) {
        fallback_display = 0;
    }

    const bool symbolic_position = SDL_WINDOWPOS_ISUNDEFINED(state->x) ||
                                   SDL_WINDOWPOS_ISCENTERED(state->x) ||
                                   SDL_WINDOWPOS_ISUNDEFINED(state->y) ||
                                   SDL_WINDOWPOS_ISCENTERED(state->y);
    int display = fallback_display;
    int64_t best_overlap = 0;
    if (!symbolic_position) {
        for (int i = 0; i < displays; ++i) {
            SDL_Rect candidate;
            if (!app_ui_display_usable_bounds(i, &candidate)) {
                continue;
            }
            const int64_t overlap = app_ui_rect_overlap_area(state, &candidate);
            if (overlap > best_overlap) {
                best_overlap = overlap;
                display = i;
            }
        }
    }

    SDL_Rect usable;
    if (!app_ui_display_usable_bounds(display, &usable) || usable.w <= 0 || usable.h <= 0) {
        state->x = SDL_WINDOWPOS_CENTERED_DISPLAY(display);
        state->y = SDL_WINDOWPOS_CENTERED_DISPLAY(display);
        return;
    }

    /* A saved 4K/maximized rectangle restored onto a smaller or differently
     * scaled monitor must not leave its title bar and controls off-screen. */
    if (state->w > usable.w) {
        state->w = usable.w;
    }
    if (state->h > usable.h) {
        state->h = usable.h;
    }

    if (symbolic_position || best_overlap == 0) {
        state->x = SDL_WINDOWPOS_CENTERED_DISPLAY(display);
        state->y = SDL_WINDOWPOS_CENTERED_DISPLAY(display);
        return;
    }

    const int max_x = usable.x + usable.w - state->w;
    const int max_y = usable.y + usable.h - state->h;
    if (state->x < usable.x) {
        state->x = usable.x;
    } else if (state->x > max_x) {
        state->x = max_x;
    }
    if (state->y < usable.y) {
        state->y = usable.y;
    } else if (state->y > max_y) {
        state->y = max_y;
    }
}

/* A config saved before `maximized` existed holds the maximized client area as the normal
 * rectangle. Exactly one monitor's usable area is that signature: take it as maximized and give
 * the window a normal rectangle it can actually be restored to. */
static void app_ui_adopt_legacy_maximized(window_state_t *state) {
    if (state->maximized) {
        return;
    }
    const int displays = SDL_GetNumVideoDisplays();
    for (int i = 0; i < displays; ++i) {
        SDL_Rect usable;
        if (SDL_GetDisplayUsableBounds(i, &usable) == 0 && state->x == usable.x && state->y == usable.y &&
            state->w == usable.w && state->h == usable.h) {
            state->maximized = true;
            state->w = 1280;
            state->h = 720;
            /* Real coordinates on display i: a symbolic "centred" position would be resolved
             * against display 0 by app_ui_normalize_window_state. */
            state->x = usable.x + (usable.w - state->w) / 2;
            state->y = usable.y + (usable.h - state->h) / 2;
            return;
        }
    }
}

SDL_Window *app_ui_create_window(app_ui_t *ui) {
    Uint32 win_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    int win_x = SDL_WINDOWPOS_UNDEFINED, win_y = SDL_WINDOWPOS_UNDEFINED,
        win_width = 1920, win_height = 1080;
    app_ui_adopt_legacy_maximized(&app_configuration->window_state);
    app_ui_normalize_window_state(&app_configuration->window_state, 0);
    if (app_configuration->fullscreen) {
        win_flags |= APP_FULLSCREEN_FLAG;
        int display = 0;
        const int saved_x = app_configuration->window_state.x;
        const int saved_y = app_configuration->window_state.y;
        if (!SDL_WINDOWPOS_ISUNDEFINED(saved_x) && !SDL_WINDOWPOS_ISCENTERED(saved_x) &&
            !SDL_WINDOWPOS_ISUNDEFINED(saved_y) && !SDL_WINDOWPOS_ISCENTERED(saved_y)) {
            SDL_Point saved_center = {
                    saved_x + app_configuration->window_state.w / 2,
                    saved_y + app_configuration->window_state.h / 2,
            };
            const int displays = SDL_GetNumVideoDisplays();
            for (int i = 0; i < displays; ++i) {
                SDL_Rect bounds;
                if (SDL_GetDisplayBounds(i, &bounds) == 0 &&
                    SDL_PointInRect(&saved_center, &bounds)) {
                    display = i;
                    break;
                }
            }
        }
        win_x = SDL_WINDOWPOS_CENTERED_DISPLAY(display);
        win_y = SDL_WINDOWPOS_CENTERED_DISPLAY(display);
        SDL_DisplayMode mode;
        if (SDL_GetDesktopDisplayMode(display, &mode) == 0 && mode.w > 0 && mode.h > 0) {
            win_width = mode.w;
            win_height = mode.h;
        }
    } else {
        win_x = app_configuration->window_state.x;
        win_y = app_configuration->window_state.y;
        win_width = app_configuration->window_state.w;
        win_height = app_configuration->window_state.h;
        if (app_configuration->window_state.maximized) {
            win_flags |= SDL_WINDOW_MAXIMIZED;
        }
    }
#if TARGET_ANDROID
    /* The UI is composited over the video surface (platform/android): its EGL surface needs an alpha channel
     * so the transparent areas of a stream's UI show the picture underneath, as the webOS plane does. */
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
#endif
    SDL_Window *win = SDL_CreateWindow("Eclipse", win_x, win_y, win_width, win_height, win_flags);
    if (win == NULL) {
#ifdef TARGET_WEBOS
        // For webOS 24 (9.0), unpopulated jailer config could cause graphics driver initialization failure
        // We don't have any way to work around this, so we try to launch a web page about this issue
        if (ui->app->os_info.version.major >= 9) {
            SDL_OpenURL("https://github.com/mariotaku/moonlight-tv/wiki/Known-Issues");
        }
#endif
        commons_log_fatal("APP", "Failed to create window: %s", SDL_GetError());
        app_halt(ui->app);
    }

    SDL_Surface *winicon = IMG_Load_RW(SDL_RWFromConstMem(lv_sdl_img_data_logo_96.data.constptr,
                                                          (int) lv_sdl_img_data_logo_96.data_len), SDL_TRUE);
    SDL_SetWindowIcon(win, winicon);
    SDL_FreeSurface(winicon);
    SDL_SetWindowMinimumSize(win, 640, 480);
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    SDL_assert_release(w > 0 && h > 0);
    ui_display_size(ui, w, h);
    /* Publish the window for the video module, which must not create one of its own: a
     * second window takes focus, and app_event_filter() interrupts the session on
     * SDL_WINDOWEVENT_FOCUS_LOST. Published here rather than at either call site because
     * there are two -- app_ui_init() at startup and app_ui_open() after a close -- and
     * only the second one was covered, so the module found no window at all. */
    char window_hint[32];
    snprintf(window_hint, sizeof(window_hint), "%p", (void *) win);
    SDL_SetHintWithPriority(SS4S_HINT_HOST_WINDOW, window_hint, SDL_HINT_OVERRIDE);
    return win;
}

bool app_set_fullscreen(app_t *app, bool fullscreen) {
    if (app == NULL || app->ui.window == NULL) {
        return false;
    }
    /* A check box emits VALUE_CHANGED before its generic preference binding writes
     * the new value, then emits it again afterwards.  Treat the stale first event as
     * a no-op before it can overwrite the saved windowed rectangle with fullscreen
     * dimensions.  Keeping this at the surface boundary also makes every caller
     * idempotent without adding a Windows-specific restore path. */
    const bool active =
            (SDL_GetWindowFlags(app->ui.window) & APP_FULLSCREEN_FLAG) != 0;
    if (active == fullscreen) {
        return true;
    }
    if (fullscreen) {
        app_ui_save_window_state(app->ui.window);
    }
    SDL_SetWindowMinimumSize(app->ui.window, 640, 480);
    if (SDL_SetWindowFullscreen(app->ui.window, fullscreen ? APP_FULLSCREEN_FLAG : 0) != 0) {
        app_configuration->fullscreen = !fullscreen;
        commons_log_warn("UI", "Could not %s fullscreen: %s",
                         fullscreen ? "enter" : "leave", SDL_GetError());
        return false;
    } else if (!fullscreen) {
        /* SDL's implicit restore rectangle is not reliable when the window was
         * CREATED fullscreen: on a fresh install it commonly restores to the
         * desktop-sized creation bounds.  Apply the configuration's explicit
         * windowed rectangle instead.  The defaults are always valid (1280x720,
         * centred), and loaded values were validated in app_ui_create_window(). */
        int display = SDL_GetWindowDisplayIndex(app->ui.window);
        app_ui_normalize_window_state(&app_configuration->window_state, display);
        SDL_RestoreWindow(app->ui.window);
        SDL_SetWindowSize(app->ui.window, app_configuration->window_state.w,
                          app_configuration->window_state.h);
        SDL_SetWindowPosition(app->ui.window,
                              app_configuration->window_state.x,
                              app_configuration->window_state.y);
        if (app_configuration->window_state.maximized) {
            SDL_MaximizeWindow(app->ui.window);   /* the rect above is where it restores to */
        }
    }
    return true;
}

static SDL_AssertState app_assertion_handler_ui(const SDL_AssertData *data, void *userdata) {
    (void) userdata;
    app_fatal_error("Assertion failure", "at %s\n(%s:%d): '%s'", data->function, data->filename, data->linenum,
                    data->condition);
    return SDL_ASSERTION_ALWAYS_IGNORE;
}

static void session_error() {
    static const char *btn_texts[] = {translatable("OK"), ""};
    lv_obj_t *dialog = lv_msgbox_create_i18n(NULL, locstr("Failed to start streaming"), streaming_errmsg,
                                             btn_texts, false);
    lv_obj_add_event_cb(dialog, session_error_dialog_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(dialog);
}

static void session_error_dialog_cb(lv_event_t *event) {
    lv_obj_t *dialog = lv_event_get_current_target(event);
    lv_msgbox_close_async(dialog);
}

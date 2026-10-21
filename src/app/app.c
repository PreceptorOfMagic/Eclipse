#include "util/crash_log.h"
#include "util/diag_logs.h"
#include "util/diag_report.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <SDL.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include "lvgl/lv_sdl_drv_input.h"
#include "lvgl/lv_disp_drv_app.h"
#include "lvgl/util/lv_app_utils.h"

#include "app.h"
#include "app_launch.h"
#include "config.h"

#include "logging.h"
#include "logging_ext_sdl.h"
#include "logging_ext_ss4s.h"
#include "backend/backend_root.h"
#if TARGET_ANDROID
#include "backend/pcmanager/discovery/discovery.h"
#endif
#include "platform/local_input.h"
#include "platform/input_names.h"
#include "stream/session.h"
#include "stream/input/coop_input.h"
#include "stream/video/coop_tunables.h"
#include "stream/video/coop_session.h"
#include "stream/video/coop_secondary.h"
#include "ui/root.h"
#include "util/bus.h"
#include "util/user_event.h"
#include "util/i18n.h"
#include "util/path.h"

#include "ss4s_modules.h"
#include "ss4s.h"
#include "stream/session_events.h"
#include "ui/fatal_error.h"
#include "app_error.h"
#include "app_session.h"
#include "stream/embed_wrapper.h"
#include "platform/webos/vendor_diag.h"

/* Defined here rather than in platform/common/app_common.c: that file is the NON-webOS
 * implementation of app_handle_launch and is not compiled for the TV, so the symbols
 * linked nowhere. app.c is built on every platform. */
static bool coop_requested_at_launch = false;

bool app_launch_coop_requested(void) { return coop_requested_at_launch; }

void app_launch_set_coop_requested(bool requested) { coop_requested_at_launch = requested; }

#define HOST_MONITOR_OFF_ENV "ECLIPSE_HOST_MONITOR_OFF"

void app_launch_choose_host_monitor(bool chose_off) {
    const int mode = app_configuration != NULL ? app_configuration->host_monitor_mode : HOST_MONITOR_CHOOSE;
    const bool off = mode == HOST_MONITOR_OFF || (mode == HOST_MONITOR_CHOOSE && chose_off);
    setenv(HOST_MONITOR_OFF_ENV, off ? "1" : "0", 1);
}

bool app_launch_host_monitor_off(void) {
    const char *v = getenv(HOST_MONITOR_OFF_ENV);
    return v != NULL && strcmp(v, "1") == 0;
}

bool app_launch_host_monitor_choice_offered(void) {
    return app_configuration != NULL && app_configuration->host_monitor_mode == HOST_MONITOR_CHOOSE;
}

PCONFIGURATION app_configuration = NULL;

static void quit_confirm_cb(lv_event_t *e);

static void libs_init(app_t *app, int argc, char *argv[]);

app_t *global = NULL;

/* TEST ONLY (support logs): stall the UI thread this many seconds on its next main-loop pass, so the stall record
 * can be tested. Desktop: ECLIPSE_TEST_HANG=N; webOS beta: the aurora_test_hang launch parameter. */
static unsigned test_hang_s;

void app_test_hang_request(unsigned seconds) { test_hang_s = seconds; }

/* Aggregate only: no key values, coordinates, or per-event disk writes. SDL
 * event age starts at SDL's timestamp, not at physical actuation. */
static bool input_trace_enabled;
static struct {
    uint32_t report_at, last_loop, gap_max, pump_max, ui_max;
    unsigned count[3], age_max[3], dispatch_max[3];
} input_trace;

static void input_trace_event(const SDL_Event *event, uint32_t start) {
    int group = event->type >= SDL_CONTROLLERAXISMOTION &&
                event->type <= SDL_CONTROLLERSENSORUPDATE ? 1 :
                (event->type >= SDL_MOUSEMOTION && event->type <= SDL_MOUSEWHEEL ? 0 : 2);
    const int32_t age = (int32_t) (start - event->common.timestamp);
    const uint32_t dispatch = SDL_GetTicks() - start;
    input_trace.count[group]++;
    if (age >= 0 && (uint32_t) age > input_trace.age_max[group]) {
        input_trace.age_max[group] = (uint32_t) age;
    }
    if (dispatch > input_trace.dispatch_max[group]) input_trace.dispatch_max[group] = dispatch;
}


int app_init(app_t *app, app_settings_loader *settings_loader, int argc, char *argv[]) {
    assert(settings_loader != NULL);
    memset(app, 0, sizeof(*app));
    commons_logging_init("aurora");
    SDL_LogSetOutputFunction(commons_sdl_log, NULL);
    SDL_SetAssertionHandler(app_assertion_handler_abort, NULL);
    SDL_Init(0);
#ifdef OS_WINDOWS
    /* A GUI-subsystem executable has no console, so stderr otherwise disappears.
     * Keep the same per-user diagnostic file used by the streaming/co-op probes;
     * the UTF-8 path wrapper also covers non-ASCII Windows profile names. */
    if (path_freopen(path_diag_log(), "a", stderr) != NULL) {
        setvbuf(stderr, NULL, _IOLBF, 0);
    }
#endif
    commons_log_info("APP", "Start Eclipse. Version %s", APP_VERSION);
    if (settings_loader(&app->settings) != 0) {
        /* Desktop release builds use the GUI subsystem, so stderr is normally invisible.
         * A broken/unwritable profile must not look like an app that simply failed to open.
         * Keep co-op helpers headless even on this fatal path. */
        if (coop_secondary_mode() == NULL) {
            (void) SDL_ShowSimpleMessageBox(
                    SDL_MESSAGEBOX_ERROR,
                    "Eclipse could not start",
                    "Eclipse could not open its per-user settings directory. Check that your user profile "
                    "is available and writable, then try again.",
                    NULL);
        }
        SDL_Quit();
        commons_logging_deinit();
        return -1;
    }
#ifdef TARGET_WEBOS
    input_trace_enabled = access(AURORA_TRACE_SENTINEL, F_OK) == 0;
    if (input_trace_enabled) setenv("AURORA_INPUT_TRACE", "1", 1);
    else unsetenv("AURORA_INPUT_TRACE");
    setenv("AURORA_INPUT_ROLE", coop_secondary_mode() != NULL ? "peer" : "primary", 1);
#endif
    app->main_thread_id = SDL_ThreadID();
    app->running = true;
    app->focused = false;
#if FEATURE_EMBEDDED_SHELL
    app->embed_version.major = -1;
#endif
    app_configuration = &app->settings;
    const bool headless_secondary = coop_secondary_mode() != NULL;
    /* A run that did not end cleanly keeps the NEXT run's log even with the setting off (util/diag_logs.h). */
    char previous_run[256] = "";
    const bool previous_unclean = !headless_secondary &&
                                  diag_logs_run_begin(app->settings.conf_dir, previous_run, sizeof(previous_run));
    if ((app->settings.diag_logging || previous_unclean) && !headless_secondary) {
        diag_logs_start(app->settings.conf_dir);   /* before "System: ..." below, so each kept log starts with it */
        if (diag_logs_active()) {
            SDL_LogSetAllPriority(SDL_LOG_PRIORITY_INFO);   /* SDL's own messages too, not just its errors */
            if (previous_unclean) {
                commons_log_warn("DiagLogs", "The previous run (%s) did not end cleanly: it crashed, froze and was "
                                             "closed by force, failed to start, or the device lost power%s",
                                 previous_run, app->settings.diag_logging ? "" :
                                               ". Keeping this run's log although \"Keep diagnostic logs\" is off");
            }
        }
    }
    /* The co-op dials are sentinel files (coop_tunables.h). Written here, at start, so
     * the app comes up in the state the settings describe -- and so a test script that
     * writes the same files afterwards still wins, which is what keeps the measured
     * co-op procedure working. */
    coop_tunables_apply(coop_orientation_resolved());
    if (os_info_get(&app->os_info) == 0) {
        char *info_str = os_info_str(&app->os_info);
        commons_log_info("APP", "System: %s", info_str);
        free(info_str);
    }
    if (diag_logs_active() && !headless_secondary) {
        /* What this run is on, BEFORE video/audio/driver set-up: a run that dies there still says (util/diag_report.h). */
        diag_report_log_start(app);
#if !defined(TARGET_WEBOS)
        if (previous_unclean) {
            /* Refresh the one file to send, so it holds the run that went wrong even if this run cannot reach the
             * Share-logs screen either (the TV has no file a user can reach: its page needs the app running). */
            char *summary = diag_report_build_start(app);
            char bundle[1024];
            if (diag_logs_write_bundle(app->settings.conf_dir, summary, bundle, sizeof(bundle))) {
                commons_log_info("DiagLogs", "Share bundle refreshed after that: %s", bundle);
            }
            free(summary);
        }
#if defined(AURORA_TEST_HOOKS)
        /* TEST ONLY: ECLIPSE_TEST_CRASH=early crashes here, while starting; =1 once the app is up (below). */
        const char *test_crash_early = getenv("ECLIPSE_TEST_CRASH");
        if (test_crash_early != NULL && strcmp(test_crash_early, "early") == 0) {
            commons_log_warn("APP", "Test crash requested (ECLIPSE_TEST_CRASH=early): crashing while starting");
            volatile int *nowhere = NULL;
            *nowhere = 1;
        }
#endif
#endif
    }
    libs_init(app, argc, argv);

    app_init_locale();
    backend_init(&app->backend, app);

#if TARGET_WEBOS
    SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_BACK, "true");
    SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_EXIT, "true");
    SDL_SetHint(SDL_HINT_WEBOS_CURSOR_SLEEP_TIME, "5000");
    SDL_SetHint(SDL_HINT_WEBOS_CURSOR_FREQUENCY, "60");
    SDL_SetHint(SDL_HINT_WEBOS_CURSOR_CALIBRATION_DISABLE, "true");
    SDL_SetHint(SDL_HINT_WEBOS_HIDAPI_IGNORE_BLUETOOTH_DEVICES, "0x057e/0x0000");
    if (app->settings.syskey_capture) {
        SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_HOME, "true");
        SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_RIBBON, "false");
    }
#else
    if (!headless_secondary && app->settings.syskey_capture) {
        SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
    }
#endif
    /* A co-op helper is a network/decode worker, not a second desktop app.
     * CREATE_NO_WINDOW suppresses only a console on Windows; SDL_CreateWindow
     * would still make a visible Eclipse window and could contaminate capture.
     * Keep all UI/video initialization out of the helper process. */
    if (!headless_secondary && SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
        commons_log_fatal("APP", "Failed to initialize SDL video subsystem: %s", SDL_GetError());
        return -1;
    }
#if !defined(TARGET_WEBOS)
    if (!headless_secondary && !app->settings.settings_save_allowed) {
        (void) SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_ERROR,
                "Eclipse settings need attention",
                "Eclipse could not read moonlight.ini. The existing file will not be overwritten. "
                "Check its permissions or syntax, then restart Eclipse.",
                NULL);
    }
#endif
    // This will occupy SDL_USEREVENT
    SDL_RegisterEvents(1);
    commons_log_info("APP", "UI locale: %s (%s)", i18n_locale(), locstr("[Localized Language]"));

    if (!headless_secondary) {
        app_input_init(&app->input, app);
        /* Only for the co-op mouse/keyboard routing: a process-wide keyboard listener is not
         * started for users who never assign them (the dialog starts it too, for a live toggle). */
        if (app->settings.coop_kbm_assign) {
            input_names_start();
        }
        app_ui_init(&app->ui, app);
#if !defined(TARGET_WEBOS) && !defined(TARGET_ANDROID)
        /* Ask the display which actually owns the restored window. Display 0 is not
         * necessarily the monitor the user selected, especially when Eclipse starts
         * fullscreen on a saved secondary monitor. settings_initialize() cannot do
         * this because SDL video is intentionally initialised later. */
        if (app->settings.stream_size_defaulted) {
            int display = SDL_GetWindowDisplayIndex(app->ui.window);
            if (display < 0) {
                display = 0;
            }
            SDL_DisplayMode mode;
            if (SDL_GetDesktopDisplayMode(display, &mode) == 0 && mode.w > 0 && mode.h > 0) {
                app->settings.stream.width = mode.w;
                app->settings.stream.height = mode.h;
                settings_refresh_auto_bitrate(&app->settings, &app->ss4s.video_cap);
                commons_log_info("APP", "Stream size defaulted to display %d: %dx%d",
                                 display, mode.w, mode.h);
            }
        }
#endif
    }

    global = app;

    vendor_diag_hang_arm("SS4S_PostInit");
    SS4S_PostInit(argc, argv);
    vendor_diag_hang_disarm("returned");
    if (diag_logs_active() && !headless_secondary) {
        diag_report_log(app);   /* every kept log carries the environment it ran in (util/diag_report.h) */
        crash_log_watch_ui();   /* a frozen UI thread leaves its stack in the log (util/crash_log.h) */
#if !defined(TARGET_WEBOS) && defined(AURORA_TEST_HOOKS)
        /* TEST ONLY: ECLIPSE_TEST_CRASH=1 crashes on purpose here, so crash capture and log recovery can be tested
         * on a desktop build (webOS: the beta-only aurora_test_crash launch parameter). Only with a kept log. */
        const char *test_crash = getenv("ECLIPSE_TEST_CRASH");
        if (test_crash != NULL && strcmp(test_crash, "1") == 0) {
            commons_log_warn("APP", "Test crash requested (ECLIPSE_TEST_CRASH=1): crashing now");
            volatile int *nowhere = NULL;
            *nowhere = 1;
        }
        /* TEST ONLY: ECLIPSE_TEST_HANG=N stalls the UI thread N seconds on its first main-loop pass. */
        const char *test_hang = getenv("ECLIPSE_TEST_HANG");
        if (test_hang != NULL && atoi(test_hang) > 0) {
            app_test_hang_request((unsigned) atoi(test_hang));
        }
#endif
    }
    return 0;
}

void app_deinit(app_t *app) {
    const bool headless_secondary = coop_secondary_mode() != NULL;
    crash_log_ui_shutdown();   /* the main loop has stopped on purpose; a close that hangs is still recorded */
    diag_logs_share_stop();   /* window close need not go through the share dialog's button */
    app_bus_drain();
    app_session_destroy(app);
    if (!headless_secondary) {
        app_ui_close(&app->ui);
        app_ui_deinit(&app->ui);
        app_set_keep_awake(app, false);
        app_input_deinit(&app->input);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    backend_destroy(&app->backend);

    /* The helper loads the same moonlight.ini as the primary but owns none of
     * its settings. Writing its startup snapshot here can overwrite a setting
     * the user changed in the primary, or race the primary's truncate/write. */
    if (!headless_secondary) {
        settings_save(&app->settings);
    }
    settings_clear(&app->settings);

#if FEATURE_INPUT_LIBCEC
    cec_sdl_deinit(&app->cec);
#endif
    SS4S_Quit();

    SS4S_ModulesListClear(&app->ss4s.modules);
    os_info_clear(&app->os_info);

    _lv_draw_mask_cleanup();

    SDL_Quit();

    commons_logging_deinit();
    if (!headless_secondary) {
        diag_logs_run_end();   /* the last step: reaching it is what "ended cleanly" means */
    }
}

void app_run_loop(app_t *app) {
    crash_log_ui_alive();
    if (test_hang_s != 0) {
        const unsigned seconds = test_hang_s;
        test_hang_s = 0;
        commons_log_warn("APP", "Test hang requested: stalling the UI thread for %u s", seconds);
        SDL_Delay(seconds * 1000);
    }
    const uint32_t start = input_trace_enabled ? SDL_GetTicks() : 0;
    /* The Windows input link is a framed non-blocking stream. Drive accept,
     * returned rumble, and POLLOUT progress even when no new SDL input arrives,
     * so the final release packet cannot remain buffered indefinitely. */
    coop_input_coord_pump();
    app_process_events(app);
    const uint32_t pumped = input_trace_enabled ? SDL_GetTicks() : 0;
    lv_task_handler();
    /* Present decoded video. It arrives on the decode thread at the stream's rate, which
     * has nothing to do with when LVGL decides to repaint -- and LVGL will not repaint at
     * all while the streaming scene sits still, so relying on its flush would show one
     * frame and then freeze. lv_app_redraw_now() re-presents the window without
     * re-rendering the UI: the module draws the frame underneath and the existing UI
     * texture is blended over it. Gated on a frame actually being pending because this
     * renderer has no vsync (lv_app_disp_drv_create) -- unconditionally it would spin. */
    /* Poll even after app->session is cleared: the desktop decoder may have transferred
     * an SDL texture to its UI-thread retirement queue during worker cleanup. Its pending
     * hook stays alive until SS4S_Quit and must get one final main-thread draw to destroy
     * that texture safely. */
    if (ui_stream_frame_pending()) {
        lv_disp_t *disp = lv_disp_get_default();
        if (disp != NULL) {
            lv_app_redraw_now(disp->driver);
        }
    }
    if (input_trace_enabled && app->session != NULL) {
        const uint32_t now = SDL_GetTicks();
        const uint32_t gap = input_trace.last_loop ? start - input_trace.last_loop : 0;
        if (gap > input_trace.gap_max) input_trace.gap_max = gap;
        if (pumped - start > input_trace.pump_max) input_trace.pump_max = pumped - start;
        if (now - pumped > input_trace.ui_max) input_trace.ui_max = now - pumped;
        if (now - input_trace.report_at >= 2000) {
            FILE *f = fopen(AURORA_DIAG_LOG, "a");
            if (f != NULL) {
                fprintf(f, "[INPUT-UI] t=%u gap_max=%u pump_max=%u ui_max=%u "
                           "mouse[n=%u age_max=%u dispatch_max=%u] "
                           "pad[n=%u age_max=%u dispatch_max=%u] "
                           "other[n=%u age_max=%u dispatch_max=%u]\n",
                        now, input_trace.gap_max, input_trace.pump_max, input_trace.ui_max,
                        input_trace.count[0], input_trace.age_max[0], input_trace.dispatch_max[0],
                        input_trace.count[1], input_trace.age_max[1], input_trace.dispatch_max[1],
                        input_trace.count[2], input_trace.age_max[2], input_trace.dispatch_max[2]);
                fclose(f);
            }
            memset(&input_trace, 0, sizeof(input_trace));
            input_trace.report_at = now;
        }
        input_trace.last_loop = start;
    } else {
        input_trace.last_loop = 0;
    }
    SDL_Delay(1);
}

static int app_event_filter(void *userdata, SDL_Event *event) {
    app_t *app = userdata;
    switch (event->type) {
        case SDL_APP_WILLENTERBACKGROUND: {
            // Interrupt streaming because app will go to background
            if (app_ui_is_opened(&app->ui) && app->session != NULL) {
                session_interrupt(app->session, false, STREAMING_INTERRUPT_BACKGROUND);
            }
#if TARGET_ANDROID
            /* SAVE NOW: ANDROID ENDS A BACKGROUND APP WITHOUT A WORD. Settings and the known hosts are written
             * on clean shutdown, which an Android TV app almost never gets -- the SHIELD TV SIGKILLs it soon
             * after it leaves the screen. A host paired since the last start was then lost, and so was the last
             * co-op pair, so a paused co-op session could not be resumed from its pane (2026-10-16). */
            settings_save(&app->settings);
            if (pcmanager != NULL) {
                pcmanager_save_known_hosts(pcmanager);
            }
#endif
            break;
        }
        case SDL_APP_DIDENTERFOREGROUND: {
            lv_obj_invalidate(lv_scr_act());
            break;
        }
        case SDL_WINDOWEVENT: {
            switch (event->window.event) {
                case SDL_WINDOWEVENT_FOCUS_GAINED:
                    app->focused = true;
                    /* A keyboard assigned to this PC's own co-op pane goes straight back to its game. */
                    local_input_focus_gained();
#if !FEATURE_FORCE_FULLSCREEN
                    if (app->session != NULL && session_has_input(app->session)) {
                        app_set_mouse_grab(&app->input, true);
                    }
#endif
                    break;
                case SDL_WINDOWEVENT_FOCUS_LOST:
                    commons_log_debug("SDL", "Window event SDL_WINDOWEVENT_FOCUS_LOST");
#if !FEATURE_FORCE_FULLSCREEN
                    /* Release the pointer; do NOT end the session. This used to call
                     * session_interrupt(BACKGROUND), and on Windows that made a local
                     * stream impossible to start at all: Apollo switches the display mode
                     * for the client a second after /launch, the mode change moves focus
                     * off this window, and the session was torn down before the first
                     * frame -- reported as "No video traffic was ever received from the
                     * host" and "Control stream connection failed: 10004", which both look
                     * like the host failed and neither was. Measured 2026-09-22: two
                     * FOCUS_LOST events ~1.2s and ~1.8s after launch, every attempt.
                     *
                     * Releasing instead of disconnecting is what moonlight-qt does on the
                     * same event (Session::handleWindowEvent), and it is the behaviour a
                     * desktop client needs anyway: alt-tabbing out of a stream should give
                     * the mouse back, not kill the game. webOS never reaches this code --
                     * FEATURE_FORCE_FULLSCREEN is ON there, and losing focus on a TV really
                     * does mean the app was backgrounded and the decoder taken away. */
                    if (app->session != NULL && session_has_input(app->session)) {
                        app_set_mouse_grab(&app->input, false);
                    }
#endif
                    app->focused = false;
                    break;
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    lv_app_display_resize(lv_disp_get_default(), event->window.data1, event->window.data2);
                    ui_display_size(&app->ui, (int) event->window.data1, (int) event->window.data2);
#if !defined(TARGET_WEBOS)
                    /* The pointer's reference size was latched when the session started, so an absolute
                     * mouse (and the co-op split) mapped a resized or un-fullscreened window as if it were
                     * still the old size. streaming_display_size() existed for this and had no caller. */
                    if (app->session != NULL) {
                        streaming_display_size(app->session, (short) event->window.data1,
                                               (short) event->window.data2);
                    }
#endif
                    bus_pushevent(USER_SIZE_CHANGED, NULL, NULL);
                    break;
                case SDL_WINDOWEVENT_HIDDEN:
                    commons_log_debug("SDL", "Window event SDL_WINDOWEVENT_HIDDEN");
                    if (app_configuration->fullscreen && app_ui_is_opened(&app->ui) && app->session != NULL) {
                        // Interrupt streaming because app will go to background
                        session_interrupt(app->session, false, STREAMING_INTERRUPT_BACKGROUND);
                    }
                    break;
                case SDL_WINDOWEVENT_EXPOSED: {
                    lv_obj_invalidate(lv_scr_act());
                    break;
                }
                case SDL_WINDOWEVENT_CLOSE: {
                    if (app_ui_is_opened(&app->ui)) {
                        app_request_exit();
                    }
                    break;
                }
                default:
                    break;
            }
            break;
        }
        case SDL_USEREVENT: {
            if (event->user.code == BUS_INT_EVENT_ACTION) {
                bus_actionfunc actionfn = event->user.data1;
                actionfn(event->user.data2);
            } else if (event->user.code == USER_INPUT_CONTROLLERDB_UPDATED) {
                app_input_handle_event(&app->input, event);
            } else {
                bool handled = backend_dispatch_userevent(&app->backend, event->user.code, event->user.data1,
                                                          event->user.data2);
                handled = handled || ui_dispatch_userevent(app, event->user.code, event->user.data1, event->user.data2);
                if (!handled) {
                    if (event->user.code & USER_EVENT_FLAG_FREE_DATA1 && event->user.data1 != NULL) {
                        free(event->user.data1);
                    }
                    if (event->user.code & USER_EVENT_FLAG_FREE_DATA2 && event->user.data2 != NULL) {
                        free(event->user.data2);
                    }
                }
            }
            break;
        }
        case SDL_QUIT: {
            app_request_exit();
            break;
        }
        case SDL_JOYDEVICEREMOVED:
        case SDL_CONTROLLERDEVICEREMOVED: {
            if (app->session != NULL) {
                session_handle_input_event(app->session, event);
            }
            app_input_handle_event(&app->input, event);
            return 0;
        }
#if TARGET_ANDROID
        case SDL_JOYBUTTONDOWN: {
            /* The raw joystick button a press arrives as, before the controller mapping (input_gamepad.c logs that
             * mapping): which index a Switch Pro Controller's + becomes on the SHIELD TV (2026-10-22). Capped. */
            static unsigned joy_button_lines;
            if (joy_button_lines < 40) {
                joy_button_lines++;
                commons_log_info("Input", "Joystick instance %d raw button %d down", (int) event->jbutton.which,
                                 event->jbutton.button);
            }
            break;
        }
#endif
        case SDL_JOYDEVICEADDED:
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMAPPED: {
            app_input_handle_event(&app->input, event);
            if (app->session != NULL) {
                session_handle_input_event(app->session, event);
                return 0;
            }
            break;
        }
        case SDL_KEYDOWN:
        case SDL_KEYUP:
        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEWHEEL:
        case SDL_CONTROLLERAXISMOTION:
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
        case SDL_CONTROLLERTOUCHPADDOWN:
        case SDL_CONTROLLERTOUCHPADMOTION:
        case SDL_CONTROLLERTOUCHPADUP:
        case SDL_CONTROLLERSENSORUPDATE:
        case SDL_TEXTINPUT: {
            const uint32_t input_start = input_trace_enabled ? SDL_GetTicks() : 0;
            if (event->type == SDL_MOUSEMOTION) {
                bool updated = app_text_input_state_update(&app->ui.input);
                if (updated && !app->ui.input.text_input_active && app->session != NULL) {
                    session_screen_keyboard_closed(app->session);
                }
            }
            if (!app_ui_is_opened(&app->ui) && app->session != NULL) {
                session_handle_input_event(app->session, event);
                if (input_trace_enabled) input_trace_event(event, input_start);
                return 0;
            }
            return 1;
        }
        case SDL_FINGERDOWN:
        case SDL_FINGERUP:
        case SDL_FINGERMOTION: {
            if (app->session != NULL) {
                session_handle_input_event(app->session, event);
                return 0;
            }
            return 1;
        }
        default:
            if (event->type == USER_REMOTEBUTTONEVENT) {
                return 1;
            }
            return 0;
    }
    return 0;
}

void app_process_events(app_t *app) {
    SDL_PumpEvents();
#if TARGET_ANDROID
    discovery_events_dispatching(true);
#endif
    SDL_FilterEvents(app_event_filter, app);
#if TARGET_ANDROID
    discovery_events_dispatching(false);
#endif
}

void app_quit_confirm() {
    static const char *btn_txts[] = {translatable("Cancel"), translatable("OK"), ""};
    lv_obj_t *mbox = lv_msgbox_create_i18n(NULL, NULL, locstr("Quit Eclipse?"), btn_txts, false);
    lv_obj_add_event_cb(mbox, quit_confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(mbox);
}

void app_request_exit() {
    global->running = false;
}

bool app_is_running() {
    return global->running;
}

bool app_is_decoder_valid(app_t *app) {
    return app->ss4s.selection.video_module != NULL && app->ss4s.selection.audio_module != NULL;
}

#if FEATURE_EMBEDDED_SHELL
bool app_has_embedded(app_t *app) {
    return version_info_valid(&app->embed_version);
}

bool app_decoder_or_embedded_present(app_t *app) {
    return app_is_decoder_valid(app) || app_has_embedded(app);
}
#endif

static void libs_init(app_t *app, int argc, char *argv[]) {
    SS4S_SetLoggingFunction(commons_ss4s_logf);
    int ss4s_error;
    if ((ss4s_error = SS4S_ModulesList(&app->ss4s.modules, &app->os_info)) != 0) {
        commons_log_error("SS4S", "Can't load modules list: %s", strerror(ss4s_error));
    }
    SS4S_ModulePreferences module_preferences = {
            .audio_module = app_configuration->audio_backend,
            .video_module = app_configuration->decoder,
    };
    SS4S_ModulesSelect(&app->ss4s.modules, &module_preferences, &app->ss4s.selection, true);
    commons_log_info("APP", "Video module: %s (requested %s)", SS4S_ModuleInfoGetName(app->ss4s.selection.video_module),
                     module_preferences.video_module);
    commons_log_info("APP", "Audio module: %s (requested %s)", SS4S_ModuleInfoGetName(app->ss4s.selection.audio_module),
                     module_preferences.audio_module);

#if FEATURE_EMBEDDED_SHELL
    if (!app_is_decoder_valid(app)) {
        // Check if moonlight-embedded is installed
        if (embed_check_version(&app->embed_version) == 0) {
            char *embed_version_str = version_info_str(&app->embed_version);
            commons_log_info("APP", "Moonlight Embedded version: %s", embed_version_str);
            free(embed_version_str);
        }
    }
#endif

    SS4S_Config ss4s_config = {
            .audioDriver = SS4S_ModuleInfoGetId(app->ss4s.selection.audio_module),
            .videoDriver = SS4S_ModuleInfoGetId(app->ss4s.selection.video_module),
            .audioDevice = app_configuration->audio_device,
    };
    SS4S_Init(argc, argv, &ss4s_config);

    SS4S_GetAudioCapabilitiesByCodecs(&app->ss4s.audio_cap, SS4S_AUDIO_PCM_S16LE | SS4S_AUDIO_OPUS);
    SS4S_GetVideoCapabilities(&app->ss4s.video_cap);


#if FEATURE_INPUT_LIBCEC
    cec_sdl_init(&app->cec, "Eclipse");
#endif
}

static void quit_confirm_cb(lv_event_t *e) {
    lv_obj_t *mbox = lv_event_get_current_target(e);
    if (lv_msgbox_get_active_btn(mbox) == 1) {
        app_request_exit();
    } else {
        lv_msgbox_close_async(mbox);
    }
}

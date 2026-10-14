#include "stream/wired_nic.h"
#include "backend/aurora_host_caps.h"
#include "app.h"
#include "config.h"

#include "pref_obj.h"
#include "pref_fps.h"
#include "pref_res.h"
#include "ui/settings/settings.controller.h"

#include "util/i18n.h"
#include "logging.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
    lv_fragment_t base;
    settings_controller_t *parent;

    lv_obj_t *res_warning;
    lv_obj_t *bitrate_label;
    lv_obj_t *bitrate_slider;
    lv_obj_t *bitrate_warning;

    pref_dropdown_string_entry_t *lang_entries;
    int lang_entries_len;
} basic_pane_t;

static void pane_ctor(lv_fragment_t *self, void *args);

static void pane_dtor(lv_fragment_t *self);

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container);

static void on_bitrate_changed(lv_event_t *e);

static void on_res_fps_updated(lv_event_t *e);

static void on_res_chosen(lv_event_t *e);

static void on_fullscreen_updated(lv_event_t *e);

static void mark_stream_reconnect_cb(lv_event_t *e);

static void update_bitrate_label(basic_pane_t *pane);

#if FEATURE_I18N_LANGUAGE_SETTINGS
static void init_locale_entries(basic_pane_t *pane);

static void pref_mark_restart_cb(lv_event_t *e);
#endif

static void update_bitrate_hint(basic_pane_t *pane);

const lv_fragment_class_t settings_pane_basic_cls = {
    .constructor_cb = pane_ctor,
    .destructor_cb = pane_dtor,
    .create_obj_cb = create_obj,
    .instance_size = sizeof(basic_pane_t),
};
#define BITRATE_STEP 1000

static void pane_ctor(lv_fragment_t *self, void *args) {
    basic_pane_t *pane = (basic_pane_t *) self;
    pane->parent = args;
#if FEATURE_I18N_LANGUAGE_SETTINGS
    init_locale_entries(pane);
#endif
}

static void pane_dtor(lv_fragment_t *self) {
    basic_pane_t *pane = (basic_pane_t *) self;
#if FEATURE_I18N_LANGUAGE_SETTINGS
    lv_mem_free(pane->lang_entries);
#endif
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container) {
    basic_pane_t *pane = (basic_pane_t *) self;
    settings_controller_t *parent = pane->parent;
    app_t *app = parent->app;
    lv_obj_t *view = pref_pane_container(container);
    lv_obj_set_layout(view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_ROW_WRAP);
    pref_title_label(view, locstr("Resolution and FPS"));


    int max_width = (int) app->ss4s.video_cap.maxWidth, max_height = (int) app->ss4s.video_cap.maxHeight;
    int native_width = app->ui.width, native_height = app->ui.height;

#if TARGET_WEBOS || TARGET_ANDROID
    if (parent->panel_width > 0 && parent->panel_height > 0) {
        native_width = parent->panel_width;
        native_height = parent->panel_height;
    }

    commons_log_info("Settings", "Panel native resolution: %d x %d, maximum video resolution: %d x %d",
                     native_width, native_height, max_width, max_height);
#else
    int display_hz = 0;   /* the refresh rate of the monitor the window is on, read each time */
    if (app->ui.window != NULL) {
        const int display = SDL_GetWindowDisplayIndex(app->ui.window);
        SDL_DisplayMode mode;
        if (display >= 0 && SDL_GetDesktopDisplayMode(display, &mode) == 0 && mode.w > 0 && mode.h > 0) {
            native_width = mode.w;
            native_height = mode.h;
            display_hz = mode.refresh_rate;
        }
    }
#endif
    if (max_width == 0 || max_height == 0) {
        max_width = native_width;
        max_height = native_height;
    }

    lv_obj_t *res_dropdown = pref_dropdown_res(view, native_width, native_height, max_width, max_height,
                                               &app_configuration->stream.width, &app_configuration->stream.height);
    lv_obj_set_width(res_dropdown, LV_PCT(60));
    lv_obj_add_event_cb(res_dropdown, on_res_fps_updated, LV_EVENT_VALUE_CHANGED, self);
    lv_obj_add_event_cb(res_dropdown, on_res_chosen, LV_EVENT_VALUE_CHANGED, NULL);

    unsigned int max_fps = app->ss4s.video_cap.maxFps;
#if TARGET_WEBOS || TARGET_ANDROID
    if (parent->panel_fps > 0 && (max_fps == 0 || parent->panel_fps < max_fps)) {
        max_fps = parent->panel_fps;
    }
#else
    /* THE MONITOR'S RATE, NOT A FIXED 60 (issue #24). The desktop decoder module reports no cap
     * (maxFps 0), so pref_dropdown_fps fell back to 60 and a 165 Hz monitor could not be chosen
     * -- nor could co-op panes, whose virtual displays run at the stream rate. Capped by the
     * monitor the window is on now, so the list follows the window between monitors. */
    if (display_hz > 0 && (max_fps == 0 || (unsigned int) display_hz < max_fps)) {
        max_fps = (unsigned int) display_hz;
    }
#endif
    const static int fps_options[] = {30, 60, 90, 120, 144, 240, 0};
    lv_obj_t *fps_dropdown = pref_dropdown_fps(view, fps_options, (int) max_fps, &app_configuration->stream.fps,
                                               &app_configuration->client_refresh_rate_x100);
    lv_obj_set_flex_grow(fps_dropdown, 1);
    lv_obj_add_event_cb(fps_dropdown, on_res_fps_updated, LV_EVENT_VALUE_CHANGED, self);

    pref_desc_label(view,
                    locstr("Tip: choose Custom FPS to enter a fractional refresh rate (e.g. 119.94 for VRR game "
                           "mode). The exact value is sent to the host for frame pacing."),
                    false);

    pane->res_warning = lv_label_create(view);
    lv_obj_add_flag(pane->res_warning, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(pane->res_warning, LV_PCT(100));
    lv_obj_set_style_text_font(pane->res_warning, lv_theme_get_font_small(view), 0);
    lv_obj_set_style_text_color(pane->res_warning, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_label_set_long_mode(pane->res_warning, LV_LABEL_LONG_WRAP);

    pane->bitrate_label = pref_title_label(view, locstr("Video bitrate"));

    /* User-facing slider cap. Set to 400 Mbps for high-end Sunshine setups; the SMP driver
     * advertises a matching maxBitrate so session.c will not silently clamp this value. */
    unsigned int max = 400000;
    lv_obj_t *bitrate_slider = pref_slider(view, &app_configuration->stream.bitrate, 5000, (int) max, BITRATE_STEP);
    lv_obj_set_width(bitrate_slider, LV_PCT(100));
    lv_obj_add_event_cb(bitrate_slider, on_bitrate_changed, LV_EVENT_VALUE_CHANGED, self);
    pane->bitrate_slider = bitrate_slider;

    pane->bitrate_warning = lv_label_create(view);
    lv_obj_add_flag(pane->bitrate_warning, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(pane->bitrate_warning, LV_PCT(100));
    lv_obj_set_style_text_font(pane->bitrate_warning, lv_theme_get_font_small(view), 0);
    lv_obj_set_style_text_color(pane->bitrate_warning, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_label_set_long_mode(pane->bitrate_warning, LV_LABEL_LONG_WRAP);

#if TARGET_WEBOS
    /* WIRED NETWORK (app_settings.h wired_pin_mode). Host-dependent, so a tri-state whose note says what was
     * found (standing policy, aurora_host_caps.h). */
    static const pref_dropdown_int_entry_t wired_entries[] = {
            {.name = "Automatic (when the PC offers it)", .value = WIRED_PIN_AUTO},
            {.name = "Off", .value = WIRED_PIN_OFF},
            {.name = "On", .value = WIRED_PIN_ON},
    };
    pref_title_label(view, locstr("Use the wired network (USB Ethernet adapter)"));
    lv_obj_t *wired = pref_dropdown_int(view, wired_entries, sizeof(wired_entries) / sizeof(wired_entries[0]),
                                        &app_configuration->wired_pin_mode, NULL);
    lv_obj_set_width(wired, LV_PCT(100));
    {
        wired_nic_t nic;
        const bool have_wired = wired_nic_discover(&nic);
        const char *offered_by = umbra_wired_pin_host_name();
        char note[320];
        snprintf(note, sizeof(note), locstr("Keeps the stream on the TV's wired adapter when Wi-Fi is also on (needs Umbra "
                                            "on the PC). Wired adapter: %s. %s%s"),
                 have_wired ? nic.name : locstr("none is up"),
                 offered_by != NULL ? locstr("Offered by ") : locstr("No paired PC offers it yet."),
                 offered_by != NULL ? offered_by : "");
        pref_desc_label(view, note, false);
    }
#endif

#if !FEATURE_FORCE_FULLSCREEN
    lv_obj_t *checkbox = pref_checkbox(view, locstr("Fullscreen UI"), &app_configuration->fullscreen, false);
    /* A traditional platform surface needs AREA_DEST so the app can move it with
     * the window.  The desktop FFmpeg backend renders into the UI renderer itself;
     * it follows the SDL window without SetDisplayArea, so UI_COMPOSITING is an
     * equally valid windowed-mode capability. */
    if (app->ss4s.video_cap.transform &
        (SS4S_VIDEO_CAP_TRANSFORM_AREA_DEST | SS4S_VIDEO_CAP_TRANSFORM_UI_COMPOSITING)) {
        lv_obj_add_event_cb(checkbox, on_fullscreen_updated, LV_EVENT_VALUE_CHANGED, pane);
    } else {
        lv_obj_add_state(checkbox, LV_STATE_DISABLED);
        pref_desc_label(view, locstr("Can't use windowed UI for this decoder"), false);
    }
#endif

    lv_obj_t *show_stats_checkbox = pref_checkbox(view, locstr("Show performance stats on stream start"),
                                                   &app_configuration->show_stats_on_start, false);
    lv_obj_add_event_cb(show_stats_checkbox, mark_stream_reconnect_cb, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Start streaming with performance overlay visible and pinned."), false);

    lv_obj_t *show_stats_compact_checkbox = pref_checkbox(view, locstr("Compact performance stats (single line)"),
                                                          &app_configuration->show_stats_compact, false);
    lv_obj_add_event_cb(show_stats_compact_checkbox, mark_stream_reconnect_cb, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Show minimalist one-line stats like Moonlight Android (FPS, RTT, bitrate)."), false);

#if FEATURE_I18N_LANGUAGE_SETTINGS
    pref_title_label(view, locstr("Language"));

    lv_obj_t *language_dropdown = pref_dropdown_string(view, pane->lang_entries, pane->lang_entries_len,
                                                       &app_configuration->language);
    lv_obj_add_event_cb(language_dropdown, pref_mark_restart_cb, LV_EVENT_VALUE_CHANGED, pane);
    lv_obj_set_width(language_dropdown, LV_PCT(100));
#endif

    update_bitrate_label(pane);
    update_bitrate_hint(pane);

    return view;
}

static void on_bitrate_changed(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    app_configuration->stream_bitrate_defaulted = false;
    pane->parent->needs_stream_reconnect = true;
    update_bitrate_label(pane);
    update_bitrate_hint(pane);
}

/* Bound to the resolution dropdown ONLY -- on_res_fps_updated() is shared with the FPS one.
 * Picking a resolution makes it the user's, not the display-derived default, and
 * settings_write() deliberately persists only a size the user chose. */
static void on_res_chosen(lv_event_t *e) {
    LV_UNUSED(e);
    app_configuration->stream_size_defaulted = false;
}

static void on_res_fps_updated(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    pane->parent->needs_stream_reconnect = true;
    /* Follow Moonlight's default curve only while the value is still automatic. Once the
     * user moves the slider, that value is theirs and resolution/FPS changes leave it alone. */
    if (app_configuration->stream_bitrate_defaulted) {
        settings_refresh_auto_bitrate(app_configuration, &pane->parent->app->ss4s.video_cap);
        lv_slider_set_value(pane->bitrate_slider,
                            app_configuration->stream.bitrate / BITRATE_STEP,
                            LV_ANIM_OFF);
    }
    if (app_configuration->stream.width > 1920 && app_configuration->stream.height > 1080 &&
        app_configuration->stream.fps > 60) {
        lv_obj_clear_flag(pane->res_warning, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_static(pane->res_warning, locstr("Your computer may not perform well when using this "
                                                           "resolution and framerate."));
    } else {
        lv_obj_add_flag(pane->res_warning, LV_OBJ_FLAG_HIDDEN);
    }
    update_bitrate_label(pane);
    update_bitrate_hint(pane);
}

static void on_fullscreen_updated(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    if (!app_set_fullscreen(pane->parent->app, app_configuration->fullscreen)) {
        lv_obj_t *checkbox = lv_event_get_current_target(e);
        if (app_configuration->fullscreen) {
            lv_obj_add_state(checkbox, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(checkbox, LV_STATE_CHECKED);
        }
    }
}

static void mark_stream_reconnect_cb(lv_event_t *e) {
    basic_pane_t *pane = lv_event_get_user_data(e);
    pane->parent->needs_stream_reconnect = true;
}

static void update_bitrate_label(basic_pane_t *pane) {
    lv_label_set_text_fmt(pane->bitrate_label, locstr("Video bitrate - %d kbps"), app_configuration->stream.bitrate);
}

static void update_bitrate_hint(basic_pane_t *pane) {
    app_t *app = pane->parent->app;
    if (app->ss4s.video_cap.suggestedBitrate > 0 &&
        app_configuration->stream.bitrate > app->ss4s.video_cap.suggestedBitrate) {
        lv_obj_clear_flag(pane->bitrate_warning, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_static(pane->bitrate_warning, locstr("Higher bitrate may cause performance issue, "
                                                               "try with caution."));
    } else {
        lv_obj_add_flag(pane->bitrate_warning, LV_OBJ_FLAG_HIDDEN);
    }
}

#if FEATURE_I18N_LANGUAGE_SETTINGS
static void init_locale_entries(basic_pane_t *pane) {
    pane->lang_entries = lv_mem_alloc(sizeof(pref_dropdown_string_entry_t) * (I18N_LOCALES_LEN + 2));
    lv_memset_00(pane->lang_entries, sizeof(pref_dropdown_string_entry_t) * (I18N_LOCALES_LEN + 2));
    for (int i = 0; i < 2; i++) {
        pref_dropdown_string_entry_t *def_entry = &pane->lang_entries[i];
        const i18n_entry_t *entry = i18n_entry_at(i);
        def_entry->value = entry->locale;
        def_entry->name = locstr(entry->name);
        def_entry->fallback = i == 0;
        pane->lang_entries_len++;
    }
    char *input = strdup(I18N_LOCALES), *tok = NULL, *saveptr = input;
    while ((tok = strtok_r(saveptr, ";", &saveptr)) != NULL) {
        const i18n_entry_t *entry = i18n_entry(tok);
        if (entry) {
            pref_dropdown_string_entry_t *pref_entry = &pane->lang_entries[pane->lang_entries_len];
            pref_entry->value = entry->locale;
            pref_entry->name = entry->name;
            pane->lang_entries_len++;
        }
    }
    free(input);
}

static void pref_mark_restart_cb(lv_event_t *e) {
    basic_pane_t *pane = (basic_pane_t *) lv_event_get_user_data(e);
    settings_controller_t *parent = pane->parent;
    parent->needs_locale_reapply = strcasecmp(i18n_locale(), app_configuration->language) != 0;
}
#endif

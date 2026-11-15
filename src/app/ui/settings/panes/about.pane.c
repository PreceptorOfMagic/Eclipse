#include "app.h"
#include "lvgl.h"
#include "pref_obj.h"
#include "ui/settings/settings.controller.h"

#include <stdlib.h>
#include <string.h>

#include "util/i18n.h"
#include "util/diag_logs.h"
#include "util/diag_report.h"
#include "lvgl/util/lv_app_utils.h"
#include "os_info.h"
#include "backend/update_check.h"
#include "ui/common/update_dot.h"


#define MAXIMUM_ROWS 32

typedef struct about_pane_t {
    lv_fragment_t base;
    settings_controller_t *parent;

    lv_coord_t row_dsc[MAXIMUM_ROWS + 1];
} about_pane_t;

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container);

static void pane_ctor(lv_fragment_t *self, void *args);

const lv_fragment_class_t settings_pane_about_cls = {
        .constructor_cb = pane_ctor,
        .create_obj_cb = create_obj,
        .instance_size = sizeof(about_pane_t),
};

static void pane_ctor(lv_fragment_t *self, void *args) {
    about_pane_t *controller = (about_pane_t *) self;
    controller->parent = args;
}

static inline void about_line(lv_obj_t *parent, const char *title, const char *text, int row, int text_span) {
    LV_ASSERT(text_span < 4);
    lv_obj_t *title_label = lv_label_create(parent);
    lv_obj_t *text_label = lv_label_create(parent);
    lv_label_set_text(title_label, title);
    lv_obj_set_grid_cell(title_label, LV_GRID_ALIGN_START, 0, 4 - text_span, LV_GRID_ALIGN_CENTER, row, 1);
    lv_label_set_text(text_label, text != NULL ? text : "(null)");
    lv_obj_set_grid_cell(text_label, LV_GRID_ALIGN_END, 4 - text_span, text_span, LV_GRID_ALIGN_CENTER, row, 1);
}

static const char *module_name(const SS4S_ModuleInfo *info);

static void share_logs_cb(lv_event_t *e);

static void whats_new_cb(lv_event_t *e);

static inline void about_full_row(lv_obj_t *obj, int row) {
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_STRETCH, 0, 4, LV_GRID_ALIGN_CENTER, row, 1);
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container) {
    about_pane_t *controller = (about_pane_t *) self;
    settings_controller_t *parent = controller->parent;
    app_t *app = parent->app;

    lv_obj_t *view = pref_pane_container(container);
    lv_obj_set_layout(view, LV_LAYOUT_GRID);
    static const lv_coord_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                         LV_GRID_TEMPLATE_LAST};
    /* THE ROW ARRAY MUST BE TERMINATED BEFORE ANY CHILD IS ADDED. LVGL can lay the grid out while the rows
     * below are still being created, and it walks row_dsc until LV_GRID_TEMPLATE_LAST: unterminated, it ran
     * off the array (Windows: access violation in grid_update, 2026-09-29, once the pane had a checkbox and
     * a button). Every row is "content" until the real count is known at the end. */
    for (int i = 0; i < MAXIMUM_ROWS; i++) {
        controller->row_dsc[i] = LV_GRID_CONTENT;
    }
    controller->row_dsc[MAXIMUM_ROWS] = LV_GRID_TEMPLATE_LAST;
    lv_obj_set_grid_dsc_array(view, col_dsc, controller->row_dsc);
    int rowcount = 0;
    about_line(view, locstr("Version"), APP_VERSION, rowcount++, 1);
    /* THE UPDATE NOTICE (backend/update_check.h): one line and a button, only when there is something. */
    const update_info_t *update = update_check_info();
    if (update->available && app->settings.check_updates) {
        static char available_text[96];
        snprintf(available_text, sizeof(available_text), locstr("Eclipse %s is available"),
                 update->latest_version);
        lv_obj_t *available = lv_label_create(view);
        lv_label_set_text_static(available, available_text);
        lv_obj_set_grid_cell(available, LV_GRID_ALIGN_START, 0, 2, LV_GRID_ALIGN_CENTER, rowcount, 1);
        lv_obj_t *whats_new = lv_btn_create(view);
        lv_obj_t *whats_new_label = lv_label_create(whats_new);
        lv_label_set_text(whats_new_label, locstr("What's new"));
        lv_obj_center(whats_new_label);
        lv_obj_set_grid_cell(whats_new, LV_GRID_ALIGN_END, 2, 2, LV_GRID_ALIGN_CENTER, rowcount++, 1);
        lv_obj_add_event_cb(whats_new, whats_new_cb, LV_EVENT_CLICKED, parent);
    }
    about_line(view, locstr("Video decoder"), module_name(app->ss4s.selection.video_module), rowcount++, 2);
    about_line(view, locstr("Audio backend"), module_name(app->ss4s.selection.audio_module), rowcount++, 2);
    char *os_str = os_info_str(&app->os_info);
    if (os_str != NULL) {
        about_line(view, locstr("System"), os_str, rowcount++, 1);
    }
    free(os_str);
#if TARGET_WEBOS || TARGET_ANDROID
    if (parent->panel_width > 0 && parent->panel_height > 0) {
        char resolution_text[16];
        SDL_snprintf(resolution_text, sizeof(resolution_text), "%5d * %5d", parent->panel_width, parent->panel_height);
        about_line(view, locstr("Screen resolution"), resolution_text, rowcount++, 1);
    }
    if (parent->panel_fps > 0) {
        char fps_text[16];
        SDL_snprintf(fps_text, sizeof(fps_text), "%4dFPS", parent->panel_fps);
        about_line(view, locstr("Refresh rate"), fps_text, rowcount++, 1);
    }
#endif
    /* LOGS A USER CAN SEND (util/diag_logs.h). */
    lv_obj_t *keep = pref_checkbox(view, locstr("Keep diagnostic logs"), &app_configuration->diag_logging, false);
    about_full_row(keep, rowcount++);
    lv_obj_t *keep_desc = pref_desc_label(view, locstr("Saves the app's log on this device (the last 5 starts), "
                                                       "from the next start. Turn it on, reproduce the problem, "
                                                       "then use Share logs."), false);
    about_full_row(keep_desc, rowcount++);
    lv_obj_t *share = lv_btn_create(view);
    lv_obj_t *share_label = lv_label_create(share);
    lv_label_set_text(share_label, locstr("Share logs..."));
    lv_obj_center(share_label);
    lv_obj_set_grid_cell(share, LV_GRID_ALIGN_START, 0, 2, LV_GRID_ALIGN_CENTER, rowcount++, 1);
    lv_obj_add_event_cb(share, share_logs_cb, LV_EVENT_CLICKED, app);
    lv_obj_t *check = pref_checkbox(view, locstr("Check for updates"), &app_configuration->check_updates, false);
    about_full_row(check, rowcount++);
    lv_obj_t *check_desc = pref_desc_label(view, locstr("Once a day, Eclipse downloads the list of releases from "
                                                        "the Project Hub. A newer version shows as a dot on "
                                                        "Settings. Nothing about you or your PCs is sent."),
                                           false);
    about_full_row(check_desc, rowcount++);
    LV_ASSERT(rowcount <= MAXIMUM_ROWS);
    for (int i = 0; i < rowcount; i++) {
        controller->row_dsc[i] = LV_GRID_CONTENT;
    }
    controller->row_dsc[rowcount] = LV_GRID_TEMPLATE_LAST;
    lv_obj_set_grid_dsc_array(view, col_dsc, controller->row_dsc);   /* re-read: the count just shrank */

    return view;
}

/* The logs folder as a URL SDL can open (Explorer / the desktop's file manager). */
static void open_logs_folder(void) {
#if SDL_VERSION_ATLEAST(2, 0, 14)
    char dir[1024], url[1100];
    diag_logs_dir(app_configuration->conf_dir, dir, sizeof(dir));
    for (char *c = dir; *c != '\0'; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }
    snprintf(url, sizeof(url), "file:///%s", dir[0] == '/' ? dir + 1 : dir);
    SDL_OpenURL(url);
#endif
}

static void share_logs_dialog_cb(lv_event_t *e) {
    lv_obj_t *dialog = lv_event_get_current_target(e);
    const char *pressed = lv_msgbox_get_active_btn_text(dialog);
    if (pressed != NULL && strcmp(pressed, locstr("Open folder")) == 0) {
        open_logs_folder();   /* the dialog stays: the user may still want the address */
        return;
    }
    diag_logs_share_stop();
    lv_msgbox_close_async(dialog);
}

static void share_logs_cb(lv_event_t *e) {
    app_t *app = lv_event_get_user_data(e);
    /* THE WHOLE REPORT (util/diag_report.h), not a few lines: the file a user sends must stand alone. */
    char *summary = diag_report_build(app);
    char body[2048] = "";
#if !TARGET_WEBOS && !TARGET_ANDROID
    /* DESKTOPS: everything in one file, in a folder the user can open and attach from. */
    char bundle[1024];
    if (diag_logs_write_bundle(app->settings.conf_dir, summary, bundle, sizeof(bundle))) {
        snprintf(body, sizeof(body), locstr("Everything is in one file:\n\n%s\n\nSend that file to whoever "
                                            "asked for your logs. PINs and passwords are left out of it."), bundle);
    } else {
        snprintf(body, sizeof(body), "%s", locstr("The log file could not be written."));
    }
#endif
#if !defined(_WIN32)
    /* THE SHARE PAGE, for the TV above all (its files are out of reach). Not on Windows: listening there
     * raised the firewall prompt, which took focus from the fullscreen app and left this dialog dead until
     * it was answered (measured on VEGA, 2026-09-29) -- and the user is at that PC with the file anyway. */
    char urls[256], part[768];
    if (diag_logs_share_start(app->settings.conf_dir, summary, urls, sizeof(urls))) {
        snprintf(part, sizeof(part), locstr("%sOn a computer or phone on the same network, open\n\n%s\n\n"
                                            "and download everything as one file. Sharing stops when you close "
                                            "this."), body[0] != '\0' ? "\n\n" : "", urls);
    } else {
        snprintf(part, sizeof(part), locstr("%sThe share page could not be opened on this device "
                                            "(no free network port)."), body[0] != '\0' ? "\n\n" : "");
    }
    strncat(body, part, sizeof(body) - strlen(body) - 1);
#endif
    if (!diag_logs_active()) {
        strncat(body, locstr("\n\nFor full logs, turn on \"Keep diagnostic logs\" and restart first."),
                sizeof(body) - strlen(body) - 1);
    }
#if TARGET_WEBOS || TARGET_ANDROID
    /* A TV's files are out of reach on Android too: the share page is the way out. */
    static const char *buttons[] = {translatable("Stop sharing"), ""};
#elif defined(_WIN32)
    static const char *buttons[] = {translatable("Open folder"), translatable("Close"), ""};
#else
    static const char *buttons[] = {translatable("Open folder"), translatable("Stop sharing"), ""};
#endif
    free(summary);
    lv_obj_t *dialog = lv_msgbox_create_i18n(NULL, locstr("Share logs"), body, buttons, false);
    lv_obj_add_event_cb(dialog, share_logs_dialog_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(dialog);
}

const char *module_name(const SS4S_ModuleInfo *info) {
    const char *id = SS4S_ModuleInfoGetId(info);
    if (id == NULL) {
        return "Not available";
    }
    return id;
}

/* "What's new": every release since this one, then how to update on this platform. */
#define WHATS_NEW_MAX_RELEASES 4
#define WHATS_NEW_MAX_HIGHLIGHTS 10

static void append(char *buf, size_t size, const char *text) {
    strncat(buf, text, size - strlen(buf) - 1);
}

static void whats_new_dialog_cb(lv_event_t *e) {
    lv_obj_t *dialog = lv_event_get_current_target(e);
    const char *pressed = lv_msgbox_get_active_btn_text(dialog);
#if !TARGET_WEBOS && !TARGET_ANDROID
    const update_info_t *update = update_check_info();
    if (pressed != NULL && strcmp(pressed, locstr("Download page")) == 0 && update->download_url != NULL) {
        SDL_OpenURL(update->download_url);
        return;   /* the dialog stays */
    }
#else
    LV_UNUSED(pressed);
#endif
    lv_msgbox_close_async(dialog);
}

static void whats_new_cb(lv_event_t *e) {
    settings_controller_t *parent = lv_event_get_user_data(e);
    const update_info_t *update = update_check_info();
    if (!update->available) {
        return;
    }
    char body[4096] = "";
    char line[512];
    for (size_t i = 0; i < update->releases_count && i < WHATS_NEW_MAX_RELEASES; i++) {
        const update_release_t *r = &update->releases[i];
        if (r->date != NULL) {
            snprintf(line, sizeof(line), "%sEclipse %s (%s)\n", i > 0 ? "\n" : "", r->version, r->date);
        } else {
            snprintf(line, sizeof(line), "%sEclipse %s\n", i > 0 ? "\n" : "", r->version);
        }
        append(body, sizeof(body), line);
        for (size_t j = 0; j < r->highlights_count && j < WHATS_NEW_MAX_HIGHLIGHTS; j++) {
            if (r->highlights[j] == NULL) {
                continue;
            }
            snprintf(line, sizeof(line), "- %s\n", r->highlights[j]);
            append(body, sizeof(body), line);
        }
    }
    if (update->how_to_update != NULL) {
        snprintf(line, sizeof(line), "\n%s\n%s", locstr("How to update"), update->how_to_update);
        append(body, sizeof(body), line);
    }
#if TARGET_WEBOS || TARGET_ANDROID
    /* No browser to hand the link to: show where the download is. */
    if (update->download_url != NULL) {
        snprintf(line, sizeof(line), "\n\n%s", update->download_url);
        append(body, sizeof(body), line);
    }
    static const char *buttons[] = {translatable("Close"), ""};
#else
    static const char *buttons[] = {translatable("Download page"), translatable("Close"), ""};
#endif
    lv_obj_t *dialog = lv_msgbox_create_i18n(NULL, locstr("What's new"), body, buttons, false);
    /* Wide enough for the release page's address on one line (at the default width it broke
     * mid-word on the TV, 2026-11-14). */
    lv_obj_set_width(dialog, LV_PCT(60));
    /* A long list scrolls inside the dialog instead of running off the screen. */
    lv_obj_t *content = lv_msgbox_get_content(dialog);
    lv_obj_set_style_max_height(content, LV_PCT(60), 0);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(dialog, whats_new_dialog_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(dialog);
    update_check_mark_seen();   /* the dot goes away for this version; the About line stays */
    update_dot_refresh(parent->about_dot);
}

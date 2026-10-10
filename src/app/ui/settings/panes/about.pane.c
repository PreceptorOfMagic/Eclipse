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
    about_line(view, locstr("Video decoder"), module_name(app->ss4s.selection.video_module), rowcount++, 2);
    about_line(view, locstr("Audio backend"), module_name(app->ss4s.selection.audio_module), rowcount++, 2);
    char *os_str = os_info_str(&app->os_info);
    if (os_str != NULL) {
        about_line(view, locstr("System"), os_str, rowcount++, 1);
    }
    free(os_str);
#if TARGET_WEBOS
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
#if !TARGET_WEBOS
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
#if TARGET_WEBOS
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

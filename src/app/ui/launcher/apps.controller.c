//
// Created by Mariotaku on 2021/08/31.
//

#include "app.h"
#include "appitem.view.h"
#include "apps.controller.h"
#include "launcher.controller.h"
#include "coop.dialog.h"
#include "app_launch.h"
#include "backend/aurora_host_caps.h"
#include "stream/video/coop_session.h"
#include "ui/streaming/streaming.controller.h"
#include "ui/streaming/pin_autoentry.h"
#include "stream/session.h"
#include "logging.h"

#include "coverloader.h"
#include "backend/apploader/apploader.h"
#include <errors.h>
#include <assert.h>
#include <string.h>

#include "lvgl/lv_ext_utils.h"
#include "lvgl/util/lv_app_utils.h"
#include "lvgl/theme/lv_theme_moonlight.h"
#include "lv_gridview.h"

#include "util/user_event.h"
#include "util/i18n.h"
#include "pair.dialog.h"
#include "ui/common/progress_dialog.h"

typedef void (*action_cb_t)(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index);

static lv_obj_t *apps_view(lv_fragment_t *self, lv_obj_t *container);

static void on_view_created(lv_fragment_t *self, lv_obj_t *view);

static void obj_will_delete(lv_fragment_t *self, lv_obj_t *obj);

static void on_destroy_view(lv_fragment_t *self, lv_obj_t *view);

static bool on_event(lv_fragment_t *self, int code, void *userdata);

static void host_info_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata);

static void send_wol_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata);

static void on_host_updated(const uuidstr_t *uuid, void *userdata);

static void on_host_removed(const uuidstr_t *uuid, void *userdata);

static void item_click_cb(lv_event_t *event);

static void item_longpress_cb(lv_event_t *event);

static void launcher_launch_game(apps_fragment_t *controller, const apploader_item_t *app);

/** The context menu's "host monitor off" start (app_settings.h host_monitor_mode); a marker, never called. */
static void launcher_launch_game_monitor_off(apps_fragment_t *controller, const apploader_item_t *app);

/** Set by that button for the ONE launch it starts; launcher_launch_game consumes it. */
static bool g_launch_monitor_off = false;

/** CHOOSE mode and a host that can switch its monitor off: starting a stream offers both start buttons. */
static bool host_monitor_choice_here(const apps_fragment_t *controller) {
    uuidstr_t host = controller->uuid;
    return app_launch_host_monitor_choice_offered() && umbra_monitor_off_on_host(&host);
}

static void launcher_toggle_fav(apps_fragment_t *controller, const apploader_item_t *app);

static void launcher_toggle_hidden(apps_fragment_t *controller, const apploader_item_t *app);

static void launcher_quit_game(apps_fragment_t *controller);

static void applist_focus_enter(lv_event_t *event);

static void applist_focus_leave(lv_event_t *event);

static void update_view_state(apps_fragment_t *controller);

static void appitem_bind(apps_fragment_t *controller, lv_obj_t *item, apploader_item_t *app);

static int adapter_item_count(lv_obj_t *, void *data);

static lv_obj_t *adapter_create_view(lv_obj_t *parent);

static void adapter_bind_view(lv_obj_t *, lv_obj_t *, void *data, int position);

static void quitgame_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata);

static void apps_controller_ctor(lv_fragment_t *self, void *args);

static void apps_controller_dtor(lv_fragment_t *self);

static void appload_started(void *userdata);

static void appload_loaded(apploader_list_t *apps, void *userdata);

static void appload_errored(int code, const char *error, void *userdata);

static void quit_dialog_cb(lv_event_t *event);

static void switch_app_dialog(apps_fragment_t *controller, int running_app, const apploader_item_t *app);

static void switch_app_dialog_cb(lv_event_t *event);

static void actions_click_cb(lv_event_t *event);

static void action_cb_wol(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index);

static void action_cb_host_reload(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index);

static void action_cb_pair(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index);

static void update_grid_config(apps_fragment_t *controller);

static void open_context_menu(apps_fragment_t *fragment, appitem_viewholder_t *holder);

static void context_menu_cancel_cb(lv_event_t *event);

static void context_menu_click_cb(lv_event_t *event);

static void app_detail_dialog(apps_fragment_t *fragment, const apploader_item_t *app);

static void app_detail_click_cb(lv_event_t *event);

static void set_actions(apps_fragment_t *controller, const char **labels, const action_cb_t *callbacks);

/**
 *
 * @param old_list
 * @param new_list
 * @param num_changes number of changes. It will be assigned to -1 if the whole dataset has been changed.
 * @return Allocated array of changes. It should be freed by caller.
 */
static lv_gridview_data_change_t *apps_list_detect_change(const apploader_list_t *old_list,
                                                          const apploader_list_t *new_list, int *num_changes);

static void show_progress(apps_fragment_t *fragment);

static void show_ok(apps_fragment_t *fragment);

static void show_error(apps_fragment_t *fragment, const char *title, const char *hint, const char *detail);

static apps_fragment_t *current_instance = NULL;

/* App id to relaunch fresh after a PIN HDR reconnect. File-static (not on the
 * fragment) so it survives the apps-controller obj teardown that starting a
 * stream causes. Consumed by appload_loaded when the apps view is recreated. */
static int g_pending_hdr_relaunch_app = 0;
/* Was the quit that armed the relaunch a CO-OP session? Captured at quit time rather than
 * read back later, because the relaunch must not depend on when the flag happens to change
 * -- and relaunching a co-op run as a single app id is exactly the bug this exists to fix
 * (it tore both halves down and brought neither back, nine seconds into every woken co-op
 * session).
 *
 * This comment used to claim app_launch_coop_requested() "is cleared during teardown". It
 * was not, anywhere -- until 2026-08-12 NOTHING in the tree ever passed false, so the flag
 * survived for the life of the process. Believing that stale sentence costs about ten
 * minutes; see the black-screen note in launcher_launch_game() for what the leak did. */
static bool g_pending_hdr_relaunch_coop = false;
/* How many fresh-launch attempts we've made for the pending reconnect. The first
 * launch right after the quit — and each retry — can hit Apollo's 503 "is a display
 * connected" while its display/encoder reinit after the HDR mode change, so we retry
 * a few times with a settle delay before giving up and surfacing the error. */
static int g_hdr_relaunch_attempts = 0;
#define HDR_RELAUNCH_MAX_ATTEMPTS 4
static lv_timer_t *g_hdr_relaunch_timer = NULL;

static void hdr_relaunch_timer_cb(lv_timer_t *timer) {
    g_hdr_relaunch_timer = NULL;
    lv_timer_del(timer);
    apps_fragment_t *controller = current_instance;
    /* Only launch if the apps view is back and no stream is currently active. */
    if (controller == NULL || controller->global->session != NULL || g_pending_hdr_relaunch_app == 0) {
        return;
    }
    if (g_pending_hdr_relaunch_coop) {
        /* A co-op session comes back through the co-op path, not the app list: it is
         * two hosts and an orientation, none of which an apploader item carries. */
        g_hdr_relaunch_attempts++;
        commons_log_info("PIN", "HDR reconnect: relaunching CO-OP session (attempt %d/%d)",
                         g_hdr_relaunch_attempts, HDR_RELAUNCH_MAX_ATTEMPTS);
        if (coop_dialog_relaunch_last(controller->global)) {
            g_pending_hdr_relaunch_app = 0;
            g_pending_hdr_relaunch_coop = false;
        } else {
            /* The remembered hosts are gone. Say so and stop, rather than falling
             * through to launch half a co-op session as an ordinary stream. */
            commons_log_warn("PIN", "HDR reconnect: co-op hosts no longer available, giving up");
            g_pending_hdr_relaunch_app = 0;
            g_pending_hdr_relaunch_coop = false;
        }
        return;
    }
    if (controller->apploader_apps == NULL) {
        return;
    }
    const apploader_item_t *app = apploader_list_item_by_id(controller->apploader_apps,
                                                            g_pending_hdr_relaunch_app);
    if (app == NULL) {
        g_pending_hdr_relaunch_app = 0; /* app vanished from the list; give up */
        return;
    }
    g_hdr_relaunch_attempts++;
    commons_log_info("PIN", "HDR reconnect: launching app %d fresh (attempt %d/%d)",
                     g_pending_hdr_relaunch_app, g_hdr_relaunch_attempts, HDR_RELAUNCH_MAX_ATTEMPTS);
    launcher_launch_game(controller, app);
}

const static lv_gridview_adapter_t apps_adapter = {
        .item_count = adapter_item_count,
        .create_view = adapter_create_view,
        .bind_view = adapter_bind_view,
};
const static pcmanager_listener_t pc_listeners = {
        .updated = on_host_updated,
        .removed = on_host_removed,
};

const lv_fragment_class_t apps_controller_class = {
        .constructor_cb = apps_controller_ctor,
        .destructor_cb = apps_controller_dtor,
        .create_obj_cb = apps_view,
        .obj_will_delete_cb = obj_will_delete,
        .obj_created_cb = on_view_created,
        .obj_deleted_cb = on_destroy_view,
        .event_cb = on_event,
        .instance_size = sizeof(apps_fragment_t),
};

static const char *action_labels_offline[] = {translatable("Wake"), translatable("Retry"), ""};
static const action_cb_t action_callbacks_offline[] = {action_cb_wol, action_cb_host_reload};
static const char *action_labels_error[] = {translatable("Retry"), ""};
static const action_cb_t action_callbacks_error[] = {action_cb_host_reload};
static const char *actions_unpaired[] = {translatable("Pair"), ""};
static const action_cb_t action_callbacks_unpaired[] = {action_cb_pair};
static const char *actions_apps_none[] = {""};

static void apps_controller_ctor(lv_fragment_t *self, void *args) {
    apps_fragment_t *controller = (apps_fragment_t *) self;
    controller->apploader_cb.start = appload_started;
    controller->apploader_cb.data = appload_loaded;
    controller->apploader_cb.error = appload_errored;
    apps_fragment_arg_t *arg = args;
    controller->global = arg->global;
    controller->uuid = arg->host;
    controller->def_app = arg->def_app;
    controller->apploader = apploader_create(arg->global, &controller->uuid, &controller->apploader_cb, controller);

    appitem_style_init(&controller->appitem_style);
}

static void apps_controller_dtor(lv_fragment_t *self) {
    apps_fragment_t *fragment = (apps_fragment_t *) self;
    appitem_style_deinit(&fragment->appitem_style);
    apploader_destroy(fragment->apploader);
    if (fragment->apploader_apps != NULL) {
        apploader_list_free(fragment->apploader_apps);
    }
}

static lv_obj_t *apps_view(lv_fragment_t *self, lv_obj_t *container) {
    apps_fragment_t *controller = (apps_fragment_t *) self;
    lv_obj_t *view = lv_obj_create(container);
    lv_obj_remove_style_all(view);
    lv_obj_add_flag(view, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(view, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scroll_dir(view, LV_DIR_NONE);

    lv_obj_t *applist = controller->applist = lv_gridview_create(view);
    lv_obj_add_flag(applist, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_scroll_dir(applist, LV_DIR_VER);
    /* AUTO, not ACTIVE: ACTIVE shows the bar only DURING a drag, so with a mouse wheel or
     * the d-pad it never appeared at all, and nothing said the list went further. The
     * gridview is its own class, which the default theme never gives a bar style. */
    lv_obj_set_scrollbar_mode(applist, LV_SCROLLBAR_MODE_AUTO);
    lv_theme_moonlight_add_scrollbar(applist);
    lv_obj_set_style_pad_hor(applist, lv_dpx(32), 0);
    lv_obj_set_style_pad_ver(applist, lv_dpx(28), 0);
    lv_obj_set_style_pad_gap(applist, lv_dpx(20), 0);
    lv_obj_set_style_radius(applist, 0, 0);
    lv_obj_set_style_border_side(applist, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_bg_opa(applist, 0, 0);
    lv_obj_set_size(applist, LV_PCT(100), LV_PCT(100));
    lv_obj_update_layout(applist);

    lv_gridview_set_adapter(applist, &apps_adapter);
    lv_obj_t *appload = controller->appload = lv_spinner_create(view, 1000, 60);
    launcher_fragment_t *parent_controller = (launcher_fragment_t *) lv_fragment_get_parent(&controller->base);
    lv_group_add_obj(parent_controller->detail_group, appload);
    lv_obj_add_flag(appload, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(appload, lv_dpx(60), lv_dpx(60));
    lv_obj_center(appload);

    lv_obj_t *apperror = controller->apperror = lv_obj_create(view);
    lv_obj_add_flag(apperror, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(apperror, LV_PCT(80), LV_PCT(60));
    lv_obj_center(apperror);
    lv_obj_set_flex_flow(apperror, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(apperror, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *errortitle = controller->errortitle = lv_label_create(apperror);
    lv_obj_set_width(errortitle, LV_PCT(100));
    lv_obj_set_style_text_font(errortitle, lv_theme_get_font_large(apperror), 0);
    lv_obj_t *errorlabel = controller->errorhint = lv_label_create(apperror);
    lv_obj_set_width(errorlabel, LV_PCT(100));
    lv_obj_t *errordetail = controller->errordetail = lv_label_create(apperror);
    lv_obj_set_style_border_width(errordetail, LV_DPX(2), 0);
    lv_obj_set_style_border_opa(errordetail, LV_OPA_50, 0);
    lv_obj_set_style_border_color(errordetail, lv_palette_main(LV_PALETTE_BLUE_GREY), 0);
    lv_obj_set_style_pad_all(errordetail, LV_DPX(10), 0);
    lv_obj_set_style_radius(errordetail, LV_DPX(20), 0);
    lv_obj_set_width(errordetail, LV_PCT(100));
    lv_obj_set_flex_grow(errordetail, 1);

    lv_obj_t *actions = controller->actions = lv_btnmatrix_create(apperror);
    lv_obj_set_style_outline_width(actions, 0, 0);
    lv_obj_set_style_outline_width(actions, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_pad_all(actions, LV_DPX(5), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_side(actions, LV_BORDER_SIDE_NONE, 0);
    const lv_font_t *font = lv_obj_get_style_text_font(actions, LV_PART_ITEMS);
    lv_obj_set_height(actions, lv_font_get_line_height(font) + LV_DPI_DEF / 10 + LV_DPX(5) * 2);
    lv_obj_set_style_max_width(actions, LV_PCT(100), 0);
    lv_obj_add_flag(actions, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_btnmatrix_set_btn_ctrl_all(actions, LV_BTNMATRIX_CTRL_CLICK_TRIG | LV_BTNMATRIX_CTRL_NO_REPEAT);

    set_actions(controller, actions_apps_none, NULL);

    return view;
}

static void on_view_created(lv_fragment_t *self, lv_obj_t *view) {
    LV_UNUSED(view);
    apps_fragment_t *controller = (apps_fragment_t *) self;
    controller->coverloader = coverloader_new(controller->global);
    pcmanager_register_listener(pcmanager, &pc_listeners, controller);
    lv_obj_t *applist = controller->applist;
    lv_obj_add_event_cb(applist, item_click_cb, LV_EVENT_SHORT_CLICKED, controller);
    lv_obj_add_event_cb(applist, item_longpress_cb, LV_EVENT_LONG_PRESSED, controller);
    lv_obj_add_event_cb(applist, applist_focus_enter, LV_EVENT_FOCUSED, controller);
    lv_obj_add_event_cb(applist, applist_focus_leave, LV_EVENT_DEFOCUSED, controller);
    lv_obj_add_event_cb(applist, applist_focus_leave, LV_EVENT_LEAVE, controller);
    lv_obj_add_event_cb(controller->actions, actions_click_cb, LV_EVENT_VALUE_CHANGED, controller);

    update_grid_config(controller);
    lv_obj_set_user_data(controller->applist, controller);

    const SERVER_STATE *state = pcmanager_state(pcmanager, &controller->uuid);
    if (state != NULL && state->code != SERVER_STATE_QUERYING) {
        pcmanager_request_update(pcmanager, &controller->uuid, host_info_cb, NULL);
        if (state->code == SERVER_STATE_AVAILABLE) {
            apploader_load(controller->apploader);
        }
    }
    current_instance = controller;
    update_view_state(controller);
}

static void obj_will_delete(lv_fragment_t *self, lv_obj_t *obj) {
    LV_UNUSED(obj);
    apps_fragment_t *fragment = (apps_fragment_t *) self;
    apploader_cancel(fragment->apploader);
    fragment->def_app = 0;
}

static void update_grid_config(apps_fragment_t *controller) {
    lv_obj_t *applist = controller->applist;
    lv_obj_update_layout(applist);
    lv_coord_t applist_width = lv_obj_get_width(applist);
    int col_count = LV_CLAMP(4, applist_width / lv_dpx(150), 8);
    lv_coord_t col_width = (applist_width - lv_obj_get_style_pad_left(applist, 0) -
                            lv_obj_get_style_pad_right(applist, 0) -
                            lv_obj_get_style_pad_column(applist, 0) * (col_count - 1)) / col_count;
    controller->col_count = col_count;
    controller->col_width = col_width;
    /* Vertical box art (Moonlight / GeForce NOW covers are ~130×180, ratio 3:4). */
    lv_coord_t row_height = (col_width * 4) / 3;
    controller->col_height = row_height;
    lv_gridview_set_config(applist, col_count, row_height, LV_GRID_ALIGN_CENTER, LV_GRID_ALIGN_CENTER);

    appitem_styles_t *styles = &controller->appitem_style;
    if (!coverloader_default_cover_build(&styles->defcover_src, &styles->defcover_data, col_width, row_height,
                                         styles->radius)) {
        /* The plain placeholder, square corners and all, beats an empty tile -- but say so: it is
         * exactly what issue #23 looked like. */
        commons_log_warn("Apps", "Rounded placeholder cover not built; tiles without art use the plain one");
        styles->defcover_src.header.w = col_width;
        styles->defcover_src.header.h = row_height;
    }
    commons_log_debug("Apps", "Grid re-measured: list %dpx -> %d columns of %dx%d",
                      applist_width, col_count, col_width, row_height);
}

static void on_destroy_view(lv_fragment_t *self, lv_obj_t *view) {
    LV_UNUSED(view);
    current_instance = NULL;
    apps_fragment_t *controller = (apps_fragment_t *) self;
    controller->show_hidden_apps = false;
    pcmanager_unregister_listener(pcmanager, &pc_listeners);
    coverloader_unref(controller->coverloader);
}

static bool on_event(lv_fragment_t *self, int code, void *userdata) {
    LV_UNUSED(userdata);
    apps_fragment_t *controller = (apps_fragment_t *) self;
    switch (code) {
        case USER_SIZE_CHANGED: {
            update_grid_config(controller);
            lv_gridview_rebind(controller->applist);
            break;
        }
        case USER_SHOW_HIDDEN_APPS: {
            ui_userevent_t *event = userdata;
            if (uuidstr_t_equals_t(&controller->uuid, event->data1)) {
                controller->show_hidden_apps = true;
                apploader_load(controller->apploader);
            }
            free(event->data1);
            return true;
        }
        default:
            break;
    }
    return false;
}

static void on_host_updated(const uuidstr_t *uuid, void *userdata) {
    apps_fragment_t *controller = (apps_fragment_t *) userdata;
    if (controller != current_instance) { return; }
    if (!uuidstr_t_equals_t(&controller->uuid, uuid)) { return; }
    const SERVER_STATE *state = pcmanager_state(pcmanager, uuid);
    if (state == NULL) { return; }
    if (state->code == SERVER_STATE_AVAILABLE) {
        apploader_load(controller->apploader);
    }
    update_view_state(controller);
}

static void on_host_removed(const uuidstr_t *uuid, void *userdata) {
    apps_fragment_t *controller = (apps_fragment_t *) userdata;
    if (!uuidstr_t_equals_t(&controller->uuid, uuid)) { return; }
    lv_fragment_del((lv_fragment_t *) controller);
}

static void host_info_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata) {
    LV_UNUSED(userdata);
    apps_fragment_t *controller = current_instance;
    if (controller == NULL) { return; }
    if (!uuidstr_t_equals_t(&controller->uuid, uuid)) { return; }
    if (!controller->base.managed->obj_created) { return; }
    lv_btnmatrix_clear_btn_ctrl_all(controller->actions, LV_BTNMATRIX_CTRL_DISABLED);
}

static void send_wol_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata) {
    LV_UNUSED(userdata);
    apps_fragment_t *controller = current_instance;
    if (controller == NULL) { return; }
    if (!uuidstr_t_equals_t(&controller->uuid, uuid)) { return; }
    if (!controller->base.managed->obj_created) { return; }
    lv_btnmatrix_clear_btn_ctrl_all(controller->actions, LV_BTNMATRIX_CTRL_DISABLED);
    const SERVER_STATE *state = pcmanager_state(pcmanager, &controller->uuid);
    if (state == NULL) { return; }
    if (state->code & SERVER_STATE_ONLINE || result != GS_OK) { return; }
    pcmanager_request_update(pcmanager, &controller->uuid, host_info_cb, NULL);
}

static void update_view_state(apps_fragment_t *controller) {
    if (controller != current_instance) { return; }
    if (!controller->base.managed->obj_created || controller->base.managed->destroying_obj) { return; }
    launcher_fragment_t *parent_controller = (launcher_fragment_t *) lv_fragment_get_parent(&controller->base);
    parent_controller->detail_changing = true;
    const SERVER_STATE *state = pcmanager_state(pcmanager, &controller->uuid);
    if (state == NULL) {
        parent_controller->detail_changing = false;
        return;
    }
    switch (state->code) {
        case SERVER_STATE_NONE:
        case SERVER_STATE_QUERYING: {
            // waiting to load server info
            show_progress(controller);
            break;
        }
        case SERVER_STATE_AVAILABLE: {
            switch (apploader_state(controller->apploader)) {
                case APPLOADER_STATE_LOADING: {
                    // is loading apps
                    if (controller->apploader_apps) {
                        break;
                    }
                    show_progress(controller);
                    break;
                }
                case APPLOADER_STATE_ERROR: {
                    // apploader has error
                    show_error(controller, locstr("Failed to load apps"),
                               locstr("Press \"Retry\" to load again.\n\nRestart your computer if error persists."),
                               controller->apploader_error);

                    set_actions(controller, action_labels_error, action_callbacks_error);
                    lv_group_focus_obj(controller->actions);
                    lv_obj_add_state(controller->actions, LV_STATE_FOCUS_KEY);
                    break;
                }
                case APPLOADER_STATE_IDLE: {
                    // has apps
                    show_ok(controller);
                    break;
                }
            }
            break;
        }
        case SERVER_STATE_NOT_PAIRED: {
            show_error(controller, locstr("Not paired"),
                       locstr("Press \"Pair\" to pair this host with your account."),
                       NULL);

            set_actions(controller, actions_unpaired, action_callbacks_unpaired);
            lv_group_focus_obj(controller->actions);
            lv_obj_add_state(controller->actions, LV_STATE_FOCUS_KEY);
            break;
        }
        case SERVER_STATE_ERROR: {
            // server has error
            show_error(controller, locstr("Host error"),
                       locstr("Press \"Retry\" to load again.\n\nRestart your computer if error persists."),
                       state->error.errmsg);

            set_actions(controller, action_labels_error, action_callbacks_error);
            lv_group_focus_obj(controller->actions);
            lv_obj_add_state(controller->actions, LV_STATE_FOCUS_KEY);
            break;
        }
        case SERVER_STATE_OFFLINE: {
            // server has error
            show_error(controller, locstr("Offline"),
                       locstr("Press \"Wake\" to send Wake-on-LAN packet to turn the computer on if it supports this feature, "
                              "press \"Retry\" to connect again.\n\n"
                              "Try restart your computer if these doesn't work."),
                       NULL);

            set_actions(controller, action_labels_offline, action_callbacks_offline);
            lv_btnmatrix_clear_btn_ctrl(controller->actions, 0, LV_BTNMATRIX_CTRL_DISABLED);
            lv_group_focus_obj(controller->actions);
            lv_obj_add_state(controller->actions, LV_STATE_FOCUS_KEY);
            break;
        }
        default: {
            break;
        }
    }
    parent_controller->detail_changing = false;
}

static void show_progress(apps_fragment_t *fragment) {
    lv_obj_add_flag(fragment->applist, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->apperror, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(fragment->appload, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->actions, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_state(fragment->actions, LV_STATE_DISABLED);

    lv_group_focus_obj(fragment->appload);
    lv_obj_add_state(fragment->appload, LV_STATE_FOCUS_KEY);
}

static void show_ok(apps_fragment_t *fragment) {
    lv_obj_clear_flag(fragment->applist, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->apperror, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->appload, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->actions, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_state(fragment->actions, LV_STATE_DISABLED);

    if (lv_group_get_focused(lv_obj_get_group(fragment->applist)) != fragment->applist) {
        lv_group_focus_obj(fragment->applist);
    }
    /* KEEP THE USER'S PLACE. show_ok runs on every host status update (on_host_updated ->
     * update_view_state, every few seconds while the host is polled), and it used to focus
     * focus_backup unconditionally -- which only changes when the grid LOSES focus, so it was
     * nearly always 0. The focus jumped back to the first app every few seconds, and OK then
     * launched THAT app: measured 2026-09-26, the ring drawn on "Desktop" and OK starting
     * "Co-op Pane", the first tile. Only a grid with nothing focused gets a focus here. */
    if (lv_gridview_get_focused_index(fragment->applist) >= 0) {
        return;
    }
    int idx = fragment->focus_backup >= 0 ? fragment->focus_backup : 0;
    lv_gridview_focus(fragment->applist, idx);
}

static void show_error(apps_fragment_t *fragment, const char *title, const char *hint, const char *detail) {
    lv_obj_add_flag(fragment->appload, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(fragment->apperror, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->applist, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(fragment->actions, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_state(fragment->actions, LV_STATE_DISABLED);

    lv_obj_set_style_opa(fragment->errordetail, detail ? LV_OPA_100 : LV_OPA_0, 0);
    lv_label_set_text_static(fragment->errortitle, title);
    lv_label_set_text_static(fragment->errorhint, hint);
}

static void appload_started(void *userdata) {
    apps_fragment_t *fragment = userdata;
    update_view_state(fragment);
}

/**
 * THE CO-OP PANE IS NOT A GAME ON THIS PC, so it is not a tile -- except while it RUNS here.
 *
 * Co-op spans two PCs and starts from the co-op screen; the "Co-op Pane" each host runs is only
 * its half. Listed as a tile it read as a game and launched a lone half-picture (09-26: OK on the
 * first tile started it). Eclipse asks hosts to list it (umbraCoop=1, for the co-op screen's
 * resolver), so the grid drops it here unless it is THIS host's running app -- a co-op session
 * to resume or quit -- or the launch payload names it (the scripted co-op harness). Names freed
 * here, so apploader_list_free() still frees exactly what is left.
 */
static void apps_hide_idle_coop_pane(const apps_fragment_t *fragment, apploader_list_t *apps) {
    const int running = pcmanager_server_current_app(pcmanager, &fragment->uuid);
    size_t out = 0;
    for (size_t i = 0; i < apps->count; ++i) {
        apploader_item_t *item = &apps->items[i];
        const bool pane = item->base.name != NULL && strcmp(item->base.name, COOP_SESSION_APP_NAME) == 0;
        const bool shown = !pane || (running != 0 && item->base.id == running) ||
                          (fragment->def_app == item->base.id && !fragment->def_app_launched);
        if (!shown) {
            free(item->base.name);
            continue;
        }
        if (out != i) {
            apps->items[out] = *item;
        }
        out++;
    }
    apps->count = out;
}

static void appload_loaded(apploader_list_t *apps, void *userdata) {
    apps_fragment_t *fragment = userdata;
    if (!fragment->base.managed->obj_created || fragment->base.managed->destroying_obj) {
        return;
    }
    apps_hide_idle_coop_pane(fragment, apps);
    int num_changes = -1;
    lv_gridview_data_change_t *changes = apps_list_detect_change(fragment->apploader_apps, apps, &num_changes);
    if (num_changes != 0) {
        fragment->focus_backup = 0;
        lv_gridview_focus(fragment->applist, 0);
    }
    const int focused = lv_gridview_get_focused_index(fragment->applist);
    const lv_gridview_data_change_t *apply = changes;
#if TARGET_ANDROID
    /* THE SAME APPS ARE REBOUND IN PLACE (Android). changes == NULL makes the grid scrap every tile and take them back
     * from its free pool, which is a stack, so they come back in reverse order: the focused tile's object lands on
     * another tile and plays its unfocus transition there while the right one focuses -- the highlight flicked to
     * another tile and back on each reload of an unchanged list (every 20 s, and after a stream ends), with no input
     * event at those moments (SHIELD TV, 2026-10-22). An empty change list keeps every tile's object where it is and
     * only rebinds it to the new data; its focus stays with it. */
    static const lv_gridview_data_change_t same_apps[1];
    if (num_changes == 0 && apply == NULL) {
        apply = same_apps;
    }
#endif
    lv_gridview_set_data_advanced(fragment->applist, apps, apply, num_changes);
    if (num_changes == 0 && focused >= 0 && apply == NULL) {
        /* The same apps: the grid still rebuilt every tile (changes == NULL scraps them all) and the focused one came
         * back without its ring, so the user could not see what OK would start. Draw it on the same tile again. */
        lv_gridview_focus(fragment->applist, focused);
    }
    if (changes != NULL) {
        free(changes);
    }
    apploader_list_free(fragment->apploader_apps);
    fragment->apploader_apps = apps;
    update_view_state(fragment);

    if (fragment->def_app > 0 && !fragment->def_app_launched) {
        fragment->def_app_launched = true;
        const apploader_item_t *app = NULL;
        for (int i = 0; i < apps->count; ++i) {
            if (apps->items[i].base.id == fragment->def_app) {
                app = &apps->items[i];
                break;
            }
        }
        if (app != NULL) {
            /* Mark it BEFORE the call, not around it: launcher_launch_game pushes the
             * streaming fragment, which starts reading the co-op globals synchronously,
             * so anything restored after the call is restored too late. */
            fragment->def_app_from_payload = true;
            launcher_launch_game(fragment, app);
        }
    }

    /* PIN HDR reconnect: after the wake stream was quit (to force a fresh, HDR-
     * capable launch), relaunch the same app once the apps view is back and no
     * session is active. The pending id lives in a file-static, not the fragment,
     * because pushing the stream deleted this controller's obj and wiped def_app.
     * The actual launch is deferred by a settle delay (and retried) via a timer so
     * Apollo's display/encoder has time to reinit after the HDR mode change. */
    if (g_pending_hdr_relaunch_app > 0 && fragment->global->session == NULL) {
        if (g_hdr_relaunch_attempts >= HDR_RELAUNCH_MAX_ATTEMPTS) {
            /* Out of retries — stop; the last failure's error dialog was shown. */
            commons_log_info("PIN", "HDR reconnect: giving up on app %d after %d attempts",
                             g_pending_hdr_relaunch_app, g_hdr_relaunch_attempts);
            g_pending_hdr_relaunch_app = 0;
            g_pending_hdr_relaunch_coop = false;
            g_hdr_relaunch_attempts = 0;
        } else if (g_hdr_relaunch_timer == NULL) {
            int settle = app_configuration->pin_relaunch_settle_ms > 0
                             ? app_configuration->pin_relaunch_settle_ms : 3000;
            g_hdr_relaunch_timer = lv_timer_create(hdr_relaunch_timer_cb, settle, NULL);
        }
    }
}

static void appload_errored(int code, const char *error, void *userdata) {
    LV_UNUSED(code);
    apps_fragment_t *fragment = userdata;
    fragment->apploader_error = error;
    update_view_state(fragment);
}

static void appitem_bind(apps_fragment_t *controller, lv_obj_t *item, apploader_item_t *app) {
    appitem_viewholder_t *holder = lv_obj_get_user_data(item);

    /* The grid centres items at their own size instead of stretching them (both axes are
     * LV_GRID_ALIGN_CENTER), and appitem_view_create() sizes an item exactly once. Recycled
     * items therefore kept whatever the column measured when they were first created: after a
     * re-layout they sat smaller than their cell, so clicks between them landed on the grid and
     * did nothing, and once the cover was reloaded at the new size an object larger than its
     * source got tiled by lv_img (lv_img.c:665) into a mosaic. Re-apply the geometry per bind. */
    lv_obj_set_size(item, controller->col_width, controller->col_height);
    lv_obj_set_style_transform_pivot_x(item, controller->col_width / 2, 0);
    lv_obj_set_style_transform_pivot_y(item, controller->col_height / 2, 0);

    coverloader_display(controller->coverloader, &controller->uuid, app->base.id, item,
                        controller->col_width, controller->col_height);
    lv_label_set_text(holder->title, app->base.name);

    int current_id = pcmanager_server_current_app(pcmanager, &controller->uuid);
    if (current_id == app->base.id) {
        lv_obj_clear_flag(holder->play_indicator, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(holder->play_indicator, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_opa(item, app->hidden ? LV_OPA_50 : LV_OPA_COVER, 0);
    holder->app_id = app->base.id;
}

static void item_click_cb(lv_event_t *event) {
    apps_fragment_t *controller = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target(event);
    lv_obj_t *target_parent = lv_obj_get_parent(target);
    if (target_parent != controller->applist) {
        return;
    }
    appitem_viewholder_t *holder = (appitem_viewholder_t *) lv_obj_get_user_data(target);
    int current_app = pcmanager_server_current_app(pcmanager, &controller->uuid);
    const apploader_item_t *item = apploader_list_item_by_id(controller->apploader_apps, holder->app_id);
    assert(item != NULL);
    if (current_app != 0) {
        if (holder->app_id == current_app) {
            open_context_menu(controller, holder);
        } else {
            /* ANOTHER APP IS RUNNING ON THIS PC. This used to return here and do nothing at
             * all -- no message, no log -- so a click or OK on any other app simply did not
             * work until the user thought to open the running app's menu and stop it
             * (measured 2026-09-26: a session left paused on a host blocked every launch). */
            switch_app_dialog(controller, current_app, item);
        }
        return;
    }
    /* CHOOSE MODE: the click asks how to start -- "Start streaming" or "...(host monitor off)". */
    if (host_monitor_choice_here(holder->controller)) {
        open_context_menu(holder->controller, holder);
        return;
    }
    launcher_launch_game(holder->controller, item);
}

bool apps_controller_reconnect_running_app(app_t *global) {
    if (global == NULL || global->session == NULL) {
        return false;
    }
    /* Read the app id from the session, not pcmanager/current_instance: during a
     * stream the apps-controller obj is destroyed (so current_instance is NULL)
     * and pcmanager's cached currentGame is 0 on a cold boot until stream end.
     * The session knows the id it launched with. */
    int app_id = session_get_app_id(global->session);
    if (app_id == 0) {
        return false;
    }
    /* Remember what to relaunch; the recreated apps view picks this up in
     * appload_loaded once the stream has torn down. */
    g_pending_hdr_relaunch_app = app_id;
    g_pending_hdr_relaunch_coop = app_launch_coop_requested();
    g_hdr_relaunch_attempts = 0;
    commons_log_info("PIN", "HDR reconnect: quitting to relaunch app %d fresh (coop=%d)", app_id,
                     g_pending_hdr_relaunch_coop ? 1 : 0);
    /* quitapp=true clears the host's currentGame so the relaunch is a FRESH
     * gs_start_app — the only thing that re-runs Apollo's HDR display setup (a
     * resume does not). On the wake/PIN path the streamed app is the Desktop, so
     * quitting + relaunching it is harmless. */
    session_interrupt(global->session, true, STREAMING_INTERRUPT_USER);
    return true;
}

bool apps_controller_hdr_relaunch_will_retry(void) {
    return g_pending_hdr_relaunch_app > 0 && g_hdr_relaunch_attempts < HDR_RELAUNCH_MAX_ATTEMPTS;
}

void apps_controller_hdr_relaunch_succeeded(void) {
    /* A stream opened — whatever we were reconnecting for is done (or superseded by
     * a manual launch). Clear the pending state so no stale relaunch/retry fires. */
    g_pending_hdr_relaunch_app = 0;
    g_pending_hdr_relaunch_coop = false;
    g_hdr_relaunch_attempts = 0;
    if (g_hdr_relaunch_timer != NULL) {
        lv_timer_del(g_hdr_relaunch_timer);
        g_hdr_relaunch_timer = NULL;
    }
}

static void item_longpress_cb(lv_event_t *event) {
    apps_fragment_t *controller = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target(event);
    lv_obj_t *target_parent = lv_obj_get_parent(target);
    if (target_parent != controller->applist) {
        return;
    }
    lv_event_send(target, LV_EVENT_RELEASED, lv_event_get_indev(event));
    open_context_menu(controller, (appitem_viewholder_t *) lv_obj_get_user_data(target));
}

static void launcher_launch_game_monitor_off(apps_fragment_t *controller, const apploader_item_t *app) {
    LV_UNUSED(controller);
    LV_UNUSED(app);
}

static void launcher_launch_game(apps_fragment_t *controller, const apploader_item_t *app) {
    LV_ASSERT(app->base.id != 0);
    /* Every launch answers the host-monitor question, so no earlier launch's answer can carry over. */
    app_launch_choose_host_monitor(g_launch_monitor_off);
    g_launch_monitor_off = false;
    /* A RUNNING CO-OP PANE RESUMES THE WHOLE CO-OP SESSION, never one half of it. The tile is only
     * shown while co-op runs on this PC (apps_hide_idle_coop_pane); resuming it alone would stream a
     * single pane. The last pair is relaunched on BOTH hosts (each resumes its paused pane); without
     * a remembered pair the co-op screen opens instead. The scripted harness (payload) is exempt. */
    if (app->base.name != NULL && strcmp(app->base.name, COOP_SESSION_APP_NAME) == 0 &&
        !(controller->def_app_from_payload && app_launch_coop_requested())) {
        /* Consumed here too: a payload that named the pane without asking for co-op must not leave
         * the flag standing for the next, ordinary tile (the 08-12 black-screen leak below). */
        controller->def_app_from_payload = false;
        /* Relaunch the remembered pair only if THIS PC is in it; a pane started for another pair
         * (another client, another session) gets the co-op screen to choose the pair instead. */
        uuidstr_t mine = controller->uuid;
        const bool in_last_pair = app_configuration != NULL &&
                ((app_configuration->coop_primary_uuid != NULL &&
                  uuidstr_t_equals_s(&mine, app_configuration->coop_primary_uuid)) ||
                 (app_configuration->coop_secondary_uuid != NULL &&
                  uuidstr_t_equals_s(&mine, app_configuration->coop_secondary_uuid)));
        commons_log_info("COOP", "co-op pane tile chosen: %s", in_last_pair ? "relaunching the last co-op pair"
                                                                            : "opening the co-op screen");
        if (!in_last_pair || !coop_dialog_relaunch_last(controller->global)) {
            launcher_fragment_t *launcher = (launcher_fragment_t *) lv_fragment_get_parent(&controller->base);
            if (launcher != NULL) {
                coop_dialog_open(launcher);
            }
        }
        return;
    }
    /* THIS IS A SINGLE-HOST LAUNCH, SO SAY SO. CO-OP MODE DOES NOT EXPIRE BY ITSELF.
     *
     * coop.dialog.c sets app_launch_set_coop_requested(true) and coop_session_set() when a
     * co-op session starts, and NOTHING ever unset them -- not the end of the session, not
     * the teardown, not returning to the launcher. So after one co-op session every later
     * stream in the same app run was still built as a co-op pane: it asked the host for a
     * half-height 3840x1088, opened a 3840x2176 composite, and waited for a peer that was
     * never coming. Reported 2026-08-12 as "closing coop and opening pc1 has given a black
     * screen", and the diag agreed while insisting nothing was wrong -- ok=43982 climbing,
     * errors=0, and a solid black panel, because the decoder was fed a half-height picture
     * into a double-height canvas.
     *
     * This is NOT machine-specific: it is a leaked global, so any host on any PC hits it
     * the moment somebody runs co-op and then opens a single machine without restarting
     * the app. Clearing it HERE -- at the one place a single-host stream begins -- keeps
     * the two modes from being able to disagree, whatever order they are used in.
     *
     * EXCEPT THIS IS NOT ONLY WHERE A SINGLE-HOST STREAM BEGINS, and assuming it was
     * broke co-op from the launch payload outright. `ares-launch -p '{...,"coop":true}'`
     * arms co-op in app_webos.c, then auto-launches the requested app through THIS
     * function -- which immediately cleared the flag again. The payload arrived intact
     * and parsed correctly; the stream then came up solo, logging "sentinels present but
     * launch did not request co-op". Every co-op run is driven that way (run_coop.sh),
     * so co-op could not be started at all. Found 2026-08-14 on the first co-op run
     * after the leak fix.
     *
     * The payload's request belongs to the ONE launch it arrived with, so it is consumed
     * here rather than left standing: a later single-host launch in the same app run
     * clears normally and the leak stays fixed. */
    const bool payload_wants_coop = controller->def_app_from_payload && app_launch_coop_requested();
    controller->def_app_from_payload = false;
    /* Both halves matter. "From the payload" alone is not enough: a payload launch that
     * did NOT ask for co-op must still clear, or a co-op session followed by a plain
     * `-p '{host_uuid,host_app_id}'` launch in the same app run would inherit co-op and
     * reopen exactly the leak above. */
    if (!payload_wants_coop) {
        app_launch_set_coop_requested(false);
        coop_session_clear();
    }

    streaming_scene_arg_t args = {
            .global = controller->global,
            .uuid = controller->uuid,
            .app = app->base,
    };
    app_ui_t *ui = &controller->global->ui;
    lv_fragment_t *fragment = lv_fragment_create(&streaming_controller_class, &args);
    lv_obj_t *const *container = lv_fragment_get_container(lv_fragment_manager_get_top(ui->fm));
    lv_fragment_manager_push(ui->fm, fragment, container);
}

static void launcher_toggle_fav(apps_fragment_t *controller, const apploader_item_t *app) {
    pcmanager_favorite_app(pcmanager, &controller->uuid, app->base.id, !app->fav);
    apploader_load(controller->apploader);
}

static void launcher_toggle_hidden(apps_fragment_t *controller, const apploader_item_t *app) {
    pcmanager_set_app_hidden(pcmanager, &controller->uuid, app->base.id, !app->hidden);
    controller->show_hidden_apps = true;
    apploader_load(controller->apploader);
}

/* STOPPING ONE CO-OP PANE STOPS THE PAIR, like Quit inside the stream (session_worker.c, "CO-OP: QUIT
 * ENDS BOTH PANES"). Each host runs its own Co-op Pane, so "Stop streaming" on one PC's pane ended only
 * that PC and left the other paused with no partner to resume with -- the user had to stop it again from
 * the other PC's list. Every other PC whose running app is the Co-op Pane is quit too; their results come
 * back through quitgame_cb, which ignores any PC but the one on screen. */
static void launcher_quit_coop_peers(const apps_fragment_t *controller, int app_id) {
    const int coop_id = app_configuration != NULL && app_configuration->coop_app_id > 0
                        ? app_configuration->coop_app_id : COOP_APP_ID_DEFAULT;
    if (app_id == 0 || app_id != coop_id) {
        return;
    }
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        if (cur->server == NULL || uuidstr_t_equals_t(&cur->id, &controller->uuid)) {
            continue;
        }
        if (cur->server->currentGame != coop_id) {
            continue;
        }
        commons_log_info("Launcher", "Stop streaming on the Co-op Pane: also quitting it on %s",
                         cur->server->hostname);
        pcmanager_quitapp(pcmanager, &cur->id, quitgame_cb, NULL);
    }
}

static void launcher_quit_game(apps_fragment_t *controller) {
    const int quitting = pcmanager_server_current_app(pcmanager, &controller->uuid);
    controller->quit_progress = progress_dialog_create(locstr("Quitting game..."));
    pcmanager_quitapp(pcmanager, &controller->uuid, quitgame_cb, NULL);
    launcher_quit_coop_peers(controller, quitting);
}

static int adapter_item_count(lv_obj_t *grid, void *data) {
    if (data == NULL) { return 0; }
    apps_fragment_t *controller = lv_obj_get_user_data(grid);
    apploader_list_t *list = data;
    // LVGL can only display up to 255 rows/columns, but I don't think anyone has library that big (1275 items)
    int count = LV_MIN(list->count, 255 * controller->col_count);
    if (!controller->show_hidden_apps) {
        for (int i = 0; i < count; i++) {
            if (list->items[i].hidden) {
                count = i;
                break;
            }
        }
    }
    return count;
}

static lv_obj_t *adapter_create_view(lv_obj_t *parent) {
    apps_fragment_t *controller = lv_obj_get_user_data(parent);
    return appitem_view(controller, parent);
}

static void adapter_bind_view(lv_obj_t *grid, lv_obj_t *item_view, void *data, int position) {
    apps_fragment_t *controller = lv_obj_get_user_data(grid);
    apploader_list_t *list = data;
    appitem_bind(controller, item_view, &list->items[position]);

    // IDE seems to be pretty confused...
    LV_UNUSED(list);
    LV_UNUSED(position);
}


static void applist_focus_enter(lv_event_t *event) {
    if (event->target != event->current_target) { return; }
    apps_fragment_t *controller = lv_event_get_user_data(event);
    int idx = controller->focus_backup >= 0 ? controller->focus_backup : 0;
    lv_gridview_focus(controller->applist, idx);
}

static void applist_focus_leave(lv_event_t *event) {
    if (event->target != event->current_target) { return; }
    apps_fragment_t *controller = lv_event_get_user_data(event);
    controller->focus_backup = lv_gridview_get_focused_index(controller->applist);
    lv_gridview_focus(controller->applist, -1);
}

static void quitgame_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata) {
    LV_UNUSED(userdata);
    apps_fragment_t *controller = current_instance;
    if (controller == NULL) { return; }
    if (!uuidstr_t_equals_t(&controller->uuid, uuid)) { return; }
    if (controller->quit_progress) {
        lv_msgbox_close(controller->quit_progress);
        controller->quit_progress = NULL;
    }
    lv_gridview_rebind(controller->applist);
    const int launch_after_quit = controller->launch_after_quit;
    controller->launch_after_quit = 0;
    if (result == GS_OK) {
        if (launch_after_quit != 0) {
            const apploader_item_t *next = apploader_list_item_by_id(controller->apploader_apps, launch_after_quit);
            if (next != NULL) {
                launcher_launch_game(controller, next);
            }
        }
        return;
    }
    static const char *btn_texts[] = {translatable("OK"), ""};
    lv_obj_t *dialog = lv_msgbox_create_i18n(NULL, locstr("Unable to quit game"),
                                             locstr("Please make sure you are quitting with the same client."),
                                             btn_texts, false);
    lv_obj_add_event_cb(dialog, quit_dialog_cb, LV_EVENT_VALUE_CHANGED, controller);
    lv_obj_center(dialog);
}

static void quit_dialog_cb(lv_event_t *event) {
    lv_obj_t *dialog = lv_event_get_current_target(event);
    lv_msgbox_close_async(dialog);
}

/** "X is running on this PC. Quit it and start Y?" -- the choice the silent return used to hide. */
static void switch_app_dialog(apps_fragment_t *controller, int running_app, const apploader_item_t *app) {
    const apploader_item_t *running = apploader_list_item_by_id(controller->apploader_apps, running_app);
    const pclist_t *node = pcmanager_node(pcmanager, &controller->uuid);
    const char *pc = node != NULL && node->server != NULL && node->server->hostname != NULL
                     ? node->server->hostname : locstr("this PC");
    char text[512];
    snprintf(text, sizeof(text), locstr("%s is running on %s. Quit it and start %s?"),
             running != NULL ? running->base.name : locstr("Another app"), pc, app->base.name);
    /* "Yes", not "Quit and start": the question already says what happens, and a msgbox button
     * has a fixed width that the longer label overflowed (clipped to "uit and start"). */
    static const char *btn_texts[] = {translatable("Yes"), translatable("Cancel"), ""};
    lv_obj_t *dialog = lv_msgbox_create_i18n(NULL, locstr("Another app is running"), text, btn_texts, false);
    lv_obj_set_user_data(dialog, (void *) (intptr_t) app->base.id);
    lv_obj_add_event_cb(dialog, switch_app_dialog_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(dialog);
}

static void switch_app_dialog_cb(lv_event_t *event) {
    lv_obj_t *dialog = lv_event_get_current_target(event);
    const int app_id = (int) (intptr_t) lv_obj_get_user_data(dialog);
    const bool confirmed = lv_msgbox_get_active_btn(dialog) == 0;
    lv_msgbox_close_async(dialog);
    /* The launcher that opened this is looked up again rather than remembered: it is the
     * same guard quitgame_cb uses, should the view have been rebuilt meanwhile. */
    apps_fragment_t *controller = current_instance;
    if (!confirmed || controller == NULL || app_id == 0) {
        return;
    }
    if (pcmanager_server_current_app(pcmanager, &controller->uuid) == 0) {
        /* It stopped by itself while the question was up: nothing left to quit. */
        const apploader_item_t *app = apploader_list_item_by_id(controller->apploader_apps, app_id);
        if (app != NULL) {
            launcher_launch_game(controller, app);
        }
        return;
    }
    controller->launch_after_quit = app_id;
    launcher_quit_game(controller);
}

static void actions_click_cb(lv_event_t *event) {
    apps_fragment_t *controller = lv_event_get_user_data(event);
    uint16_t index = lv_btnmatrix_get_selected_btn(controller->actions);
    const action_cb_t *actions = lv_obj_get_user_data(controller->actions);
    actions[index](controller, controller->actions, index);
}

static void action_cb_wol(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index) {
    LV_UNUSED(index);
    lv_btnmatrix_set_btn_ctrl_all(buttons, LV_BTNMATRIX_CTRL_DISABLED);
    pcmanager_send_wol(pcmanager, &controller->uuid, send_wol_cb, NULL);
    /* Arm PIN auto-entry so the stream that follows this wake auto-unlocks the host. */
    pin_autoentry_arm(&controller->uuid);
}

static void action_cb_host_reload(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index) {
    LV_UNUSED(index);
    lv_btnmatrix_set_btn_ctrl_all(buttons, LV_BTNMATRIX_CTRL_DISABLED);
    pcmanager_request_update(pcmanager, &controller->uuid, host_info_cb, NULL);
}

static void action_cb_pair(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index) {
    LV_UNUSED(buttons);
    LV_UNUSED(index);
    pair_dialog_open(&controller->uuid);
}

static void open_context_menu(apps_fragment_t *fragment, appitem_viewholder_t *holder) {
    const apploader_item_t *app = apploader_list_item_by_id(fragment->apploader_apps, holder->app_id);
    assert(app != NULL);
    lv_obj_t *msgbox = lv_msgbox_create(NULL, app->base.name, NULL, NULL, false);
    /* THE APP, NOT THE TILE. The grid rebuilds its tiles on every app-list reload (each host status update, every
     * few seconds) and hands this holder to another app; read back at the click, it launched whichever app the tile
     * now showed -- measured 2026-10-15: the "Desktop" menu started DAVE THE DIVER, and its Info showed that id. */
    lv_obj_set_user_data(msgbox, (void *) (intptr_t) app->base.id);
    lv_obj_t *content = lv_msgbox_get_content(msgbox);
    lv_obj_add_flag(content, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    lv_obj_add_event_cb(content, context_menu_cancel_cb, LV_EVENT_CANCEL, fragment);
    lv_obj_add_event_cb(content, context_menu_click_cb, LV_EVENT_SHORT_CLICKED, fragment);

    int currentId = pcmanager_server_current_app(pcmanager, &fragment->uuid);
    if (!currentId || currentId == app->base.id) {
        lv_obj_t *start_btn = lv_list_add_btn(content, NULL,
                                              currentId == app->base.id ? locstr("Resume streaming")
                                                                        : locstr("Start streaming"));
        lv_obj_add_flag(start_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_user_data(start_btn, launcher_launch_game);
        if (host_monitor_choice_here(fragment)) {
            lv_obj_t *off_btn = lv_list_add_btn(content, NULL,
                                                currentId == app->base.id ? locstr("Resume streaming (host monitor off)")
                                                                          : locstr("Start streaming (host monitor off)"));
            lv_obj_add_flag(off_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_set_user_data(off_btn, launcher_launch_game_monitor_off);
        }
    }

    if (currentId) {
        lv_obj_t *quit_btn = lv_list_add_btn(content, NULL, locstr("Stop streaming"));
        lv_obj_add_flag(quit_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_user_data(quit_btn, launcher_quit_game);
    }
    lv_obj_t *fav_btn = lv_list_add_btn(content, NULL, app->fav ? locstr("Unstar") : locstr("Star"));
    lv_obj_add_flag(fav_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_user_data(fav_btn, launcher_toggle_fav);

    lv_obj_t *hide_btn = lv_list_add_btn(content, NULL, app->hidden ? locstr("Unhide") : locstr("Hide"));
    lv_obj_add_flag(hide_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_user_data(hide_btn, launcher_toggle_hidden);

    lv_obj_t *info_btn = lv_list_add_btn(content, NULL, locstr("Info"));
    lv_obj_add_flag(info_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_user_data(info_btn, app_detail_dialog);

    lv_obj_t *cancel_btn = lv_list_add_btn(content, NULL, locstr("Cancel"));
    lv_obj_add_flag(cancel_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_center(msgbox);
}

static void context_menu_cancel_cb(lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    if (target->parent != lv_event_get_current_target(e)) { return; }
    lv_msgbox_close(lv_event_get_current_target(e)->parent);
}

static void context_menu_click_cb(lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    lv_obj_t *current_target = lv_event_get_current_target(e);
    if (target->parent != current_target) { return; }
    lv_obj_t *mbox = lv_event_get_current_target(e)->parent;
    apps_fragment_t *self = lv_event_get_user_data(e);
    const int app_id = (int) (intptr_t) lv_obj_get_user_data(mbox);
    const apploader_item_t *app = apploader_list_item_by_id(self->apploader_apps, app_id);
    if (app == NULL) {
        /* Gone from the host's list since the menu opened: nothing to act on. */
        lv_msgbox_close_async(mbox);
        return;
    }
    if (lv_obj_get_user_data(target) == launcher_quit_game) {
        launcher_quit_game(self);
    } else if (lv_obj_get_user_data(target) == launcher_launch_game) {
        launcher_launch_game(self, app);
    } else if (lv_obj_get_user_data(target) == launcher_launch_game_monitor_off) {
        g_launch_monitor_off = true;
        launcher_launch_game(self, app);
    } else if (lv_obj_get_user_data(target) == launcher_toggle_fav) {
        launcher_toggle_fav(self, app);
    } else if (lv_obj_get_user_data(target) == launcher_toggle_hidden) {
        launcher_toggle_hidden(self, app);
    } else if (lv_obj_get_user_data(target) == app_detail_dialog) {
        app_detail_dialog(self, app);
    }
    lv_msgbox_close_async(mbox);
}

static void app_detail_dialog(apps_fragment_t *fragment, const apploader_item_t *app) {
    LV_UNUSED(fragment);
    static const char *btn_txts[] = {translatable("OK"), ""};
    lv_obj_t *msgbox = lv_msgbox_create_i18n(NULL, app->base.name, "text", btn_txts, false);
    lv_obj_t *msgobj = lv_msgbox_get_text(msgbox);
    lv_label_set_text_fmt(msgobj,
                          "ID: %d\n"
                          "Support HDR: %s",
                          app->base.id, app->base.hdr ? "Yes" : "No");
    lv_obj_center(msgbox);
    lv_obj_add_event_cb(msgbox, app_detail_click_cb, LV_EVENT_CLICKED, NULL);
}

static void app_detail_click_cb(lv_event_t *event) {
    lv_msgbox_close(lv_event_get_current_target(event));
}

static void set_actions(apps_fragment_t *controller, const char **labels, const action_cb_t *callbacks) {
    lv_btnmatrix_set_map(controller->actions, labels);
    int num_actions = 0;
    for (int i = 0; labels[i][0] != '\0'; i++) {
        num_actions++;
    }
    lv_obj_set_style_min_width(controller->actions, LV_PCT(20 * num_actions), 0);
    lv_obj_set_user_data(controller->actions, (void *) callbacks);
}


static lv_gridview_data_change_t *apps_list_detect_change(const apploader_list_t *old_list,
                                                          const apploader_list_t *new_list, int *num_changes) {
    if (old_list == NULL && new_list == NULL) {
        *num_changes = 0;
        return NULL;
    } else if ((old_list != NULL) != (new_list != NULL)) {
        *num_changes = -1;
        return NULL;
    } else if (old_list->count != new_list->count) {
        *num_changes = -1;
        return NULL;
    }
    for (int i = 0; i < old_list->count; i++) {
        if (old_list->items[i].base.id != new_list->items[i].base.id) {
            *num_changes = -1;
            return NULL;
        }
    }
    *num_changes = 0;
    return NULL;
}
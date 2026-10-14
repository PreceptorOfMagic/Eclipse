#include "settings.controller.h"

#include <string.h>
#include <stdio.h>

#include "ui/root.h"

#include "lvgl/font/material_icons_regular_symbols.h"
#include "lvgl/ext/lv_child_group.h"
#include "lvgl/util/lv_app_utils.h"

#include "util/user_event.h"
#include "util/font.h"
#include "util/i18n.h"
#include "app.h"
#include "stream/session.h"
#include "lvgl/theme/lv_theme_moonlight.h"
#include "lvgl/theme/lv_theme_moonlight_colors.h"

#include "../launcher/launcher.controller.h"
#if TARGET_ANDROID
#include "platform/android/android_platform.h"
#endif

typedef struct {
    const char *icon;
    const char *name;
    const lv_fragment_class_t *cls;
} settings_entry_t;

static const settings_entry_t entries[] = {
        {MAT_SYMBOL_SETTINGS,        translatable("Basic Settings"), &settings_pane_basic_cls},
        {MAT_SYMBOL_DESKTOP_WINDOWS, translatable("Host Settings"),  &settings_pane_host_cls},
        {MAT_SYMBOL_SPORTS_ESPORTS,  translatable("Input Settings"), &settings_pane_input_cls},
        {MAT_SYMBOL_VOLUME_UP,       translatable("Audio Settings"), &settings_pane_audio_cls},
        {MAT_SYMBOL_VIDEO_SETTINGS,  translatable("Video Settings"), &settings_pane_video_cls},
        /* NOT SPORTS_ESPORTS -- that is Input Settings' icon and the two tabs were
         * indistinguishable in the nav rail. ADD_TO_QUEUE is a display with a plus
         * on it: a second screen joining the first, which is what co-op split is.
         * The icon font is SUBSETTED (21 glyphs, src/app/lvgl/font/
         * material_icons_regular_symbols.h) and there is no generator script in the
         * tree, so a literal split-screen glyph would mean regenerating the font --
         * this is the closest unused glyph that still reads as the feature. */
        {MAT_SYMBOL_ADD_TO_QUEUE,    translatable("Co-op Split"),    &settings_pane_coop_cls},
        {MAT_SYMBOL_HELP,            translatable("About"),          &settings_pane_about_cls},
};
static const int entries_len = sizeof(entries) / sizeof(settings_entry_t);

static void on_view_created(lv_fragment_t *self, lv_obj_t *view);

static void on_destroy_view(lv_fragment_t *self, lv_obj_t *view);

static void on_entry_focus(lv_event_t *event);

static void on_entry_click(lv_event_t *event);

static void on_nav_key(lv_event_t *event);

static void on_detail_key(lv_event_t *e);

static void on_back_request(lv_event_t *e);

static void on_tab_key(lv_event_t *event);

static void on_tab_content_key(lv_event_t *e);

static void settings_dropdown_esc_preprocess_cb(lv_event_t *e);

static void settings_dropdown_release_preprocess_cb(lv_event_t *e);

static void settings_dropdown_arrow_preprocess_cb(lv_event_t *e);

static void on_dropdown_clicked(lv_event_t *event);

static void settings_controller_ctor(lv_fragment_t *self, void *args);

static bool on_event(lv_fragment_t *self, int code, void *userdata);

static void detail_defocus(settings_controller_t *controller, lv_event_t *e);

static bool detail_item_needs_lrkey(lv_obj_t *obj);

static void show_pane(settings_controller_t *controller, const lv_fragment_class_t *cls);

static void settings_close(lv_event_t *e);

static bool settings_finish_close(settings_controller_t *fragment);

static bool settings_try_close(settings_controller_t *fragment);

static void stream_reconnect_confirm_cb(lv_event_t *e);

static void app_restart_confirm_cb(lv_event_t *e);

static void pane_child_added(lv_event_t *e);

static void settings_launcher_detach(settings_controller_t *fragment);

static void settings_close_pane_popup(settings_controller_t *c);

static void settings_request_close_pane_popup(settings_controller_t *c);

static void settings_show_pane_popup(settings_controller_t *c, const lv_fragment_class_t *cls);

static void on_launcher_embedded_view_created(settings_controller_t *controller);

static void embed_cancel_cb(lv_event_t *e);

static void settings_embed_refocus_appbar(settings_controller_t *c);

static void embed_popup_add_objs_recursive(lv_obj_t *parent, lv_group_t *g);

static lv_obj_t *embed_popup_first_focusable(lv_obj_t *parent);

static void embed_popup_attach_key_handlers(lv_obj_t *parent, settings_controller_t *c);

static void settings_pane_fragment_destroy(settings_controller_t *c);

static void settings_style_pane_msgbox_amoled(lv_obj_t *mbox);

static void on_textarea_focused(lv_event_t *e);

static void on_textarea_defocused(lv_event_t *e);

static void embed_popup_cancel_cb(lv_event_t *e);

static void settings_dropdown_cancel_cb(lv_event_t *e);

static bool settings_dropdown_list_open(settings_controller_t *c, lv_obj_t *target);

static bool settings_close_dropdown_on_back(settings_controller_t *c, lv_obj_t *target);

static lv_group_t *settings_nav_group_for(settings_controller_t *c);

static void settings_dropdown_set_list_editing(settings_controller_t *c, bool editing);

static lv_obj_t *settings_active_dropdown_get(settings_controller_t *c);

static void settings_clear_active_dropdown(settings_controller_t *c, bool close);

static void settings_dropdown_deleted_cb(lv_event_t *e);

static void pane_child_attach_handlers(settings_controller_t *controller, lv_obj_t *child, bool popup);

static void pane_popup_child_added(lv_event_t *e);

static void settings_save_error_cb(lv_event_t *e);

#define UI_IS_MINI(width) ((width) < LV_DPX(240))
#define SETTINGS_HANDLERS_ATTACHED_FLAG LV_OBJ_FLAG_USER_2

const lv_fragment_class_t settings_controller_cls = {
        .constructor_cb = settings_controller_ctor,
        .create_obj_cb = settings_win_create,
        .obj_created_cb = on_view_created,
        .obj_deleted_cb = on_destroy_view,
        .event_cb = on_event,
        .instance_size = sizeof(settings_controller_t),
};

void settings_navigation_exclude(lv_obj_t *obj) {
    if (obj == NULL) {
        return;
    }
    lv_obj_add_flag(obj, SETTINGS_NAV_EXCLUDE_FLAG);
    lv_group_remove_obj(obj);
}

static void settings_controller_ctor(lv_fragment_t *self, void *args) {
    settings_controller_t *fragment = (settings_controller_t *) self;
    settings_open_args_t *open = (settings_open_args_t *) args;
    fragment->app = open->app;
    fragment->launcher_host = open->launcher;
    fragment->pane_mbox = NULL;
    fragment->pane_fragment = NULL;
    fragment->pane_popup_group = NULL;
    fragment->embed_root = NULL;
    fragment->embed_appbar = NULL;
    fragment->embed_origin_btn = NULL;
    fragment->needs_app_restart = false;
    fragment->needs_stream_reconnect = false;
    fragment->needs_locale_reapply = false;
    fragment->save_complete = false;
    fragment->mini = fragment->pending_mini = UI_IS_MINI(fragment->app->ui.width);
    os_info_get(&fragment->os_info);
#if TARGET_WEBOS
    if (!SDL_webOSGetPanelResolution(&fragment->panel_width, &fragment->panel_height)) {
        fragment->panel_width = 1920;
        fragment->panel_height = 1080;
    }
    if (!SDL_webOSGetRefreshRate(&fragment->panel_fps)) {
        fragment->panel_fps = 60;
    }
#elif TARGET_ANDROID
    /* The TV's physical mode and the fastest refresh it offers at that size: the activity switches the
     * display to the stream's rate when a stream starts (platform/android, android_display_request_refresh). */
    int max_refresh_x100 = 0;
    if (!android_display_mode(&fragment->panel_width, &fragment->panel_height, NULL, &max_refresh_x100, NULL)) {
        fragment->panel_width = 1920;
        fragment->panel_height = 1080;
    }
    fragment->panel_fps = max_refresh_x100 > 0 ? (max_refresh_x100 + 50) / 100 : 60;
#endif
}

static void on_view_created(lv_fragment_t *self, lv_obj_t *view) {
    LV_UNUSED(view);
    settings_controller_t *controller = (settings_controller_t *) self;
    if (controller->launcher_host) {
        on_launcher_embedded_view_created(controller);
        return;
    }
    lv_obj_add_event_cb(controller->close_btn, settings_close, LV_EVENT_CLICKED, controller);
    if (controller->mini) {
        controller->nav_group = lv_group_create();
        controller->tab_groups = lv_mem_alloc(sizeof(lv_group_t *) * entries_len);
        app_input_set_group(&controller->app->ui.input, controller->nav_group);

        lv_obj_t *btns = lv_tabview_get_tab_btns(controller->tabview);
        lv_obj_set_style_text_font(btns, lv_theme_moonlight_get_iconfont_large(btns), 0);
        lv_group_remove_obj(btns);

        lv_group_add_obj(controller->nav_group, controller->nav);
        lv_obj_add_event_cb(controller->nav, on_tab_key, LV_EVENT_KEY, controller);

        for (int i = 0; i < entries_len; ++i) {
            settings_entry_t entry = entries[i];
            lv_group_t *tab_group = lv_group_create();
            controller->tab_groups[i] = tab_group;
            lv_obj_t *page = lv_tabview_add_tab(controller->tabview, entry.icon);
            lv_obj_add_event_cb(page, cb_child_group_add, LV_EVENT_CHILD_CREATED, tab_group);
            lv_obj_add_event_cb(page, pane_child_added, LV_EVENT_CHILD_CREATED, controller);
            lv_fragment_t *pane = lv_fragment_create(entry.cls, controller);
            lv_fragment_create_obj(pane, page);
            lv_obj_set_user_data(page, pane);

            lv_obj_t *tab_focused = lv_group_get_focused(tab_group);
            if (tab_focused) {
                lv_obj_clear_state(tab_focused, LV_STATE_FOCUS_KEY);
            }
        }
    } else {
        controller->nav_group = lv_group_create();
        controller->detail_group = lv_group_create();
        lv_group_set_wrap(controller->detail_group, false);

        lv_obj_add_event_cb(controller->nav, cb_child_group_add, LV_EVENT_CHILD_CREATED, controller->nav_group);
        lv_obj_add_event_cb(controller->detail, cb_child_group_add, LV_EVENT_CHILD_CREATED, controller->detail_group);
        lv_obj_add_event_cb(controller->detail, pane_child_added, LV_EVENT_CHILD_CREATED, controller);
        lv_obj_add_event_cb(controller->detail, on_back_request, LV_EVENT_CANCEL, controller);

        lv_obj_add_event_cb(controller->nav, on_entry_focus, LV_EVENT_FOCUSED, controller);
        lv_obj_add_event_cb(controller->nav, on_entry_click, LV_EVENT_CLICKED, controller);
        lv_obj_add_event_cb(controller->nav, on_nav_key, LV_EVENT_KEY, controller);
        lv_obj_add_event_cb(controller->nav, on_back_request, LV_EVENT_CANCEL, controller);

        app_input_set_group(&controller->app->ui.input, controller->nav_group);

        for (int i = 0; i < entries_len; ++i) {
            settings_entry_t entry = entries[i];
            lv_obj_t *item_view = lv_list_add_btn(controller->nav, entry.icon, locstr(entry.name));
            lv_btn_set_icon_font(item_view, lv_theme_moonlight_get_iconfont_normal(item_view));

            lv_obj_set_style_bg_opa(item_view, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
            lv_obj_add_flag(item_view, LV_OBJ_FLAG_EVENT_BUBBLE);
            item_view->user_data = (void *) entry.cls;
        }
        show_pane(controller, entries[0].cls);
    }
}

static void on_destroy_view(lv_fragment_t *self, lv_obj_t *view) {
    settings_controller_t *controller = (settings_controller_t *) self;
    LV_UNUSED(view);
    if (!controller->save_complete) {
        (void) settings_save(app_configuration);
    }

    if (controller->launcher_host) {
        settings_close_pane_popup(controller);
        launcher_restore_nav_focus(controller->launcher_host);
        lv_group_del(controller->nav_group);
        return;
    }
    app_input_set_group(&controller->app->ui.input, NULL);
    if (controller->mini) {
        for (int i = 0; i < entries_len; i++) {
            lv_group_del(controller->tab_groups[i]);
        }
        lv_mem_free(controller->tab_groups);
        lv_group_del(controller->nav_group);
    } else {
        lv_group_del(controller->nav_group);
        lv_group_del(controller->detail_group);
    }
}

static bool on_event(lv_fragment_t *self, int code, void *userdata) {
    LV_UNUSED(userdata);
    settings_controller_t *controller = (settings_controller_t *) self;
    app_ui_t *ui = &controller->app->ui;
    switch (code) {
        case USER_SIZE_CHANGED: {
            lv_obj_set_size(self->obj, ui->width, ui->height);
            if (controller->launcher_host) {
                break;
            }
            bool mini = UI_IS_MINI(ui->width);
            if (mini != controller->mini) {
                controller->pending_mini = mini;
                lv_fragment_recreate_obj(self);
            }
            break;
        }
    }
    return false;
}

static void on_entry_focus(lv_event_t *event) {
    settings_controller_t *controller = event->user_data;
    if (controller->launcher_host) {
        return;
    }
    if (controller->base.managed->destroying_obj) { return; }
    lv_obj_t *target = lv_event_get_target(event);
    if (lv_obj_get_parent(target) != controller->nav) { return; }
    lv_fragment_t *pane = lv_fragment_manager_get_top(controller->base.child_manager);
    lv_fragment_class_t *cls = target->user_data;
    if (pane && pane->cls == cls) {
        return;
    }
    for (int i = 0, j = (int) lv_obj_get_child_cnt(controller->nav); i < j; i++) {
        lv_obj_t *child = lv_obj_get_child(controller->nav, i);
        if (child == target) {
            lv_obj_add_state(child, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(child, LV_STATE_CHECKED);
        }
    }
    show_pane(controller, cls);
}

static void show_pane(settings_controller_t *controller, const lv_fragment_class_t *cls) {
    /* A pane owns its dropdown objects.  Never let a raw pointer to one cross a
     * pane replacement: pointer navigation can switch sections without taking
     * the keyboard/controller Back path which normally clears it. */
    settings_clear_active_dropdown(controller, true);
    if (controller->launcher_host) {
        settings_show_pane_popup(controller, cls);
        return;
    }
    lv_fragment_t *fragment = lv_fragment_create(cls, controller);
    lv_fragment_manager_replace(controller->base.child_manager, fragment, &controller->detail);
    lv_obj_scroll_to_y(controller->detail, 0, LV_ANIM_OFF);
    lv_obj_t *focused = lv_group_get_focused(controller->detail_group);
    lv_event_send(focused, LV_EVENT_DEFOCUSED, NULL);
}

static void on_entry_click(lv_event_t *event) {
    settings_controller_t *controller = event->user_data;
    lv_obj_t *target = lv_event_get_target(event);
    if (lv_obj_get_parent(target) != controller->nav) { return; }
    lv_fragment_t *pane = lv_fragment_manager_find_by_container(controller->base.child_manager,
                                                                controller->detail);
    if (!pane) { return; }
    lv_obj_t *first_focusable = NULL;
    for (int i = 0, j = (int) lv_obj_get_child_cnt(pane->obj); i < j; i++) {
        lv_obj_t *child = lv_obj_get_child(pane->obj, i);
        if (lv_obj_get_group(child)) {
            first_focusable = child;
            break;
        }
    }
    if (!first_focusable) { return; }
    app_input_set_group(&controller->app->ui.input, controller->detail_group);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_KEYPAD) { return; }
    lv_group_focus_obj(first_focusable);
}

static void on_nav_key(lv_event_t *event) {
    settings_controller_t *controller = event->user_data;
    switch (lv_event_get_key(event)) {
        case LV_KEY_DOWN: {
            lv_obj_t *target = lv_event_get_target(event);
            if (lv_obj_get_parent(target) != controller->nav) { return; }
            lv_group_t *group = controller->nav_group;
            lv_group_focus_next(group);
            break;
        }
        case LV_KEY_UP: {
            lv_obj_t *target = lv_event_get_target(event);
            if (lv_obj_get_parent(target) != controller->nav) { return; }
            lv_group_t *group = controller->nav_group;
            lv_group_focus_prev(group);
            break;
        }
        case LV_KEY_RIGHT: {
            lv_obj_t *target = lv_event_get_target(event);
            if (lv_obj_get_parent(target) != controller->nav) { return; }
            on_entry_click(event);
            break;
        }
    }
}

static void on_detail_key(lv_event_t *e) {
    settings_controller_t *controller = e->user_data;
    if (controller->mini) {
        on_tab_content_key(e);
        return;
    }
    lv_group_t *nav_detail = controller->pane_popup_group ? controller->pane_popup_group : controller->detail_group;
    if (!nav_detail) {
        return;
    }
    lv_obj_t *target = lv_event_get_target(e);
    const uint32_t key = lv_event_get_key(e);

    if (controller->pane_popup_group != NULL) {
        if (lv_event_get_target(e) != lv_event_get_current_target(e)) {
            return;
        }
        switch (key) {
            case LV_KEY_ESC:
                if (lv_obj_check_type(target, &lv_textarea_class) && lv_group_get_editing(nav_detail)) {
                    lv_group_set_editing(nav_detail, false);
                    return;
                }
                if (settings_close_dropdown_on_back(controller, target)) {
                    return;
                }
                if (controller->pane_mbox != NULL) {
                    lv_event_send(controller->pane_mbox, LV_EVENT_CANCEL, lv_indev_get_act());
                }
                return;
            case LV_KEY_ENTER:
                if (lv_obj_has_class(target, &lv_dropdown_class)) {
                    if (!lv_dropdown_is_open(target)) {
                        lv_dropdown_open(target);
                        controller->active_dropdown = target;
                        settings_dropdown_set_list_editing(controller, true);
                    }
                    return;
                }
                if (lv_obj_check_type(target, &lv_textarea_class)) {
                    lv_group_set_editing(nav_detail, true);
                    return;
                }
                return;
            case LV_KEY_UP:
                if (settings_dropdown_list_open(controller, target)) {
                    lv_event_stop_processing(e);
                    return;
                }
                if (lv_obj_check_type(target, &lv_textarea_class) && lv_group_get_editing(nav_detail)) {
                    return;
                }
                if (lv_obj_has_class(target, &lv_dropdown_class)) {
                    lv_group_focus_prev(nav_detail);
                    lv_event_stop_processing(e);
                    return;
                }
                lv_group_focus_prev(nav_detail);
                return;
            case LV_KEY_DOWN:
                if (settings_dropdown_list_open(controller, target)) {
                    lv_event_stop_processing(e);
                    return;
                }
                if (lv_obj_check_type(target, &lv_textarea_class) && lv_group_get_editing(nav_detail)) {
                    return;
                }
                if (lv_obj_has_class(target, &lv_dropdown_class)) {
                    lv_group_focus_next(nav_detail);
                    lv_event_stop_processing(e);
                    return;
                }
                lv_group_focus_next(nav_detail);
                return;
            case LV_KEY_LEFT:
                if (settings_dropdown_list_open(controller, target)) {
                    lv_event_stop_processing(e);
                    return;
                }
                if (lv_obj_check_type(target, &lv_textarea_class) && lv_group_get_editing(nav_detail)) {
                    return;
                }
                if (detail_item_needs_lrkey(target)) {
                    return;
                }
                if (lv_obj_has_class(target, &lv_dropdown_class)) {
                    lv_group_focus_prev(nav_detail);
                    lv_event_stop_processing(e);
                    return;
                }
                lv_group_focus_prev(nav_detail);
                return;
            case LV_KEY_RIGHT:
                if (settings_dropdown_list_open(controller, target)) {
                    lv_event_stop_processing(e);
                    return;
                }
                if (lv_obj_check_type(target, &lv_textarea_class) && lv_group_get_editing(nav_detail)) {
                    return;
                }
                if (detail_item_needs_lrkey(target)) {
                    return;
                }
                if (lv_obj_has_class(target, &lv_dropdown_class)) {
                    lv_group_focus_next(nav_detail);
                    lv_event_stop_processing(e);
                    return;
                }
                lv_group_focus_next(nav_detail);
                return;
            default:
                return;
        }
    }

    switch (key) {
        case LV_KEY_ESC: {
            if (settings_close_dropdown_on_back(controller, target)) {
                lv_event_stop_bubbling(e);
                break;
            }
            detail_defocus(controller, e);
            lv_event_stop_bubbling(e);
            break;
        }
        case LV_KEY_ENTER: {
            if (lv_obj_has_class(target, &lv_dropdown_class)) {
                if (!lv_dropdown_is_open(target)) {
                    lv_dropdown_open(target);
                    controller->active_dropdown = target;
                    settings_dropdown_set_list_editing(controller, true);
                }
                lv_event_stop_bubbling(e);
                break;
            }
            if (lv_obj_check_type(target, &lv_textarea_class)) {
                lv_group_set_editing(nav_detail, true);
                lv_event_stop_bubbling(e);
            }
            break;
        }
        case LV_KEY_UP: {
            if (settings_dropdown_list_open(controller, target)) { return; }
            lv_group_focus_prev(nav_detail);
            break;
        }
        case LV_KEY_DOWN: {
            if (settings_dropdown_list_open(controller, target)) { return; }
            lv_group_focus_next(nav_detail);
            break;
        }
        case LV_KEY_LEFT: {
            if (detail_item_needs_lrkey(target)) { return; }
            if (settings_dropdown_list_open(controller, target)) { return; }
            detail_defocus(controller, e);
            break;
        }
        case LV_KEY_RIGHT: {
            if (detail_item_needs_lrkey(target)) { return; }
            if (settings_dropdown_list_open(controller, target)) { return; }
            if (lv_obj_has_class(target, &lv_dropdown_class)) {
                lv_group_focus_next(nav_detail);
                lv_event_stop_bubbling(e);
            }
            break;
        }
    }
}

static void on_back_request(lv_event_t *e) {
    if (lv_event_get_param(e) == NULL) { return; }
    settings_controller_t *controller = e->user_data;
    lv_obj_t *target = lv_event_get_target(e);
    if (settings_close_dropdown_on_back(controller, target)) {
        return;
    }
    if (lv_obj_has_state(controller->detail, LV_STATE_FOCUS_KEY)) {
        detail_defocus(controller, e);
    } else {
        settings_close(e);
    }
}

static void on_tab_key(lv_event_t *event) {
    settings_controller_t *controller = event->user_data;
    switch (lv_event_get_key(event)) {
        case LV_KEY_ESC:
            /* In the compact desktop layout the first Escape moves focus from a
             * setting back to the tab strip.  The tab strip itself receives key
             * events (not LV_EVENT_CANCEL), so it must own the second Escape and
             * close Settings.  Without this branch keyboard/controller users are
             * trapped at the top level of the compact settings view. */
            (void) settings_try_close(controller);
            lv_event_stop_bubbling(event);
            break;
        case LV_KEY_LEFT: {
            uint16_t act = lv_tabview_get_tab_act(controller->tabview);
            if (act <= 0) { return; }
            lv_tabview_set_act(controller->tabview, act - 1, true);
            break;
        }
        case LV_KEY_RIGHT: {
            uint16_t act = lv_tabview_get_tab_act(controller->tabview);
            if (act + 1 >= entries_len) { return; }
            lv_tabview_set_act(controller->tabview, act + 1, true);
            break;
        }
        case LV_KEY_UP:
        case LV_KEY_DOWN:
        case LV_KEY_ENTER: {
            uint16_t act = lv_tabview_get_tab_act(controller->tabview);
            lv_group_t *content_group = controller->tab_groups[act];
            if (lv_group_get_obj_count(content_group) == 0) {
                break;
            }
            app_input_set_group(&controller->app->ui.input, content_group);
            lv_obj_t *focused = lv_group_get_focused(content_group);
            if (focused) {
                lv_obj_add_state(focused, LV_STATE_FOCUS_KEY);
            }
            break;
        }
    }
}

static void on_tab_content_key(lv_event_t *e) {
    settings_controller_t *controller = e->user_data;
    lv_obj_t *target = lv_event_get_target(e);
    uint16_t act = lv_tabview_get_tab_act(controller->tabview);
    lv_group_t *group = controller->tab_groups[act];
    switch (lv_event_get_key(e)) {
        case LV_KEY_ESC: {
            if (settings_close_dropdown_on_back(controller, target)) {
                return;
            }
            if (lv_obj_check_type(target, &lv_textarea_class) && lv_group_get_editing(group)) {
                lv_group_set_editing(group, false);
                return;
            }
            lv_group_set_editing(group, false);
            lv_obj_t *focused = lv_group_get_focused(group);
            if (focused != NULL) {
                lv_obj_clear_state(focused, LV_STATE_FOCUS_KEY);
            }
            app_input_set_group(&controller->app->ui.input, controller->nav_group);
            lv_obj_t *tab_bar = lv_group_get_focused(controller->nav_group);
            if (tab_bar != NULL) {
                lv_obj_add_state(tab_bar, LV_STATE_FOCUS_KEY);
            }
            lv_event_stop_bubbling(e);
            break;
        }
        case LV_KEY_ENTER: {
            if (lv_obj_has_class(target, &lv_dropdown_class)) {
                if (!lv_dropdown_is_open(target)) {
                    lv_dropdown_open(target);
                    controller->active_dropdown = target;
                    settings_dropdown_set_list_editing(controller, true);
                }
                return;
            }
            if (lv_obj_check_type(target, &lv_textarea_class)) {
                lv_group_set_editing(group, true);
                return;
            }
            break;
        }
        case LV_KEY_DOWN: {
            if (settings_dropdown_list_open(controller, target)) { return; }
            if (lv_obj_get_parent(target) == controller->tabview) { return; }
            lv_group_focus_next(group);
            break;
        }
        case LV_KEY_UP: {
            if (settings_dropdown_list_open(controller, target)) { return; }
            if (lv_obj_get_parent(target) == controller->tabview) { return; }
            lv_group_focus_prev(group);
            break;
        }
        case LV_KEY_LEFT: {
            if (detail_item_needs_lrkey(target)) { return; }
            break;
        }
        case LV_KEY_RIGHT: {
            if (detail_item_needs_lrkey(target)) { return; }
            if (settings_dropdown_list_open(controller, target)) { return; }
            if (lv_obj_has_class(target, &lv_dropdown_class)) {
                lv_group_focus_next(group);
            }
            break;
        }
    }
}

static bool detail_item_needs_lrkey(lv_obj_t *obj) {
    if (lv_obj_has_class(obj, &lv_slider_class)) {
        return true;
    }
    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        return true;
    }
    return false;
}

static void detail_defocus(settings_controller_t *controller, lv_event_t *e) {
    (void) e;
    lv_obj_t *detail_focused = lv_group_get_focused(controller->detail_group);
    if (detail_focused) {
        lv_event_send(detail_focused, LV_EVENT_DEFOCUSED, lv_indev_get_act());
    }
    app_input_set_group(&controller->app->ui.input, controller->nav_group);
    lv_obj_t *nav_focused = lv_group_get_focused(controller->nav_group);
    if (nav_focused) {
        lv_obj_add_state(nav_focused, LV_STATE_FOCUS_KEY);
    }
}

static void on_dropdown_clicked(lv_event_t *event) {
    settings_controller_t *controller = event->user_data;
    /* This callback belongs to the dropdown.  A child/list event can bubble to
     * it, so current_target is the object whose lifetime we track. */
    lv_obj_t *target = lv_event_get_current_target(event);
    if (lv_obj_has_state(target, LV_STATE_CHECKED)) {
        controller->active_dropdown = target;
        settings_dropdown_set_list_editing(controller, true);
    } else {
        controller->active_dropdown = NULL;
        settings_dropdown_set_list_editing(controller, false);
    }
}

static void settings_save_error_cb(lv_event_t *e) {
    settings_controller_t *fragment = lv_event_get_user_data(e);
    lv_obj_t *msgbox = lv_event_get_current_target(e);
    const bool discard = lv_msgbox_get_active_btn(msgbox) == 1;
    lv_msgbox_close_async(msgbox);
    if (!discard) {
        return;
    }

    /* Suppress on_destroy_view's best-effort save.  In particular, when the
     * input file was unreadable or malformed, closing must never overwrite the
     * recoverable original with the in-memory defaults/partial parse. */
    fragment->save_complete = true;
    settings_close_pane_popup(fragment);
    settings_launcher_detach(fragment);
    lv_fragment_del((lv_fragment_t *) fragment);
}

static bool settings_finish_close(settings_controller_t *fragment) {
    if (!settings_save(app_configuration)) {
        static const char *btn_txts[] = {
                translatable("Keep editing"), translatable("Discard and close"), ""
        };
        lv_obj_t *msgbox = lv_msgbox_create_i18n(
                NULL, locstr("Can't save settings"),
                locstr("Eclipse could not write the settings file. Your previous settings were kept. "
                       "Check that the settings directory is writable, then try again, or discard "
                       "these changes and close Settings."),
                btn_txts, false);
        lv_obj_add_event_cb(msgbox, settings_save_error_cb, LV_EVENT_VALUE_CHANGED, fragment);
        lv_obj_center(msgbox);
        return false;
    }
    fragment->save_complete = true;
    settings_close_pane_popup(fragment);
    settings_launcher_detach(fragment);
    lv_fragment_del((lv_fragment_t *) fragment);
    return true;
}

static bool settings_try_close(settings_controller_t *fragment) {
    bool locale_restart = false;
#if FEATURE_I18N_LANGUAGE_SETTINGS
    locale_restart = fragment->needs_locale_reapply && app_configuration->language != NULL &&
                     app_configuration->language[0] != '\0';
#endif
    if (fragment->needs_app_restart || locale_restart) {
        static const char *btn_txts[] = {translatable("Later"), translatable("Exit app"), ""};
        const char *message = fragment->needs_app_restart
                ? locstr("Some settings take effect only after restarting the app. "
                         "Exit now so you can reopen it?")
                : locstr("Language changes take effect after restarting the app. "
                         "Exit now so you can reopen it?");
        lv_obj_t *msgbox = lv_msgbox_create_i18n(NULL, NULL,
                                                 message, btn_txts, false);
        lv_obj_center(msgbox);
        lv_obj_add_event_cb(msgbox, app_restart_confirm_cb, LV_EVENT_VALUE_CHANGED, fragment);
        return true;
    }
    if (fragment->needs_stream_reconnect && fragment->app->session != NULL && session_is_streaming(fragment->app->session)) {
        static const char *btn_txts[] = {translatable("Later"), translatable("Disconnect stream"), ""};
        lv_obj_t *msgbox =
                lv_msgbox_create_i18n(NULL, NULL,
                                      locstr("Settings apply on the next streaming session. Disconnect now, then "
                                             "start the stream again to use them?"),
                                      btn_txts, false);
        lv_obj_center(msgbox);
        lv_obj_add_event_cb(msgbox, stream_reconnect_confirm_cb, LV_EVENT_VALUE_CHANGED, fragment);
        return true;
    }
    return settings_finish_close(fragment);
}

static void settings_close(lv_event_t *e) {
    (void) settings_try_close(lv_event_get_user_data(e));
}

static void stream_reconnect_confirm_cb(lv_event_t *e) {
    settings_controller_t *fragment = lv_event_get_user_data(e);
    app_t *app = fragment->app;
    lv_obj_t *msgbox = lv_event_get_current_target(e);
    const bool disconnect = lv_msgbox_get_active_btn(msgbox) == 1;
    lv_msgbox_close_async(msgbox);
    if (settings_finish_close(fragment) && disconnect && app->session != NULL) {
        session_interrupt(app->session, false, STREAMING_INTERRUPT_USER);
    }
}

static void app_restart_confirm_cb(lv_event_t *e) {
    settings_controller_t *fragment = lv_event_get_user_data(e);
    lv_obj_t *msgbox = lv_event_get_current_target(e);
    const bool exit_app = lv_msgbox_get_active_btn(msgbox) == 1;
    lv_msgbox_close_async(msgbox);
    if (settings_finish_close(fragment) && exit_app) {
        app_request_exit();
    }
}

static void embed_cancel_cb(lv_event_t *e) {
    settings_controller_t *c = lv_event_get_user_data(e);
    if (c->pane_mbox != NULL) {
        settings_request_close_pane_popup(c);
        return;
    }
    (void) settings_try_close(c);
}

static void pane_child_attach_handlers(settings_controller_t *controller, lv_obj_t *child, bool popup) {
    if (!child || !lv_obj_is_group_def(child) ||
        lv_obj_has_flag(child, SETTINGS_HANDLERS_ATTACHED_FLAG)) {
        return;
    }
    lv_obj_add_flag(child, SETTINGS_HANDLERS_ATTACHED_FLAG);
    lv_obj_add_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);
    /* THE PANE MUST FOLLOW THE FOCUS. Only pref_desc_label ever asked to be scrolled
     * into view, so focus walking down a pane went off the bottom of the visible area
     * and simply vanished -- which reads exactly like navigation being stuck, because
     * nothing on screen is selected any more and the next press acts somewhere
     * invisible. Every focusable thing in a pane gets it, not just the ones that
     * remembered to. */
    lv_obj_add_flag(child, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(child, on_detail_key, LV_EVENT_KEY, controller);
    if (lv_obj_has_class(child, &lv_dropdown_class)) {
        lv_obj_add_event_cb(child, on_dropdown_clicked, LV_EVENT_CLICKED, controller);
        lv_obj_add_event_cb(child, on_dropdown_clicked, LV_EVENT_VALUE_CHANGED, controller);
        lv_obj_add_event_cb(child, settings_dropdown_deleted_cb, LV_EVENT_DELETE, controller);
        lv_obj_add_event_cb(child, settings_dropdown_arrow_preprocess_cb, LV_EVENT_KEY | LV_EVENT_PREPROCESS,
                            controller);
        lv_obj_add_event_cb(child, settings_dropdown_release_preprocess_cb,
                            LV_EVENT_RELEASED | LV_EVENT_PREPROCESS, controller);
        if (popup) {
            lv_obj_add_event_cb(child, settings_dropdown_cancel_cb, LV_EVENT_CANCEL, controller);
            lv_obj_add_event_cb(child, settings_dropdown_esc_preprocess_cb, LV_EVENT_KEY | LV_EVENT_PREPROCESS,
                                controller);
        }
    }
    if (lv_obj_check_type(child, &lv_textarea_class)) {
        lv_obj_add_event_cb(child, on_textarea_focused, LV_EVENT_FOCUSED, controller);
        lv_obj_add_event_cb(child, on_textarea_defocused, LV_EVENT_DEFOCUSED, controller);
    }
}

static void pane_child_added(lv_event_t *e) {
    pane_child_attach_handlers(lv_event_get_user_data(e), lv_event_get_param(e), false);
}

static void pane_popup_child_added(lv_event_t *e) {
    settings_controller_t *c = lv_event_get_user_data(e);
    lv_obj_t *child = lv_event_get_param(e);
    if (child == NULL) {
        return;
    }
    pane_child_attach_handlers(c, child, true);
    if (lv_obj_is_group_def(child) && !lv_obj_has_flag(child, SETTINGS_NAV_EXCLUDE_FLAG)) {
        lv_group_add_obj(c->pane_popup_group, child);
    }
    /* Also cover a subtree which was populated before being reparented here. */
    embed_popup_attach_key_handlers(child, c);
    embed_popup_add_objs_recursive(child, c->pane_popup_group);
}

static void on_textarea_focused(lv_event_t *e) {
    settings_controller_t *controller = lv_event_get_user_data(e);
    lv_group_t *group = controller->pane_popup_group ? controller->pane_popup_group : controller->detail_group;
    if (group) {
        lv_group_set_editing(group, false);
    }
}

static void on_textarea_defocused(lv_event_t *e) {
    settings_controller_t *controller = lv_event_get_user_data(e);
    lv_group_t *group = controller->pane_popup_group ? controller->pane_popup_group : controller->detail_group;
    if (group) {
        lv_group_set_editing(group, false);
    }
}

static lv_group_t *settings_nav_group_for(settings_controller_t *c) {
    if (c->pane_popup_group) {
        return c->pane_popup_group;
    }
    if (c->mini && c->tabview) {
        uint16_t act = lv_tabview_get_tab_act(c->tabview);
        return c->tab_groups[act];
    }
    return c->detail_group;
}

static void settings_dropdown_set_list_editing(settings_controller_t *c, bool editing) {
    lv_group_t *group = settings_nav_group_for(c);
    if (group != NULL) {
        lv_group_set_editing(group, editing);
    }
}

static lv_obj_t *settings_active_dropdown_get(settings_controller_t *c) {
    lv_obj_t *dropdown = c->active_dropdown;
    if (dropdown != NULL && !lv_obj_is_valid(dropdown)) {
        c->active_dropdown = NULL;
        settings_dropdown_set_list_editing(c, false);
        return NULL;
    }
    return dropdown;
}

static void settings_clear_active_dropdown(settings_controller_t *c, bool close) {
    lv_obj_t *dropdown = settings_active_dropdown_get(c);
    c->active_dropdown = NULL;
    settings_dropdown_set_list_editing(c, false);
    if (close && dropdown != NULL && lv_dropdown_is_open(dropdown)) {
        lv_dropdown_close(dropdown);
    }
}

static void settings_dropdown_deleted_cb(lv_event_t *e) {
    settings_controller_t *c = lv_event_get_user_data(e);
    if (c->active_dropdown == lv_event_get_current_target(e)) {
        c->active_dropdown = NULL;
        settings_dropdown_set_list_editing(c, false);
    }
}

static bool settings_dropdown_list_open(settings_controller_t *c, lv_obj_t *target) {
    lv_obj_t *active = settings_active_dropdown_get(c);
    if (active != NULL && lv_dropdown_is_open(active)) {
        return true;
    }
    return target != NULL && lv_obj_has_class(target, &lv_dropdown_class) && lv_dropdown_is_open(target);
}

static bool settings_close_dropdown_on_back(settings_controller_t *c, lv_obj_t *target) {
    lv_obj_t *dropdown = NULL;
    lv_obj_t *active = settings_active_dropdown_get(c);
    if (active != NULL) {
        if (lv_dropdown_is_open(active)) {
            dropdown = active;
        } else if (target == active) {
            dropdown = active;
        }
    } else if (target != NULL && lv_obj_has_class(target, &lv_dropdown_class) && lv_dropdown_is_open(target)) {
        dropdown = target;
    }
    if (dropdown == NULL) {
        return false;
    }
    c->active_dropdown = NULL;
    c->suppress_pane_back = true;
    settings_dropdown_set_list_editing(c, false);
    if (lv_dropdown_is_open(dropdown)) {
        lv_dropdown_close(dropdown);
    }
    lv_group_focus_obj(dropdown);
    return true;
}

/**
 * THE KEY THAT OPENED IT MUST NOT CLOSE IT.
 *
 * lv_dropdown's release handler TOGGLES (lv_dropdown.c: btn_release_handler -- open when
 * closed, close when open), and this screen opens the list itself when OK is pressed. So
 * OK opened the list and the release of that same press shut it: the list appeared to
 * flash and vanish, with nothing in the logs to say so. Exactly one release is swallowed,
 * and only for the dropdown we opened.
 *
 * PREPROCESS because the class handler runs BEFORE ordinary user callbacks
 * (lv_event.c: event_send_core), and only a preprocess handler can stop it.
 */
/**
 * OK ON A CLOSED DROPDOWN OPENED IT AND THE SAME PRESS SHUT IT AGAIN.
 *
 * lv_dropdown's release handler TOGGLES (lv_dropdown.c: btn_release_handler -- open when
 * closed, close when open) and the LIST IS ALREADY OPEN by the time the release arrives:
 * measured on the TV, `release preprocess: open=1` on the very first OK. So the press
 * opened it and the release of that same press closed it, and the list appeared to flash
 * and vanish with nothing in any log to say so.
 *
 * ONLY the release of the press that OPENED it is swallowed -- recorded below on the key
 * press, from whether the list was closed at that moment. A press while the list is open
 * is the user choosing an option, and its release must close and commit as usual.
 *
 * PREPROCESS because the widget's own class handler runs BEFORE ordinary user callbacks
 * (lv_event.c: event_send_core), and only a preprocess handler can stop it.
 */
static void settings_dropdown_release_preprocess_cb(lv_event_t *e) {
    settings_controller_t *c = lv_event_get_user_data(e);
    if (!c->suppress_dropdown_release) {
        return;
    }
    c->suppress_dropdown_release = false;
    if (lv_event_get_current_target(e) == c->active_dropdown) {
        lv_event_stop_processing(e);
    }
}

static void settings_dropdown_esc_preprocess_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY || lv_event_get_key(e) != LV_KEY_ESC) {
        return;
    }
    settings_controller_t *c = lv_event_get_user_data(e);
    if (settings_close_dropdown_on_back(c, lv_event_get_current_target(e))) {
        lv_event_stop_processing(e);
    }
}

static void settings_dropdown_arrow_preprocess_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) {
        return;
    }
    settings_controller_t *c = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_current_target(e);
    if (!lv_obj_has_class(target, &lv_dropdown_class)) {
        return;
    }
    /* RECORD WHETHER THIS OK IS THE ONE THAT OPENS IT, before anything acts on the key
     * -- see settings_dropdown_release_preprocess_cb. Closed now means the release that
     * follows would toggle the freshly-opened list straight back shut. */
    if (lv_event_get_key(e) == LV_KEY_ENTER) {
        c->suppress_dropdown_release = !lv_dropdown_is_open(target);
        c->active_dropdown = target;
    }
    if (settings_dropdown_list_open(c, target)) {
        return;
    }
    const uint32_t key = lv_event_get_key(e);
    lv_group_t *nav_detail = c->pane_popup_group ? c->pane_popup_group : c->detail_group;
    if (!nav_detail) {
        return;
    }

    if (c->pane_popup_group != NULL) {
        switch (key) {
            case LV_KEY_UP:
            case LV_KEY_LEFT:
                lv_group_focus_prev(nav_detail);
                lv_event_stop_processing(e);
                return;
            case LV_KEY_DOWN:
            case LV_KEY_RIGHT:
                lv_group_focus_next(nav_detail);
                lv_event_stop_processing(e);
                return;
            default:
                return;
        }
    }

    if (c->mini) {
        uint16_t act = lv_tabview_get_tab_act(c->tabview);
        lv_group_t *group = c->tab_groups[act];
        switch (key) {
            case LV_KEY_UP:
                lv_group_focus_prev(group);
                lv_event_stop_processing(e);
                return;
            case LV_KEY_DOWN:
                lv_group_focus_next(group);
                lv_event_stop_processing(e);
                return;
            case LV_KEY_RIGHT:
                lv_group_focus_next(group);
                lv_event_stop_processing(e);
                return;
            default:
                return;
        }
    }

    switch (key) {
        case LV_KEY_UP:
            lv_group_focus_prev(nav_detail);
            lv_event_stop_processing(e);
            break;
        case LV_KEY_DOWN:
            lv_group_focus_next(nav_detail);
            lv_event_stop_processing(e);
            break;
        case LV_KEY_LEFT:
            detail_defocus(c, e);
            lv_event_stop_processing(e);
            break;
        case LV_KEY_RIGHT:
            lv_group_focus_next(nav_detail);
            lv_event_stop_processing(e);
            break;
        default:
            break;
    }
}

static void settings_dropdown_cancel_cb(lv_event_t *e) {
    settings_controller_t *c = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_current_target(e);
    if (settings_close_dropdown_on_back(c, target)) {
        lv_event_stop_bubbling(e);
        return;
    }
    c->active_dropdown = NULL;
    lv_event_stop_bubbling(e);
}

static void embed_popup_cancel_cb(lv_event_t *e) {
    settings_controller_t *c = lv_event_get_user_data(e);
    if (c->pane_mbox == NULL || lv_event_get_current_target(e) != c->pane_mbox) {
        return;
    }
    lv_group_t *group = c->pane_popup_group;
    lv_obj_t *focused = group ? lv_group_get_focused(group) : NULL;
    if (lv_obj_check_type(focused, &lv_textarea_class) && group && lv_group_get_editing(group)) {
        lv_group_set_editing(group, false);
        return;
    }
    if (settings_close_dropdown_on_back(c, focused)) {
        return;
    }
    settings_request_close_pane_popup(c);
}

/* ------------------------------------------------------------------------- */
/* Launcher-embedded settings: second AppBar + pane popups (below main top bar) */
/* ------------------------------------------------------------------------- */

static void settings_embed_refocus_appbar(settings_controller_t *c) {
    if (!c->embed_appbar || !c->nav_group) {
        return;
    }
    if (c->embed_origin_btn != NULL && lv_obj_is_valid(c->embed_origin_btn)) {
        lv_group_focus_obj(c->embed_origin_btn);
        if (app_ui_get_input_mode(&c->app->ui.input) & UI_INPUT_MODE_BUTTON_FLAG) {
            lv_obj_add_state(c->embed_origin_btn, LV_STATE_FOCUS_KEY);
        }
        return;
    }
    uint32_t n = lv_obj_get_child_cnt(c->embed_appbar);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(c->embed_appbar, i);
        if (lv_obj_check_type(ch, &lv_btn_class)) {
            lv_group_focus_obj(ch);
            if (app_ui_get_input_mode(&c->app->ui.input) & UI_INPUT_MODE_BUTTON_FLAG) {
                lv_obj_add_state(ch, LV_STATE_FOCUS_KEY);
            }
            break;
        }
    }
}

static void settings_launcher_detach(settings_controller_t *fragment) {
    if (fragment->launcher_host) {
        fragment->launcher_host->settings_fragment = NULL;
        lv_obj_add_flag(fragment->launcher_host->settings_layer, LV_OBJ_FLAG_HIDDEN);
    }
}

static void settings_pane_fragment_destroy(settings_controller_t *c) {
    if (c->pane_fragment == NULL) {
        return;
    }
    lv_fragment_t *pane = c->pane_fragment;
    c->pane_fragment = NULL;
    /* Widget tree is owned by the msgbox; do not lv_obj_del() again from the fragment. */
    pane->obj = NULL;
    const lv_fragment_class_t *cls = pane->cls;
    if (cls != NULL && cls->destructor_cb != NULL) {
        cls->destructor_cb(pane);
    }
    if (pane->child_manager != NULL) {
        lv_fragment_manager_del(pane->child_manager);
    }
    lv_mem_free(pane);
}

static void embed_pane_mbox_delete_cb(lv_event_t *e) {
    settings_controller_t *c = lv_event_get_user_data(e);
    if (c->pane_popup_group != NULL) {
        app_input_remove_modal_group(&c->app->ui.input, c->pane_popup_group);
        lv_group_del(c->pane_popup_group);
        c->pane_popup_group = NULL;
    }
    settings_pane_fragment_destroy(c);
    c->pane_mbox = NULL;
    c->active_dropdown = NULL;
    if (c->launcher_host) {
        settings_embed_refocus_appbar(c);
    }
}

static void settings_close_pane_popup_async_cb(void *user_data) {
    settings_controller_t *c = user_data;
    if (!c->pane_mbox) {
        return;
    }
    if (c->suppress_pane_back) {
        c->suppress_pane_back = false;
        return;
    }
    lv_obj_t *mbox = c->pane_mbox;
    c->pane_mbox = NULL;
    c->active_dropdown = NULL;
    lv_msgbox_close(mbox);
}

static void settings_request_close_pane_popup(settings_controller_t *c) {
    if (!c->pane_mbox) {
        return;
    }
    lv_async_call_cancel(settings_close_pane_popup_async_cb, c);
    lv_async_call(settings_close_pane_popup_async_cb, c);
}

static void settings_close_pane_popup(settings_controller_t *c) {
    if (!c->pane_mbox) {
        return;
    }
    lv_async_call_cancel(settings_close_pane_popup_async_cb, c);
    lv_obj_t *mbox = c->pane_mbox;
    c->pane_mbox = NULL;
    c->active_dropdown = NULL;
    lv_msgbox_close(mbox);
}

static void embed_popup_add_objs_recursive(lv_obj_t *parent, lv_group_t *g) {
    uint32_t n = lv_obj_get_child_cnt(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(parent, i);
        if (lv_obj_is_group_def(ch) && !lv_obj_has_flag(ch, SETTINGS_NAV_EXCLUDE_FLAG)) {
            lv_group_add_obj(g, ch);
        }
        embed_popup_add_objs_recursive(ch, g);
    }
}

static lv_obj_t *embed_popup_first_focusable(lv_obj_t *parent) {
    uint32_t n = lv_obj_get_child_cnt(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(parent, i);
        if (lv_obj_is_group_def(ch) && !lv_obj_has_flag(ch, SETTINGS_NAV_EXCLUDE_FLAG)) {
            return ch;
        }
        lv_obj_t *inner = embed_popup_first_focusable(ch);
        if (inner) {
            return inner;
        }
    }
    return NULL;
}

static void embed_popup_attach_key_handlers(lv_obj_t *parent, settings_controller_t *c) {
    uint32_t n = lv_obj_get_child_cnt(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(parent, i);
        embed_popup_attach_key_handlers(ch, c);
        pane_child_attach_handlers(c, ch, true);
    }
}

static void embed_style_msgbox_close_red(lv_obj_t *mbox) {
    lv_obj_t *xb = lv_msgbox_get_close_btn(mbox);
    if (xb == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(xb, ml_color_hex(ML_COLOR_SURFACE_ALT), 0);
    lv_obj_set_style_bg_opa(xb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(xb, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *lb = lv_btn_find_label(xb);
    if (lb == NULL) {
        return;
    }
    lv_label_set_text_static(lb, MAT_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(lb, lv_theme_moonlight_get_iconfont_normal(mbox), 0);
    lv_obj_set_style_text_color(lb, ml_color_hex(ML_COLOR_TEXT), 0);
}

static void settings_style_pane_msgbox_amoled(lv_obj_t *mbox) {
    lv_obj_t *backdrop = lv_obj_get_parent(mbox);
    if (backdrop != NULL) {
        lv_obj_set_style_bg_color(backdrop, ml_color_hex(ML_COLOR_BG), 0);
        lv_obj_set_style_bg_opa(backdrop, LV_OPA_70, 0);
    }
    lv_obj_set_style_bg_color(mbox, ml_color_hex(ML_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(mbox, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(mbox, ml_color_hex(ML_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(mbox, LV_DPX(1), 0);
    lv_obj_set_style_radius(mbox, LV_DPX(12), 0);
    lv_obj_set_style_shadow_width(mbox, LV_DPX(20), 0);
    lv_obj_set_style_shadow_color(mbox, ml_color_hex(ML_COLOR_BG), 0);
    lv_obj_set_style_shadow_opa(mbox, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(mbox, lv_dpx(12), 0);

    lv_obj_t *title = lv_msgbox_get_title(mbox);
    if (title != NULL) {
        lv_obj_set_style_text_color(title, ml_color_hex(ML_COLOR_TEXT), 0);
    }
    lv_obj_t *content = lv_msgbox_get_content(mbox);
    if (content != NULL) {
        lv_obj_set_style_bg_color(content, ml_color_hex(ML_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(content, 0, 0);
    }
}

static void settings_show_pane_popup(settings_controller_t *c, const lv_fragment_class_t *cls) {
    settings_close_pane_popup(c);

    const char *title = locstr("Settings");
    for (int i = 0; i < entries_len; i++) {
        if (entries[i].cls == cls) {
            title = locstr(entries[i].name);
            break;
        }
    }

    lv_obj_t *mbox = lv_msgbox_create(NULL, title, NULL, NULL, true);
    lv_obj_add_flag(mbox, LV_OBJ_FLAG_USER_4);
    settings_style_pane_msgbox_amoled(mbox);
    embed_style_msgbox_close_red(mbox);
    lv_disp_t *disp = lv_obj_get_disp(mbox);
    const lv_coord_t hor = lv_disp_get_hor_res(disp);
    const lv_coord_t ver = lv_disp_get_ver_res(disp);
    lv_obj_set_size(mbox, hor * 92 / 100, ver * 92 / 100);

    lv_obj_t *content = lv_msgbox_get_content(mbox);
    lv_obj_add_flag(content, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_width(content, LV_PCT(100));
    /* The pane's own height is its cap. A content cap of 90% of the dialog, as there was
     * here, sat BELOW the title row and so ran past the dialog's bottom edge: the last
     * lines of every long pane were cut off instead of scrolled to. The theme now fits the
     * content between the title and the bottom edge (ui_scroll_fit_msgbox). */
    lv_obj_set_style_max_height(mbox, ver * 92 / 100, 0);
    lv_obj_set_style_pad_all(content, lv_dpx(12), 0);

    c->pane_mbox = mbox;
    c->pane_popup_group = lv_group_create();
    lv_group_set_wrap(c->pane_popup_group, true);

    lv_fragment_t *pane = lv_fragment_create(cls, c);
    lv_fragment_create_obj(pane, content);
    c->pane_fragment = pane;

    embed_popup_attach_key_handlers(content, c);
    embed_popup_add_objs_recursive(content, c->pane_popup_group);
    /* Initial controls were attached by the traversal above.  Listen only
     * after construction so dynamically-created controls are attached once,
     * rather than registering duplicate key/value/delete callbacks. */
    lv_obj_add_event_cb(content, pane_popup_child_added, LV_EVENT_CHILD_CREATED, c);

    lv_obj_t *close_btn = lv_msgbox_get_close_btn(mbox);
    if (close_btn != NULL) {
        lv_group_add_obj(c->pane_popup_group, close_btn);
        lv_obj_add_flag(close_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(close_btn, on_detail_key, LV_EVENT_KEY, c);
    }

    lv_obj_add_event_cb(mbox, embed_popup_cancel_cb, LV_EVENT_CANCEL, c);

    app_input_push_modal_group(&c->app->ui.input, c->pane_popup_group);
    lv_obj_t *first = embed_popup_first_focusable(content);
    if (first) {
        lv_group_focus_obj(first);
        if (app_ui_get_input_mode(&c->app->ui.input) & UI_INPUT_MODE_BUTTON_FLAG) {
            lv_obj_add_state(first, LV_STATE_FOCUS_KEY);
        }
    }

    lv_obj_add_event_cb(mbox, embed_pane_mbox_delete_cb, LV_EVENT_DELETE, c);
    lv_obj_center(mbox);
}

static void embed_entry_btn_cb(lv_event_t *e) {
    settings_controller_t *controller = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    const lv_fragment_class_t *cls = lv_obj_get_user_data(btn);
    if (cls) {
        controller->embed_origin_btn = btn;
        settings_show_pane_popup(controller, cls);
    }
}

static void embed_fechar_btn_cb(lv_event_t *e) {
    settings_close(e);
}

static void embed_backdrop_cb(lv_event_t *e) {
    settings_controller_t *c = lv_event_get_user_data(e);
    settings_request_close_pane_popup(c);
}

static void embed_appbar_key(lv_event_t *e) {
    settings_controller_t *controller = lv_event_get_user_data(e);
    switch (lv_event_get_key(e)) {
        case LV_KEY_LEFT:
        case LV_KEY_UP:
            lv_group_focus_prev(controller->nav_group);
            break;
        case LV_KEY_RIGHT:
        case LV_KEY_DOWN:
            lv_group_focus_next(controller->nav_group);
            break;
        default:
            break;
    }
}

static void on_launcher_embedded_view_created(settings_controller_t *controller) {
    controller->nav_group = lv_group_create();
    lv_group_set_wrap(controller->nav_group, true);
    lv_group_set_editing(controller->nav_group, false);
    lv_obj_add_event_cb(controller->embed_appbar, embed_appbar_key, LV_EVENT_KEY, controller);

    uint32_t n = lv_obj_get_child_cnt(controller->embed_appbar);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(controller->embed_appbar, i);
        if (lv_obj_check_type(ch, &lv_btn_class)) {
            lv_group_add_obj(controller->nav_group, ch);
        }
    }

    app_input_set_group(&controller->app->ui.input, controller->nav_group);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(controller->embed_appbar, i);
        if (lv_obj_check_type(ch, &lv_btn_class)) {
            lv_group_focus_obj(ch);
            lv_obj_add_state(ch, LV_STATE_FOCUS_KEY);
            break;
        }
    }
}

lv_obj_t *settings_launcher_embedded_create(lv_fragment_t *self, lv_obj_t *parent) {
    settings_controller_t *c = (settings_controller_t *) self;

    lv_obj_t *root = lv_obj_create(parent);
    c->embed_root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_color(root, ml_color_hex(ML_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_70, 0);

    lv_obj_t *bar = lv_obj_create(root);
    c->embed_appbar = bar;
    c->nav = bar;
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, LV_DPX(44));
    lv_obj_set_layout(bar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, LV_DPX(8), 0);
    lv_obj_set_style_pad_gap(bar, LV_DPX(4), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, ml_color_hex(ML_COLOR_BG), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, LV_DPX(1), 0);
    lv_obj_set_style_border_color(bar, ml_color_hex(ML_COLOR_BORDER), 0);
    lv_obj_set_scroll_dir(bar, LV_DIR_HOR);
    lv_obj_set_style_pad_ver(bar, LV_DPX(4), 0);

    for (int i = 0; i < entries_len; i++) {
        lv_obj_t *btn = lv_btn_create(bar);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_size(btn, LV_DPX(36), LV_DPX(36));
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_t *lab = lv_label_create(btn);
        lv_label_set_text_static(lab, entries[i].icon);
        lv_obj_set_style_text_font(lab, lv_theme_moonlight_get_iconfont_small(bar), 0);
        lv_obj_set_style_text_color(lab, ml_color_hex(ML_COLOR_TEXT), 0);
        lv_obj_center(lab);
        lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(btn, (void *) entries[i].cls);
        lv_obj_add_event_cb(btn, embed_entry_btn_cb, LV_EVENT_CLICKED, c);
        lv_obj_add_event_cb(btn, embed_cancel_cb, LV_EVENT_CANCEL, c);
    }

    lv_obj_t *sp = lv_obj_create(bar);
    lv_obj_remove_style_all(sp);
    lv_obj_set_height(sp, LV_DPX(4));
    lv_obj_set_flex_grow(sp, 1);

    lv_obj_t *close_btn = lv_btn_create(bar);
    c->close_btn = close_btn;
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(close_btn, LV_DPX(36), LV_DPX(36));
    lv_obj_set_style_radius(close_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(close_btn, 0, 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_t *clab = lv_label_create(close_btn);
    lv_label_set_text_static(clab, MAT_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(clab, lv_theme_moonlight_get_iconfont_small(bar), 0);
    lv_obj_set_style_text_color(clab, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_center(clab);
    lv_obj_clear_flag(clab, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(close_btn, embed_fechar_btn_cb, LV_EVENT_CLICKED, c);
    lv_obj_add_event_cb(close_btn, embed_cancel_cb, LV_EVENT_CANCEL, c);

    lv_obj_t *backdrop = lv_obj_create(root);
    lv_obj_remove_style_all(backdrop);
    lv_obj_set_width(backdrop, LV_PCT(100));
    lv_obj_set_flex_grow(backdrop, 1);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(backdrop, embed_backdrop_cb, LV_EVENT_CLICKED, c);

    return root;
}

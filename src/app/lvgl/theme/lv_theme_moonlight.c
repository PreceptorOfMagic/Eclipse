#include "app.h"

#include "lv_theme_moonlight.h"
#include "lv_theme_moonlight_colors.h"

#include "util/font.h"
#include "lvgl/ext/lv_child_group.h"
#include "ui/ui_scroll.h"

static lv_style_t knob_shadow;
static lv_style_t hover_hint;
static lv_style_t scrollbar;
static lv_style_t scrollbar_scrolled;

static void apply_cb(lv_theme_t *, lv_obj_t *);

static void lv_start_text_input(lv_event_t *event);

static void lv_stop_text_input(lv_event_t *event);

static void msgbox_key(lv_event_t *event);

static void msgbox_cancel(lv_event_t *event);

static void msgbox_destroy(lv_event_t *event);

static void msgbox_fit(lv_event_t *event);

static void msgbox_backdrop_resized(lv_event_t *event);

void lv_theme_moonlight_init(lv_theme_t *theme, const app_fonts_t *fonts, app_t *app) {
    lv_theme_set_apply_cb(theme, apply_cb);
    theme->user_data = app;
    theme->font_small = fonts->fonts.small;
    theme->font_normal = fonts->fonts.normal;
    theme->font_large = fonts->fonts.large;
    lv_style_init(&knob_shadow);
    lv_style_set_shadow_color(&knob_shadow, lv_color_black());
    lv_style_set_shadow_width(&knob_shadow, LV_DPX(5));
    lv_style_set_shadow_opa(&knob_shadow, LV_OPA_50);
    /* HOVER. LVGL v8 declares LV_STATE_HOVERED but never sets it -- ui_hover.c does
     * that -- and without a style attached the state would be invisible. Attached to
     * EVERY object in apply_cb so the highlight is uniform app-wide instead of each
     * screen reinventing it; it can only show on something the hover tracker marked,
     * and it only marks clickable objects. Deliberately an outline rather than a
     * background: it reads on top of any widget's own colours, and it stays
     * distinguishable from the focus ring, which is thicker. */
    lv_style_init(&hover_hint);
    lv_style_set_outline_width(&hover_hint, LV_DPX(2));
    lv_style_set_outline_color(&hover_hint, lv_palette_main(LV_PALETTE_BLUE));
    lv_style_set_outline_opa(&hover_hint, LV_OPA_70);
    lv_style_set_outline_pad(&hover_hint, LV_DPX(1));
    /* SCROLLBARS. LVGL's default theme styles the bar only on plain objects and lists, and a
     * msgbox's content is a class of its own -- so every dialog scrolled with a bar of no
     * width and no colour, and nothing said there was more below. Lists get the same look
     * so the two match. Kept narrow and inside the padding, where it does not paint over
     * the rows beside it. */
    lv_style_init(&scrollbar);
    lv_style_set_bg_color(&scrollbar, ml_color_hex(ML_COLOR_TEXT_MUTED));
    lv_style_set_bg_opa(&scrollbar, LV_OPA_40);
    lv_style_set_radius(&scrollbar, LV_RADIUS_CIRCLE);
    lv_style_set_width(&scrollbar, LV_DPX(4));
    lv_style_set_pad_right(&scrollbar, LV_DPX(2));
    lv_style_set_pad_bottom(&scrollbar, LV_DPX(2));
    lv_style_set_pad_ver(&scrollbar, LV_DPX(4));
    lv_style_init(&scrollbar_scrolled);
    lv_style_set_bg_opa(&scrollbar_scrolled, LV_OPA_COVER);
}

void lv_theme_moonlight_add_scrollbar(lv_obj_t *obj) {
    lv_obj_add_style(obj, &scrollbar, LV_PART_SCROLLBAR);
    lv_obj_add_style(obj, &scrollbar_scrolled, LV_PART_SCROLLBAR | LV_STATE_SCROLLED);
}

void lv_theme_moonlight_deinit(lv_theme_t *theme) {
    (void) theme;
    lv_style_reset(&knob_shadow);
    lv_style_reset(&hover_hint);
    lv_style_reset(&scrollbar);
    lv_style_reset(&scrollbar_scrolled);
}

const lv_font_t *lv_theme_moonlight_get_iconfont_large(lv_obj_t *obj) {
    lv_theme_t *th = lv_theme_get_from_obj(obj);
    return ((app_t *) th->user_data)->ui.fonts.icons.large;
}

const lv_font_t *lv_theme_moonlight_get_iconfont_normal(lv_obj_t *obj) {
    lv_theme_t *th = lv_theme_get_from_obj(obj);
    return ((app_t *) th->user_data)->ui.fonts.icons.normal;
}

const lv_font_t *lv_theme_moonlight_get_iconfont_small(lv_obj_t *obj) {
    lv_theme_t *th = lv_theme_get_from_obj(obj);
    return ((app_t *) th->user_data)->ui.fonts.icons.small;
}

static void apply_cb(lv_theme_t *theme, lv_obj_t *obj) {
    app_t *app = theme->user_data;
    bool set_font = true;
    lv_obj_add_style(obj, &hover_hint, LV_STATE_HOVERED);
    if (lv_obj_has_class(obj, &lv_btn_class)) {
        lv_obj_set_style_flex_cross_place(obj, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_t *parent = lv_obj_get_parent(obj);
        if (parent == NULL || !lv_obj_check_type(parent, &lv_msgbox_class)) {
            lv_obj_set_style_radius(obj, LV_DPX(10), 0);
            lv_obj_set_style_bg_color(obj, ml_color_hex(ML_COLOR_SURFACE_ALT), 0);
            lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(obj, ml_color_hex(ML_COLOR_SURFACE_HI), LV_STATE_PRESSED);
            /* FOCUS_KEY, NOT FOCUSED, and for the same reason as the outline below it:
             * clicking a button focuses it, and on LV_STATE_FOCUSED that button then
             * stayed lit while the cursor moved on -- a second thing looking selected.
             * FOCUS_KEY is only ever set for a keypad or encoder (lv_obj.c), which is
             * exactly when the ring is the selection. */
            lv_obj_set_style_bg_color(obj, ml_color_hex(ML_COLOR_SURFACE_HI), LV_STATE_FOCUS_KEY);
            lv_obj_set_style_outline_color(obj, ml_color_hex(ML_COLOR_PRIMARY), LV_STATE_FOCUS_KEY);
            lv_obj_set_style_outline_width(obj, LV_DPX(2), LV_STATE_FOCUS_KEY);
            lv_obj_set_style_outline_opa(obj, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
            lv_obj_set_style_outline_pad(obj, LV_DPX(2), LV_STATE_FOCUS_KEY);
            lv_obj_set_style_text_color(obj, ml_color_hex(ML_COLOR_TEXT), 0);
        }
        if (lv_obj_check_type(obj, &lv_list_btn_class)) {
            /* A LIST ROW IS FILLED, NOT OUTLINED.
             *
             * Rows are stacked with no room between them, and an outline is drawn
             * OUTSIDE the object -- so the ring came out with pieces missing and the
             * next row painted over what was left. Measured on the server picker: only
             * the top and bottom edges of the ring ever reached the screen. A filled
             * row cannot be clipped by anything, and it is what the settings nav has
             * always done, so the two lists finally agree. Local styles, so they beat
             * the outline the branch above just set and the hover outline the theme
             * attaches to every object. */
            lv_obj_set_style_outline_width(obj, 0, LV_STATE_FOCUS_KEY);
            lv_obj_set_style_outline_width(obj, 0, LV_STATE_HOVERED);
            lv_obj_set_style_bg_color(obj, ml_color_hex(ML_COLOR_PRIMARY), LV_STATE_FOCUS_KEY);
            lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
            lv_obj_set_style_bg_color(obj, ml_color_hex(ML_COLOR_SURFACE_HI), LV_STATE_HOVERED);
            lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_STATE_HOVERED);
        }
    }
    if (lv_obj_check_type(obj, &lv_obj_class) && lv_obj_get_parent(obj) != NULL) {
        lv_obj_t *parent = lv_obj_get_parent(obj);
        if (!lv_obj_check_type(parent, &lv_btn_class) && !lv_obj_check_type(parent, &lv_img_class)) {
            const lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, 0);
            if (bg_opa > LV_OPA_TRANSP && bg_opa < LV_OPA_COVER) {
                lv_obj_set_style_bg_color(obj, ml_color_hex(ML_COLOR_SURFACE), 0);
            }
        }
    }
    if (lv_obj_has_class(obj, &lv_label_class)) {
        lv_obj_t *parent = lv_obj_get_parent(obj);
        if (parent) {
            // Assume this is title
            if (lv_obj_check_type(parent, &lv_msgbox_class) && lv_msgbox_get_title(parent) == NULL) {
                lv_obj_set_style_text_font(obj, theme->font_large, 0);
                set_font = false;
            } else {
                lv_obj_t *parent2 = lv_obj_get_parent(parent);
                if (parent2) {
                    if (lv_obj_has_class(parent2, &lv_win_class) && lv_win_get_header(parent2) == parent) {
                        lv_obj_set_style_text_font(obj, theme->font_large, 0);
                        set_font = false;
                    } else if (lv_obj_check_type(parent2, &lv_msgbox_class) &&
                               lv_msgbox_get_close_btn(parent2) == parent) {
                        lv_obj_set_style_text_font(obj, lv_theme_moonlight_get_iconfont_large(obj), 0);
                        set_font = false;
                    }
                }
            }
        }
    }
    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        lv_obj_set_style_bg_color(obj, ml_color_hex(ML_COLOR_SURFACE_ALT), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(obj, ml_color_hex(ML_COLOR_TEXT), 0);
        lv_obj_set_style_border_color(obj, ml_color_hex(ML_COLOR_BORDER), 0);
        lv_obj_set_style_border_width(obj, LV_DPX(1), 0);
        lv_obj_add_event_cb(obj, lv_start_text_input, LV_EVENT_FOCUSED, theme);
        lv_obj_add_event_cb(obj, lv_stop_text_input, LV_EVENT_DEFOCUSED, theme);
    } else if (lv_obj_check_type(obj, &lv_msgbox_class)) {
        if (lv_obj_get_width(lv_scr_act()) / 10 * 4 > LV_DPI_DEF * 2) {
            lv_obj_set_width(obj, LV_PCT(40));
        } else {
            lv_obj_set_width(obj, LV_DPI_DEF * 2);
        }
        lv_obj_set_style_min_width(obj, LV_PCT(40), 0);
        lv_obj_set_style_max_width(obj, LV_PCT(60), 0);
        /* EVERY DIALOG FITS THE WINDOW. The cap alone clipped: the title, content and
         * button row simply ran past it, taking the buttons out of reach. msgbox_fit makes
         * the CONTENT the part that scrolls, so the title and buttons always stay on
         * screen; fonts never shrink to make room (the user's rule). A dialog that sets a
         * larger cap or a fixed height of its own keeps it. */
        lv_obj_set_style_max_height(obj, LV_PCT(90), 0);
        lv_obj_add_event_cb(obj, msgbox_fit, LV_EVENT_LAYOUT_CHANGED, NULL);
        lv_obj_t *backdrop = lv_obj_get_parent(obj);
        if (backdrop != NULL && lv_obj_check_type(backdrop, &lv_msgbox_backdrop_class)) {
            lv_obj_add_event_cb(backdrop, msgbox_backdrop_resized, LV_EVENT_SIZE_CHANGED, NULL);
        }
        lv_obj_set_style_flex_main_place(obj, LV_FLEX_ALIGN_END, 0);
        lv_group_t *group = lv_group_create();
        group->user_data = theme;
        app_input_push_modal_group(&app->ui.input, group);
        lv_obj_add_event_cb(obj, cb_child_group_add, LV_EVENT_CHILD_CREATED, group);
        lv_obj_add_event_cb(obj, msgbox_key, LV_EVENT_KEY, NULL);
        lv_obj_add_event_cb(obj, msgbox_cancel, LV_EVENT_CANCEL, NULL);
        lv_obj_add_event_cb(obj, msgbox_destroy, LV_EVENT_DELETE, group);
    } else if (lv_obj_check_type(obj, &lv_msgbox_content_class) || lv_obj_check_type(obj, &lv_list_class)) {
        lv_theme_moonlight_add_scrollbar(obj);
    } else if (lv_obj_check_type(obj, &lv_msgbox_backdrop_class)) {
        lv_obj_set_style_bg_color(obj, ml_color_hex(ML_COLOR_BG), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_70, 0);
    } else if (lv_obj_check_type(obj, &lv_btnmatrix_class)) {
        lv_obj_t *parent = lv_obj_get_parent(obj);
        if (lv_obj_check_type(parent, &lv_msgbox_class)) {
            lv_obj_set_style_text_font(obj, theme->font_small, 0);
            set_font = false;
        }
    } else if (lv_obj_check_type(obj, &lv_dropdown_class)) {
        lv_obj_set_style_text_font(obj, lv_theme_moonlight_get_iconfont_large(obj), LV_PART_INDICATOR);
        lv_dropdown_set_symbol(obj, MAT_SYMBOL_ARROW_DROP_DOWN);
        lv_obj_set_style_bg_color(obj, ml_color_hex(ML_COLOR_SURFACE_ALT), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(obj, ml_color_hex(ML_COLOR_TEXT), 0);
        lv_obj_set_style_border_color(obj, ml_color_hex(ML_COLOR_BORDER), 0);
        lv_obj_set_style_border_width(obj, LV_DPX(1), 0);
    } else if (lv_obj_check_type(obj, &lv_dropdownlist_class)) {
        lv_obj_set_style_bg_color(obj, ml_color_hex(ML_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(obj, ml_color_hex(ML_COLOR_BORDER), 0);
        lv_obj_set_style_border_width(obj, LV_DPX(1), 0);
        lv_obj_set_style_text_color(obj, ml_color_hex(ML_COLOR_TEXT), 0);
        lv_obj_set_style_bg_color(obj, ml_color_hex(ML_COLOR_SURFACE_HI), LV_PART_SELECTED);
        lv_obj_set_style_text_color(obj, ml_color_hex(ML_COLOR_TEXT), LV_PART_SELECTED);
    } else if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        lv_obj_set_style_text_font(obj, lv_theme_moonlight_get_iconfont_large(obj),
                                   LV_PART_INDICATOR | LV_STATE_CHECKED);
    } else if (lv_obj_check_type(obj, &lv_slider_class)) {
        lv_obj_add_style(obj, &knob_shadow, LV_PART_KNOB);
    }
    if (set_font) {
        lv_obj_set_style_text_font(obj, theme->font_normal, 0);
    }
}

static void lv_start_text_input(lv_event_t *event) {
    lv_obj_t *target = lv_event_get_target(event);
    lv_theme_t *theme = lv_event_get_user_data(event);
    app_t *app = theme->user_data;
    lv_area_t *coords = &target->coords;
    lv_coord_t w = lv_area_get_width(coords), h = lv_area_get_height(coords);
    if (w <= 0 || h <= 0) {
        // No size, no text input
        return;
    }
    app_start_text_input(&app->ui.input, coords->x1, coords->y1, w, h);
}

static void lv_stop_text_input(lv_event_t *event) {
    lv_theme_t *theme = lv_event_get_user_data(event);
    app_t *app = theme->user_data;
    app_stop_text_input(&app->ui.input);
}

static void msgbox_key(lv_event_t *event) {
    lv_obj_t *mbox = lv_event_get_current_target(event);
    if (lv_obj_has_flag(mbox, LV_OBJ_FLAG_USER_4)) {
        return;
    }
    lv_obj_t *target = lv_event_get_target(event);
    lv_group_t *group = lv_obj_get_group(target);
    if (group == NULL) {
        return;
    }
    switch (lv_event_get_key(event)) {
        case LV_KEY_UP: {
            lv_group_focus_prev(group);
            break;
        }
        case LV_KEY_DOWN: {
            lv_group_focus_next(group);
            break;
        }
        default: {
            break;
        }
    }
}

static void msgbox_fit(lv_event_t *event) {
    ui_scroll_fit_msgbox(lv_event_get_current_target(event));
}

/** The window changed size: a dialog whose content was fitted to the OLD size must be
 *  refitted -- a larger window has room to show more of it. The children are walked rather
 *  than the msgbox being remembered, because a msgbox deleted directly would leave that
 *  pointer dangling while the backdrop lives on. */
static void msgbox_backdrop_resized(lv_event_t *event) {
    lv_obj_t *backdrop = lv_event_get_current_target(event);
    const uint32_t count = lv_obj_get_child_cnt(backdrop);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(backdrop, i);
        if (lv_obj_check_type(child, &lv_msgbox_class)) {
            ui_scroll_fit_msgbox(child);
        }
    }
}

static void msgbox_cancel(lv_event_t *event) {
    lv_obj_t *mbox = lv_event_get_current_target(event);
    lv_obj_t *target = lv_event_get_target(event);
    lv_group_t *group = lv_obj_get_group(target);
    if (group == NULL || lv_obj_has_flag(mbox, LV_OBJ_FLAG_USER_4)) {
        return;
    }
    lv_obj_t *btns = lv_msgbox_get_btns(mbox);
    if (btns && !lv_obj_has_flag(btns, LV_OBJ_FLAG_HIDDEN) &&
        !lv_btnmatrix_has_btn_ctrl(btns, 0, LV_BTNMATRIX_CTRL_DISABLED)) {
        lv_msgbox_close(mbox);
    }
}

static void msgbox_destroy(lv_event_t *event) {
    lv_group_t *group = lv_event_get_user_data(event);
    lv_theme_t *theme = group->user_data;
    app_t *app = theme->user_data;
    app_input_remove_modal_group(&app->ui.input, group);
    lv_group_remove_all_objs(group);
    lv_group_del(group);
}
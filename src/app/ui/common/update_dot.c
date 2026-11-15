#include "update_dot.h"

#include "backend/update_check.h"

lv_obj_t *update_dot_create(lv_obj_t *parent) {
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    /* Sized from the parent's font so it scales with the UI like the icon it sits on. */
    lv_coord_t size = lv_font_get_line_height(lv_obj_get_style_text_font(parent, 0)) / 3;
    if (size < 6) {
        size = 6;
    }
    lv_obj_set_size(dot, size, size);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_theme_get_color_primary(parent), 0);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, 0, 0);
    return dot;
}

void update_dot_refresh(lv_obj_t *dot) {
    if (dot == NULL) {
        return;
    }
    if (update_check_dot_visible()) {
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    }
}

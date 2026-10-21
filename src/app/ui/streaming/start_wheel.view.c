#include "start_wheel.view.h"

#include "app.h"
#include "stream/input/start_wheel.h"
#include "util/i18n.h"

#include <string.h>

/* Text only: the embedded icon font is a subset (res/iconfonts/MaterialIcons-Regular.list) without these glyphs. */
static lv_obj_t *slot_create(lv_obj_t *parent, const char *label, lv_coord_t size, lv_coord_t dx, lv_coord_t dy) {
    lv_obj_t *slot = lv_obj_create(parent);
    lv_obj_remove_style_all(slot);
    lv_obj_set_size(slot, size, size);
    lv_obj_align(slot, LV_ALIGN_CENTER, dx, dy);
    lv_obj_set_style_radius(slot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(slot, lv_color_hex(0x1e1e24), 0);
    lv_obj_set_style_border_color(slot, lv_color_white(), 0);
    lv_obj_clear_flag(slot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *text = lv_label_create(slot);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(text, size - LV_DPX(16));
    lv_label_set_text(text, label);
    lv_obj_set_style_text_color(text, lv_color_hex(0xfff7ec), 0);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(text);
    return slot;
}

static void create(start_wheel_view_t *view) {
    lv_obj_t *root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_40, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    const lv_coord_t satellite = LV_DPX(150), centre = LV_DPX(130), offset = LV_DPX(165);
    view->slots[START_WHEEL_CONTINUE] = slot_create(root, locstr("Continue"), centre, 0, 0);
    view->slots[START_WHEEL_MENU] = slot_create(root, locstr("Menu"), satellite, 0, -offset);
    view->slots[START_WHEEL_MOUSE] = slot_create(root, locstr("Mouse"), satellite, offset, 0);
    view->slots[START_WHEEL_KEYBOARD] = slot_create(root, locstr("Keyboard"), satellite, 0, offset);
    view->slots[START_WHEEL_PERFORMANCE] = slot_create(root, locstr("Performance"), satellite, -offset, 0);
    view->root = root;
}

void start_wheel_view_update(start_wheel_view_t *view, int code) {
    if (code <= 0) {
        if (view->root != NULL) {
            lv_obj_add_flag(view->root, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (view->root == NULL) {
        create(view);
    }
    lv_obj_clear_flag(view->root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(view->root);
    const int selected = code - 1;
    for (int i = START_WHEEL_CONTINUE; i <= START_WHEEL_PERFORMANCE; i++) {
        const bool on = i == selected;
        lv_obj_set_style_bg_color(view->slots[i], on ? lv_theme_get_color_primary(view->root) : lv_color_hex(0x1e1e24),
                                  0);
        lv_obj_set_style_bg_opa(view->slots[i], on ? LV_OPA_90 : i == START_WHEEL_CONTINUE ? LV_OPA_80 : LV_OPA_60, 0);
        lv_obj_set_style_border_width(view->slots[i], on ? LV_DPX(4) : 0, 0);
    }
    /* Without the virtual mouse setting the Mouse choice does nothing (session_toggle_vmouse), so it shows faded. */
    lv_obj_set_style_text_opa(lv_obj_get_child(view->slots[START_WHEEL_MOUSE], 0),
                              app_configuration->virtual_mouse ? LV_OPA_COVER : LV_OPA_40, 0);
}

void start_wheel_view_destroy(start_wheel_view_t *view) {
    if (view->root != NULL) {
        lv_obj_del(view->root);
    }
    memset(view, 0, sizeof(*view));
}

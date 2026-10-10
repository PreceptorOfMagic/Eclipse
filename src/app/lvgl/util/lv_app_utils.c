//
// Created by Mariotaku on 2021/09/15.
//

#include "lv_app_utils.h"

#include "util/i18n.h"

static void msgbox_i18n_destroy(lv_event_t *e);


lv_obj_t *lv_child_find_type(lv_obj_t *obj, const lv_obj_class_t *cls);

void lv_btn_set_text_font(lv_obj_t *obj, const lv_font_t *font) {
    lv_obj_t *child = lv_btn_find_label(obj);
    if (!child) return;
    lv_obj_set_style_text_font(child, font, 0);
}

void lv_btn_set_icon_font(lv_obj_t *obj, const lv_font_t *font) {
    lv_obj_t *child = lv_btn_find_img(obj);
    if (!child) return;
    lv_obj_set_style_text_font(child, font, 0);
}

void lv_btn_set_icon(lv_obj_t *obj, const char *symbol) {
    lv_obj_t *child = lv_btn_find_img(obj);
    if (!child) return;
    lv_img_set_src(child, symbol);
}

lv_obj_t *lv_btn_find_img(lv_obj_t *obj) {
    if (!lv_obj_has_class(obj, &lv_btn_class)) return NULL;
    return lv_child_find_type(obj, &lv_img_class);
}

lv_obj_t *lv_btn_find_label(lv_obj_t *obj) {
    if (!lv_obj_has_class(obj, &lv_btn_class)) return NULL;
    return lv_child_find_type(obj, &lv_label_class);
}

lv_obj_t *lv_child_find_type(lv_obj_t *obj, const lv_obj_class_t *cls) {
    for (int i = 0, j = (int) lv_obj_get_child_cnt(obj); i < j; ++i) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        if (lv_obj_has_class(child, cls)) {
            return child;
        }
    }
    return NULL;
}


/* LVGL GIVES EVERY MESSAGE-BOX BUTTON THE SAME FIXED SLOT (2/3 DPI, lv_msgbox.c) WHATEVER ITS LABEL,
 * so "Start co-op" filled its button and "Quit it and start" was cut to "uit it and start" (user,
 * 2026-09-27). Widen to the labels -- never shrink the text (fit-by-space rule) -- and weight each
 * button by its label. A dialog whose labels already fit is left exactly as LVGL made it. */
static void msgbox_fit_buttons(lv_obj_t *btns, const char **labels, int count) {
    if (btns == NULL || count <= 0 || count > 8) {
        return;
    }
    const lv_font_t *font = lv_obj_get_style_text_font(btns, LV_PART_ITEMS);
    const lv_coord_t slot = 2 * LV_DPI_DEF / 3;
    /* Room around the text inside a button: the item padding plus a margin so the label does not
     * touch the button's rounded edge. */
    const lv_coord_t margin = lv_obj_get_style_pad_left(btns, LV_PART_ITEMS) +
                              lv_obj_get_style_pad_right(btns, LV_PART_ITEMS) + LV_DPX(28);
    lv_coord_t need[8];
    lv_coord_t total = 0, least = LV_COORD_MAX;
    bool widen = false;
    for (int i = 0; i < count; i++) {
        lv_point_t size;
        lv_txt_get_size(&size, labels[i], font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        need[i] = LV_MAX(slot, size.x + margin);
        widen |= need[i] > slot;
        total += need[i];
        least = LV_MIN(least, need[i]);
    }
    if (!widen) {
        return;
    }
    total += lv_obj_get_style_pad_column(btns, LV_PART_MAIN) * (count - 1) +
             lv_obj_get_style_pad_left(btns, LV_PART_MAIN) + lv_obj_get_style_pad_right(btns, LV_PART_MAIN);
    lv_obj_set_width(btns, total);
    for (int i = 0; i < count; i++) {
        /* Relative widths are whole numbers 1..7 (lv_btnmatrix); round to the nearest. */
        const lv_coord_t rel = (need[i] * 2 + least) / (2 * least);
        lv_btnmatrix_set_btn_width(btns, (uint16_t) i, (uint8_t) LV_CLAMP(1, rel, 7));
    }
}

lv_obj_t *lv_msgbox_create_i18n(lv_obj_t *parent, const char *title, const char *txt, const char *btn_txts[],
                                bool add_close_btn) {
    int btn_txts_len;
    for (btn_txts_len = 0; btn_txts[btn_txts_len][0]; btn_txts_len++);
    const char **btn_txts_loc = lv_mem_alloc(sizeof(char *) * (btn_txts_len + 1));
    for (int i = 0; i < btn_txts_len; i++) {
        btn_txts_loc[i] = locstr(btn_txts[i]);
    }
    btn_txts_loc[btn_txts_len] = "";
    lv_obj_t *msgbox = lv_msgbox_create(parent, title, txt, btn_txts_loc, add_close_btn);
    lv_obj_add_event_cb(msgbox, msgbox_i18n_destroy, LV_EVENT_DELETE, btn_txts_loc);
    msgbox_fit_buttons(lv_msgbox_get_btns(msgbox), btn_txts_loc, btn_txts_len);
    return msgbox;
}

static void msgbox_i18n_destroy(lv_event_t *e) {
    lv_mem_free(lv_event_get_user_data(e));
}
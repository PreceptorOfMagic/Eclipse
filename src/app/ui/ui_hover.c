#include "ui_hover.h"
#include "ui_input.h"

#include "lvgl.h"

/* 50 ms: fast enough that the highlight feels attached to the cursor, slow enough that
 * the tree walk below is irrelevant next to everything else the UI does. */
#define HOVER_POLL_MS 50

/** Set by ui_hover_mark(): "a click here does something". USER_1..3 are unused in this
 *  app; USER_4 already means "this msgbox drives its own keys" (lv_theme_moonlight.c). */
#define HOVER_TARGET_FLAG LV_OBJ_FLAG_USER_1

static lv_timer_t *hover_timer = NULL;
static lv_obj_t *hovered = NULL;
static app_ui_input_t *hover_input = NULL;

static void hover_clear(void) {
    if (hovered != NULL) {
        lv_obj_clear_state(hovered, LV_STATE_HOVERED);
        hovered = NULL;
    }
}

static void hover_deleted_cb(lv_event_t *e) {
    /* An object can be destroyed while hovered -- a dialog closing under the cursor is
     * the common case. Without this, the stale pointer would be cleared later and
     * write into freed memory. */
    if (lv_event_get_target(e) == hovered) {
        hovered = NULL;
    }
}

void ui_hover_mark(lv_obj_t *obj) {
    if (obj != NULL) {
        lv_obj_add_flag(obj, HOVER_TARGET_FLAG);
    }
}

/**
 * Would a click on @p obj DO something?
 *
 * Marked objects say so themselves. Everything else has to be a widget that acts on a
 * click without being told to -- a button, a checkbox, a dropdown, a slider, a text
 * area. Containers are excluded even though LVGL makes them clickable by default,
 * because a highlight around a box that swallows the click and does nothing is worse
 * than no highlight at all.
 *
 * A BUTTON MATRIX IS DELIBERATELY NOT HERE. Its buttons are drawn parts, not objects,
 * and a state can only be set on the whole widget -- so hovering one button would
 * outline the entire strip, naming the wrong target. The msgbox button row is the one
 * that matters here.
 */
static bool hover_target(lv_obj_t *obj) {
    if (lv_obj_has_flag(obj, HOVER_TARGET_FLAG)) {
        return true;
    }
    return lv_obj_has_class(obj, &lv_btn_class) ||
           lv_obj_has_class(obj, &lv_checkbox_class) ||
           lv_obj_has_class(obj, &lv_dropdown_class) ||
           lv_obj_has_class(obj, &lv_slider_class) ||
           lv_obj_has_class(obj, &lv_imgbtn_class) ||
           lv_obj_has_class(obj, &lv_textarea_class);
}

/** The target @p hit belongs to: itself, or the nearest ancestor that is one. A card
 *  built from plain objects hands its clicks to the card, so the highlight has to name
 *  the card rather than the decoration the cursor happens to be over. */
static lv_obj_t *hover_owner(lv_obj_t *hit) {
    for (lv_obj_t *obj = hit; obj != NULL; obj = lv_obj_get_parent(obj)) {
        if (hover_target(obj)) {
            return obj;
        }
    }
    return NULL;
}

/**
 * ONE SELECTION INDICATOR AT A TIME.
 *
 * The focus ring says where the d-pad is, the hover outline says where the cursor is,
 * and both drawn at once reads as two things selected. With a pointer that happens
 * constantly, because clicking anything also focuses it. So the ring follows the ACTIVE
 * input: hidden while the pointer drives, restored the moment a key, stick or button is
 * used. FOCUS ITSELF NEVER MOVES -- the d-pad still carries on from wherever the
 * pointer left off, which is the whole point of taking focus on a click.
 *
 * LV_STATE_FOCUS_KEY is the right switch: LVGL sets it only for keypad and encoder
 * devices (lv_obj.c), every focus ring in this app is already keyed on it, and the
 * settings screens do this by hand at each group change (detail_defocus, on_tab_key) --
 * this is the same rule in one place, for every screen.
 */
static void focus_ring_follows_input(bool pointing) {
    if (hover_input == NULL) {
        return;
    }
    lv_indev_t *key = hover_input->key.indev;
    lv_group_t *group = key != NULL ? key->group : NULL;
    lv_obj_t *focused = group != NULL ? lv_group_get_focused(group) : NULL;
    if (focused == NULL) {
        return;
    }
    /* Only ever the ACTIVE group's focused object: screens that keep a second group
     * parked (settings' nav vs detail) rely on its ring staying off. */
    if (pointing) {
        lv_obj_clear_state(focused, LV_STATE_FOCUS_KEY);
    } else {
        lv_obj_add_state(focused, LV_STATE_FOCUS_KEY);
    }
}

/** Is @p point inside @p obj's own area? Used to decide whether to DESCEND, which
 *  lv_obj_hit_test() cannot answer: it returns false for anything not CLICKABLE, and
 *  the containers we must walk through are exactly that. */
static bool point_in_obj(lv_obj_t *obj, const lv_point_t *point) {
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return point->x >= area.x1 && point->x <= area.x2 &&
           point->y >= area.y1 && point->y <= area.y2;
}

/** Topmost clickable object under @p point, or NULL. Children are walked back to front
 *  because the LAST child is drawn on top and must win the hit. */
static lv_obj_t *hover_search(lv_obj_t *obj, const lv_point_t *point) {
    if (obj == NULL || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (!point_in_obj(obj, point)) {
        return NULL;
    }
    const uint32_t count = lv_obj_get_child_cnt(obj);
    for (int32_t i = (int32_t) count - 1; i >= 0; --i) {
        lv_obj_t *found = hover_search(lv_obj_get_child(obj, (uint32_t) i), point);
        if (found != NULL) {
            return found;
        }
    }
    return lv_obj_hit_test(obj, point) ? obj : NULL;
}

static void hover_timer_cb(lv_timer_t *timer) {
    LV_UNUSED(timer);
    if (hover_input == NULL || hover_input->pointer.indev == NULL) {
        hover_clear();
        return;
    }
    /* The cursor is meaningless once the user has picked up a controller or the
     * d-pad; the focus ring is the selection then, and two highlights would fight. */
    if (!(hover_input->mode & UI_INPUT_MODE_POINTER_FLAG)) {
        hover_clear();
        focus_ring_follows_input(false);
        return;
    }
    focus_ring_follows_input(true);

    lv_point_t point;
    lv_indev_get_point(hover_input->pointer.indev, &point);

    /* Top layer first: modal dialogs live there, and a msgbox over the launcher must
     * take the hover rather than the buttons showing through underneath it. */
    lv_obj_t *found = hover_search(lv_layer_top(), &point);
    if (found == NULL) {
        found = hover_search(lv_scr_act(), &point);
    }
    found = hover_owner(found);
    if (found == hovered) {
        return;
    }
    hover_clear();
    if (found != NULL) {
        hovered = found;
        lv_obj_add_state(found, LV_STATE_HOVERED);
        lv_obj_add_event_cb(found, hover_deleted_cb, LV_EVENT_DELETE, NULL);
    }
}

void ui_hover_input_mode_changed(void) {
    /* IMMEDIATELY, NOT ON THE NEXT TICK. The mode changes inside the input driver's
     * read callback, which runs BEFORE LVGL turns that press into events -- and screens
     * read LV_STATE_FOCUS_KEY to decide what a press means (settings asks whether the
     * detail pane holds it before choosing between "go back to the nav" and "close
     * settings"). A ring that is 50 ms late is a key that does the wrong thing. */
    if (hover_input == NULL) {
        return;
    }
    const bool pointing = (hover_input->mode & UI_INPUT_MODE_POINTER_FLAG) != 0;
    if (!pointing) {
        /* The cursor's highlight goes at once too, or for one frame the d-pad ring and
         * a stale hover outline are both on screen. */
        hover_clear();
    }
    focus_ring_follows_input(pointing);
}

void ui_hover_init(app_ui_input_t *input) {
    hover_input = input;
    if (hover_timer == NULL) {
        hover_timer = lv_timer_create(hover_timer_cb, HOVER_POLL_MS, NULL);
    }
}

void ui_hover_deinit(void) {
    if (hover_timer != NULL) {
        lv_timer_del(hover_timer);
        hover_timer = NULL;
    }
    hovered = NULL;
    hover_input = NULL;
}

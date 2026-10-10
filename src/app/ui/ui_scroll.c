#include "ui_scroll.h"

/* One wheel notch moves this far. Small on purpose: the wheel steps through content, while
 * the d-pad moves focus and snaps to the next item -- the two must feel different. */
#define WHEEL_STEP_DPX 36

/* How far left of the drawn thumb a press still grabs the bar. The bar sits in its
 * container's right padding, and a larger margin would steal clicks from the rows beside it. */
#define BAR_GRAB_SLOP_DPX 3

/** Is @p point inside @p obj's own area? */
static bool point_in_obj(lv_obj_t *obj, const lv_point_t *point) {
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return point->x >= area.x1 && point->x <= area.x2 &&
           point->y >= area.y1 && point->y <= area.y2;
}

/** Deepest visible object under @p point. Unlike lv_indev_search_obj() this does NOT require
 *  CLICKABLE: scroll containers are often not clickable, and they are what we are after.
 *  Children are walked back to front because the LAST child is drawn on top. */
static lv_obj_t *scroll_search(lv_obj_t *obj, const lv_point_t *point) {
    if (obj == NULL || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) || !point_in_obj(obj, point)) {
        return NULL;
    }
    const uint32_t count = lv_obj_get_child_cnt(obj);
    for (int32_t i = (int32_t) count - 1; i >= 0; --i) {
        lv_obj_t *found = scroll_search(lv_obj_get_child(obj, (uint32_t) i), point);
        if (found != NULL) {
            return found;
        }
    }
    return obj;
}

/** What the pointer is over. Top layer first -- a modal dialog lives there, and the wheel
 *  must scroll the dialog, never the screen showing through behind its backdrop. */
static lv_obj_t *scroll_hit(const lv_point_t *point) {
    lv_obj_t *top = lv_layer_top();
    lv_obj_t *found = scroll_search(top, point);
    if (found != NULL && found != top) {
        return found;
    }
    return scroll_search(lv_scr_act(), point);
}

/**
 * How far @p obj can still scroll by @p d (LVGL's sign: positive reveals content above or
 * to the left). 0 if it does not scroll that way at all.
 */
static lv_coord_t scroll_room(lv_obj_t *obj, bool vertical, lv_coord_t d) {
    if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE)) {
        return 0;
    }
    const lv_dir_t dir = lv_obj_get_scroll_dir(obj);
    if (!(dir & (vertical ? LV_DIR_VER : LV_DIR_HOR))) {
        return 0;
    }
    if (vertical) {
        return d > 0 ? lv_obj_get_scroll_top(obj) : lv_obj_get_scroll_bottom(obj);
    }
    return d > 0 ? lv_obj_get_scroll_left(obj) : lv_obj_get_scroll_right(obj);
}

/** Scroll the innermost ancestor of @p hit that can still move by @p d; an ancestor at its
 *  end hands the movement outward, as a desktop browser does. */
static bool scroll_chain(lv_obj_t *hit, bool vertical, lv_coord_t d) {
    for (lv_obj_t *obj = hit; obj != NULL; obj = lv_obj_get_parent(obj)) {
        const lv_coord_t room = scroll_room(obj, vertical, d);
        if (room <= 0) {
            continue;
        }
        /* Clamped to the room left, so the elastic overscroll a touch drag allows never
         * appears -- a wheel has no release to spring back from. */
        const lv_coord_t step = LV_CLAMP(-room, d, room);
        if (vertical) {
            lv_obj_scroll_by(obj, 0, step, LV_ANIM_OFF);
        } else {
            lv_obj_scroll_by(obj, step, 0, LV_ANIM_OFF);
        }
        return true;
    }
    return false;
}

bool ui_scroll_wheel(const lv_point_t *point, float notches_x, float notches_y) {
    lv_obj_t *hit = scroll_hit(point);
    if (hit == NULL) {
        return false;
    }
    const lv_coord_t step = LV_DPX(WHEEL_STEP_DPX);
    if (notches_y != 0) {
        /* Wheel away from the user = see what is above = LVGL's positive direction. */
        const lv_coord_t d = (lv_coord_t) (notches_y * (float) step);
        if (d != 0 && scroll_chain(hit, true, d)) {
            return true;
        }
        /* Over something that only scrolls sideways (a row of cards), the ordinary wheel
         * moves it sideways -- there is no vertical movement for it to mean. */
        return d != 0 && scroll_chain(hit, false, d);
    }
    /* Tilting right = see what is to the right = LVGL's negative direction. */
    const lv_coord_t d = (lv_coord_t) (-notches_x * (float) step);
    return d != 0 && scroll_chain(hit, false, d);
}

/* ---- Scrollbar dragging -------------------------------------------------------------- */

/* LVGL draws scrollbars but never lets a pointer drag them; on a desktop that is the first
 * thing a mouse user tries. Both bars: the co-op pane's controller-routing strip scrolls
 * SIDEWAYS (coop.pane.c pad_body, LV_DIR_HOR) and its bar could not be grabbed (issue #38). */
static struct {
    lv_obj_t *obj;       /* being dragged, NULL when idle */
    bool hor;            /* dragging the horizontal bar (x axis) rather than the vertical one */
    lv_coord_t press;    /* pointer position along the bar's axis at the press */
    lv_coord_t start;    /* scroll position at the press */
    lv_coord_t range;    /* largest scroll position */
    lv_coord_t travel;   /* how far the thumb itself can move */
} bar;

static void bar_deleted_cb(lv_event_t *e) {
    /* A dialog can close in the middle of a drag; the next move must not touch it. */
    if (lv_event_get_target(e) == bar.obj) {
        bar.obj = NULL;
    }
}

static void bar_scroll_to(lv_coord_t pos) {
    pos = LV_CLAMP(0, pos, bar.range);
    if (bar.hor) {
        lv_obj_scroll_to_x(bar.obj, pos, LV_ANIM_OFF);
    } else {
        lv_obj_scroll_to_y(bar.obj, pos, LV_ANIM_OFF);
    }
}

/** Start dragging @p obj's bar along one axis. @p thumb is the bar as LVGL draws it, @p at the
 *  pointer along that axis, @p track the bar's usable length. False when the thumb cannot move. */
static bool bar_begin(lv_obj_t *obj, bool hor, const lv_area_t *thumb, lv_coord_t at, lv_coord_t track,
                      lv_coord_t range, lv_coord_t now) {
    const lv_coord_t t1 = hor ? thumb->x1 : thumb->y1, t2 = hor ? thumb->x2 : thumb->y2;
    const lv_coord_t travel = track - (t2 - t1 + 1);
    if (travel <= 0) {
        return false;
    }
    /* Removed first so the callback is on the object once, however many drags it has
     * seen -- release() clears bar.obj, so every drag would otherwise add another. */
    lv_obj_remove_event_cb(obj, bar_deleted_cb);
    lv_obj_add_event_cb(obj, bar_deleted_cb, LV_EVENT_DELETE, NULL);
    bar.obj = obj;
    bar.hor = hor;
    bar.range = range;
    bar.travel = travel;
    bar.press = at;
    bar.start = now;
    if (at < t1 || at > t2) {
        /* On the track but off the thumb: bring the thumb's middle to the pointer,
         * then drag from there, as a desktop scrollbar does. */
        const lv_coord_t moved = at - (t1 + t2) / 2;
        bar.start = LV_CLAMP(0, bar.start + moved * bar.range / bar.travel, bar.range);
        bar_scroll_to(bar.start);
    }
    return true;
}

bool ui_scroll_bar_press(const lv_point_t *point) {
    /* A press always ends a drag whose release never arrived (a stream started mid-drag, or
     * the button came up while no motion followed): a stale drag would scroll the old object
     * on the next held move and force LVGL's pointer up under the new press. */
    bar.obj = NULL;
    for (lv_obj_t *obj = scroll_hit(point); obj != NULL; obj = lv_obj_get_parent(obj)) {
        if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE)) {
            continue;
        }
        /* The thumbs as LVGL draws them -- empty when this object shows no bar (mode OFF, or
         * ACTIVE while idle), and an invisible bar must not be grabbable. */
        lv_area_t hor, ver;
        lv_obj_get_scrollbar_area(obj, &hor, &ver);
        const lv_coord_t slop = LV_DPX(BAR_GRAB_SLOP_DPX);
        const lv_coord_t vrange = lv_obj_get_scroll_top(obj) + lv_obj_get_scroll_bottom(obj);
        if (vrange > 0 && ver.x2 >= ver.x1 && ver.y2 > ver.y1 &&
            point->x >= ver.x1 - slop && point->x <= obj->coords.x2 &&
            point->y >= obj->coords.y1 && point->y <= obj->coords.y2) {
            const lv_coord_t track = lv_obj_get_height(obj) - lv_obj_get_style_pad_top(obj, LV_PART_SCROLLBAR) -
                                     lv_obj_get_style_pad_bottom(obj, LV_PART_SCROLLBAR);
            if (bar_begin(obj, false, &ver, point->y, track, vrange, lv_obj_get_scroll_y(obj))) {
                return true;
            }
        }
        const lv_coord_t hrange = lv_obj_get_scroll_left(obj) + lv_obj_get_scroll_right(obj);
        if (hrange > 0 && hor.y2 >= hor.y1 && hor.x2 > hor.x1 &&
            point->y >= hor.y1 - slop && point->y <= obj->coords.y2 &&
            point->x >= obj->coords.x1 && point->x <= obj->coords.x2) {
            const lv_coord_t track = lv_obj_get_width(obj) - lv_obj_get_style_pad_left(obj, LV_PART_SCROLLBAR) -
                                     lv_obj_get_style_pad_right(obj, LV_PART_SCROLLBAR);
            if (bar_begin(obj, true, &hor, point->x, track, hrange, lv_obj_get_scroll_x(obj))) {
                return true;
            }
        }
    }
    return false;
}

bool ui_scroll_bar_dragging(void) {
    return bar.obj != NULL;
}

void ui_scroll_bar_move(const lv_point_t *point) {
    if (bar.obj == NULL) {
        return;
    }
    const lv_coord_t at = bar.hor ? point->x : point->y;
    bar_scroll_to(bar.start + (at - bar.press) * bar.range / bar.travel);
}

void ui_scroll_bar_release(void) {
    bar.obj = NULL;
}

/* ---- Dialogs that fit the window ----------------------------------------------------- */

/** @p value resolved against @p parent's content height when it is a percentage. */
static lv_coord_t resolve_height(lv_coord_t value, lv_obj_t *parent) {
    if (LV_COORD_IS_PCT(value)) {
        return parent != NULL ? lv_obj_get_content_height(parent) * LV_COORD_GET_PCT(value) / 100 : LV_COORD_MAX;
    }
    return value;
}

void ui_scroll_fit_msgbox(lv_obj_t *mbox) {
    lv_obj_t *content = lv_msgbox_get_content(mbox);
    if (content == NULL) {
        return;
    }
    lv_obj_t *parent = lv_obj_get_parent(mbox);
    lv_coord_t limit = resolve_height(lv_obj_get_style_max_height(mbox, LV_PART_MAIN), parent);
    const lv_coord_t height = lv_obj_get_style_height(mbox, LV_PART_MAIN);
    if (height != LV_SIZE_CONTENT) {
        /* A dialog given a fixed height (the settings panes) fits inside THAT. */
        limit = LV_MIN(limit, lv_obj_get_height(mbox));
    }
    if (limit >= LV_COORD_MAX / 2) {
        return;
    }
    /* Everything that is not the content: the title row and padding above it, the button
     * row and padding below it. Measured from the laid-out positions, so a close button
     * sharing the title row, a two-line title or custom padding all count. */
    lv_coord_t other = content->coords.y1 - mbox->coords.y1 + lv_obj_get_scroll_y(mbox);
    other += lv_obj_get_style_pad_bottom(mbox, LV_PART_MAIN) + lv_obj_get_style_border_width(mbox, LV_PART_MAIN);
    lv_obj_t *btns = lv_msgbox_get_btns(mbox);
    if (btns != NULL && !lv_obj_has_flag(btns, LV_OBJ_FLAG_HIDDEN)) {
        other += lv_obj_get_height(btns) + lv_obj_get_style_pad_row(mbox, LV_PART_MAIN);
    }
    /* A floor, so a window shrunk to nothing still leaves a strip of content to scroll
     * rather than a negative size; below it the dialog itself scrolls, as before. */
    const lv_coord_t room = LV_MAX(limit - other, LV_DPX(40));
    if (lv_obj_get_style_max_height(content, LV_PART_MAIN) != room) {
        lv_obj_set_style_max_height(content, room, 0);
    }
}

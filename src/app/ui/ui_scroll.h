#pragma once

#include <stdbool.h>

#include "lvgl.h"

/**
 * Scroll whatever scrolls under @p point by a wheel movement, in small steps -- the desktop
 * counterpart of the d-pad, which moves focus and snaps to the next item instead.
 * @p notches_x / @p notches_y are SDL wheel units (positive y = away from the user).
 * @return true if something scrolled.
 */
bool ui_scroll_wheel(const lv_point_t *point, float notches_x, float notches_y);

/**
 * A pointer press at @p point: if it lands on a visible vertical scrollbar, start dragging
 * it and return true -- the caller must then keep the press away from LVGL, or it would
 * also drag the content and click whatever sits under the bar.
 */
bool ui_scroll_bar_press(const lv_point_t *point);

/** True while a scrollbar drag started by ui_scroll_bar_press() is in progress. */
bool ui_scroll_bar_dragging(void);

/** Pointer moved during a scrollbar drag. */
void ui_scroll_bar_move(const lv_point_t *point);

/** Pointer released (or lost) during a scrollbar drag. */
void ui_scroll_bar_release(void);

/**
 * Keep @p mbox within its max height by letting only its CONTENT scroll: the title stays
 * at the top and the button row at the bottom, whatever the window size or however much
 * the content holds. The fonts never change size to make things fit.
 */
void ui_scroll_fit_msgbox(lv_obj_t *mbox);

#pragma once

#include <lvgl.h>

/* THE START WHEEL'S PICTURE (stream/input/start_wheel.h), in V+'s layout: the screen dimmed, Continue in the centre,
 * Menu above, Mouse to the right, Keyboard below, Performance to the left; the highlighted one brighter and outlined.
 * On LVGL's top layer, above the stream, like the pinned performance panel. */
typedef struct start_wheel_view_t {
    lv_obj_t *root;
    lv_obj_t *slots[5];     /* by start_wheel_action_t */
} start_wheel_view_t;

/** @p code: 0 hides the wheel; otherwise it shows with action (code - 1) highlighted (USER_START_WHEEL's data1). */
void start_wheel_view_update(start_wheel_view_t *view, int code);

void start_wheel_view_destroy(start_wheel_view_t *view);

#pragma once

#include "lvgl.h"

/* The update notice's dot (backend/update_check.h): a small filled circle in the top-right corner of
 * `parent`, outside its layout, never focusable or clickable. Starts hidden. */
lv_obj_t *update_dot_create(lv_obj_t *parent);

/* Shows the dot when backend/update_check.h says the user has an update they have not looked at. */
void update_dot_refresh(lv_obj_t *dot);

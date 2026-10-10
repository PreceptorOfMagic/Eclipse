/*
 * Pointer hover highlighting.
 *
 * LVGL v8 DECLARES LV_STATE_HOVERED AND NEVER SETS IT -- there is not one reference to
 * it in lv_indev.c. Its pointer handling only hit-tests on PRESS, so between clicks
 * nothing in the tree knows the cursor is over it. That is why nothing anywhere in
 * Eclipse lights up under the magic remote: not a missing style, a missing state.
 *
 * So this drives the state itself: on a timer, find what is under the pointer and mark
 * it hovered. The theme attaches a hover style to every object, so once the state is set
 * the highlight is uniform across the whole app rather than something each screen has to
 * remember to implement.
 *
 * ONLY WHILE THE POINTER IS THE ACTIVE INPUT. The LG remote's cursor vanishes when you
 * put it down and start using the d-pad, and leaving a highlight behind under an
 * invisible cursor would compete with the focus ring and suggest two selections at
 * once. The same rule runs the other way too -- see ui_hover.c on the focus ring.
 *
 * WHAT MAY LIGHT UP IS A SHORT LIST, NOT "whatever the cursor is over". A plain
 * container is CLICKABLE BY DEFAULT in LVGL (lv_obj.c: `obj->flags =
 * LV_OBJ_FLAG_CLICKABLE`), so hit-testing alone lit up every box that happened to lie
 * under the pointer -- section frames, column boxes, the icon inside a card -- most of
 * which do nothing at all when clicked. Widgets that act on a click by themselves are
 * recognised here; anything else has to say so with ui_hover_mark().
 */
#pragma once

#include "lvgl.h"

typedef struct app_ui_input_t app_ui_input_t;

/** Start tracking. Safe to call once at UI init; does nothing without a pointer. */
void ui_hover_init(app_ui_input_t *input);

void ui_hover_deinit(void);

/**
 * Declare @p obj a hover target: a click on it does something, so the pointer may
 * highlight it. For the hand-built targets -- cards, tiles, rows made of plain objects
 * and labels -- that LVGL has no way to recognise.
 */
void ui_hover_mark(lv_obj_t *obj);

/** The active input device changed: hand the highlight over NOW rather than on the next
 *  poll, because the key that caused the change is dispatched before then. */
void ui_hover_input_mode_changed(void);

#pragma once

#include <SDL.h>

#include "ui/ui_input.h"

/**
 * THE RIGHT STICK SCROLLS (user, 2026-11-04): in every menu and dialog, the right stick's up/down
 * scrolls the scrollable area that holds the focused item -- a dialog's text, a settings page, a
 * list. It never moves focus (the left stick and the D-pad do that, navkey_sdl.c) and never
 * scrolls sideways. A stick resting in someone's lap must not drift: it engages past 60 % of its
 * travel and lets go under 35 %, the same hysteresis as the left stick.
 *
 * Fed only with events the UI gets (lv_drv_sdl_key.c): during a stream the stick belongs to the game.
 */

/** A right-stick Y axis event that reached the UI. */
void ui_stick_scroll_axis(app_ui_input_t *input, SDL_JoystickID which, int value);

/** A controller went away: forget its stick, or a held deflection would scroll forever. */
void ui_stick_scroll_forget(SDL_JoystickID which);

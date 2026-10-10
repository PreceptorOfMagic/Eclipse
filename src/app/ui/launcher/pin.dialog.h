#pragma once

#include "lvgl.h"
#include "uuidstr.h"

/**
 * Per-host Wake-on-LAN sign-in credential editor: which kind (PIN or password), the
 * value, and an on-screen keyboard matched to the kind — a number pad for a PIN, a
 * full one for a password.
 *
 * Opened from Settings -> Host Settings, which lists one row per known PC. It is a
 * plain modal msgbox rather than a fragment: it is opened from inside a settings
 * pane, which has no fragment manager of its own to push onto.
 *
 * @param uuid     host whose credential is being set.
 * @param on_saved called after OK has written the credential, so the caller can
 *                 refresh whatever it shows about it. May be NULL.
 * @param userdata passed back to on_saved.
 */
void pin_dialog_open(const uuidstr_t *uuid, void (*on_saved)(void *), void *userdata);

#pragma once

#include "lvgl.h"

typedef struct launcher_fragment_t launcher_fragment_t;

/**
 * The co-op SESSION CONFIGURATION dialog, opened from the "Co-op" entry in the server
 * picker. Everything a co-op run needs and nothing a normal stream needs: which
 * controller drives which host, how the screen splits, which PC is player 1, and the
 * button that starts it.
 *
 * It sits under "select server" rather than in the app list because co-op is not an
 * application -- it is a session spanning TWO hosts, so an entry filed under one PC
 * could never say what the other one is.
 */
void coop_dialog_open(launcher_fragment_t *launcher);

typedef struct app_t app_t;

/**
 * Start a co-op session again from the choices the last one used, with no dialog.
 *
 * WHY THIS EXISTS: the PIN auto-entry quits the stream ~9 s in and relaunches it, so
 * Apollo re-runs its HDR display setup in the now-logged-in session (a resume does
 * not). That relaunch used to be `launcher_launch_game(one app id)`, which in a co-op
 * session tore BOTH halves down and brought neither back -- a co-op run started from
 * a woken PC died after nine seconds, every time. A co-op session is not one app on
 * one host, so it needs its own way back.
 *
 * The host pair and orientation are persisted by launch_session(). The co-op app id is
 * deliberately not: ids are host-local, so relaunch queries both current app lists and
 * resolves the exact "Co-op Pane" entry again.
 *
 * @return false when the remembered hosts are no longer usable, so the caller can
 *         fall back rather than silently doing nothing.
 */
bool coop_dialog_relaunch_last(app_t *global);

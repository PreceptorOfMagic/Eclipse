#pragma once

/* The update notice (docs/update-notice.md).
 *
 * At most once a day, when the launcher opens (never during a stream), Eclipse downloads the Project
 * Hub's updates.json in the background and keeps a copy in its settings folder. If a notifying release
 * newer than this build applies to this platform, the launcher's Settings button and the About entry
 * carry a small dot until the user has opened its "What's new"; About then keeps one line saying the
 * version is available. Nothing pops up, and a failed check is silent and retried at a later launch.
 * The setting "Check for updates" (check_updates in moonlight.ini) turns the download off. */

#include <stdbool.h>

#include "backend/update_manifest.h"

typedef struct app_t app_t;

typedef void (*update_check_listener)(void *ctx);

/* Reads the saved copy and state, so a known update shows at once, offline too. */
void update_check_init(app_t *app);

/* Stops a download in progress (it is aborted, not waited for) and frees everything. */
void update_check_deinit(void);

/* Starts a download if the setting is on and the last successful check is a day old. */
void update_check_maybe_start(void);

/* The current result; info->available is false when there is nothing to show. Never NULL. */
const update_info_t *update_check_info(void);

/* True while there is an update whose "What's new" the user has not opened yet. */
bool update_check_dot_visible(void);

/* The user opened "What's new" for the current update: the dot goes away for that version. */
void update_check_mark_seen(void);

/* One listener (the launcher), called on the UI thread whenever the result or the dot changes.
 * Pass NULL to remove it. */
void update_check_set_listener(update_check_listener listener, void *ctx);

/* "webos", "windows", "linux" or "android": the key used in updates.json. */
const char *update_check_platform(void);

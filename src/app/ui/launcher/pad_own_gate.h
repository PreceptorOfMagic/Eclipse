#pragma once

#include <stdbool.h>

#include "platform/pad_ownership.h"

/**
 * THE STREAM-START STEP for controller ownership (user requirement 2026-11-03): when this PC is one
 * of the co-op hosts, the controllers assigned to the OTHER PC are hidden from this PC, and the
 * programs that already hold them are found. If any of those must be closed and reopened, the user
 * is told what, why and which apps -- or their remembered choice (setting `pad_apps_policy`) is
 * applied. See platform/pad_ownership.h for the mechanism.
 *
 * @p done is called on the UI thread with proceed = true to continue the launch, false = cancelled
 * (everything already undone). Not called after pad_own_gate_cancel().
 */
typedef void (*pad_own_gate_done_fn)(void *ctx, bool proceed);

/** @return true when the gate took over (done comes later); false = nothing to do, launch now. */
bool pad_own_gate_begin(const pad_own_pad_t *pads, int count, pad_own_gate_done_fn done, void *ctx);

/** The caller is going away: never call its done callback. */
void pad_own_gate_cancel(void *ctx);

/** Values of `pad_apps_policy` (app_settings.h). */
enum {
    PAD_APPS_ASK = 0,
    PAD_APPS_CLOSE_REOPEN = 1,
    PAD_APPS_LEAVE = 2,
};

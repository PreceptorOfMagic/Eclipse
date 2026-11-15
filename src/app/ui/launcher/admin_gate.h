#pragma once

#include <stdbool.h>

/**
 * ADMIN FOR A LOCAL-PANE SESSION (user 2026-11-12: "change how the admin works altogether to instead always ask
 * and say all the reasons why whenever opening a single pc host client combo"). Before a co-op session in which
 * this PC is both a host and the client, the user is told everything administrator rights are used for, then
 * Windows asks once (platform/pad_ownership.h, pad_own_session_admin_begin). Already elevated: nothing is asked.
 * Declined: the session goes on, and the user is told what will not work.
 *
 * @p done runs on the UI thread: proceed = true to continue the launch, false = cancelled.
 */
typedef void (*admin_gate_done_fn)(void *ctx, bool proceed);

/** @return true when the gate took over (done comes later); false = nothing to ask, launch now. */
bool admin_gate_begin(admin_gate_done_fn done, void *ctx);

/** The caller is going away: never call its done callback. */
void admin_gate_cancel(void *ctx);

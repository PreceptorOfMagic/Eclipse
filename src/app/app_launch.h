#pragma once

#include "uuidstr.h"
#include <stdbool.h>

typedef struct app_t app_t;

typedef struct app_launch_params_t {
    uuidstr_t default_host_uuid;
    int default_app_id;
    /* Co-op is armed BY THE LAUNCH ("coop":true in the -p payload), never by leftover
     * state. File sentinels in /tmp outlive the run that armed them and silently turned
     * later ordinary streams into co-op streams; the app cannot even clean them up,
     * because they belong to another uid in a sticky directory. */
    bool coop_requested;
} app_launch_params_t;

app_launch_params_t *app_handle_launch(app_t *app, int argc, char *argv[]);

/**
 * Whether THIS launch asked for co-op.
 *
 * A tiny global rather than plumbing the launch params through the video stack: the
 * value is set once at startup, read once at stream setup, and never changes. The
 * point is only that it is per-LAUNCH -- the previous file-sentinel activation
 * persisted across runs and silently made later ordinary streams co-op streams.
 */
bool app_launch_coop_requested(void);
void app_launch_set_coop_requested(bool requested);

/**
 * CO-OP WITH THIS PC'S SIDE RUN LOCALLY (user 2026-11-10: "a pc acting as client and host isn't streaming its
 * own plane to itself instead we are running the pc acting as client and host fully locally and composing in the
 * other pc's stream"). The launch about to start is such a session: ONE ordinary stream, to the other PC's co-op
 * pane, drawn in one half of the window; the other half is this PC's own virtual display shown directly
 * (platform/local_pane_session.h). app_launch_coop_requested() is FALSE in such a session on purpose -- nothing
 * is spliced, there is no second stream and no second process, so every two-stream path stays off.
 *
 * @p vertical: the halves are side by side (a vertical divider), else stacked. @p local_first: this PC is
 * player 1's machine, so its half is the left / top one. Set by the co-op launch and cleared by every other
 * launch, like the flag above; latched by the session when it starts.
 */
bool app_launch_local_pane(void);
bool app_launch_local_pane_vertical(void);
bool app_launch_local_pane_first(void);
void app_launch_set_local_pane(bool active, bool vertical, bool local_first);

/**
 * HOST MONITOR OFF for the launch about to start (app_settings.h host_monitor_mode). Called at every
 * user-started launch with what the user picked: @p chose_off is the "host monitor off" start button, and
 * only counts in CHOOSE mode -- OFF and ON answer for every launch. Published in the environment, like the
 * co-op codec, so the co-op secondary (a separate process spawned after this) streams its pane the same way.
 */
void app_launch_choose_host_monitor(bool chose_off);
/** This launch keeps every host's monitor on, whatever host_monitor_mode says: every CO-OP launch (user,
 *  2026-11-04 -- co-op offers no monitor-off). Same environment value, so the co-op secondary's pane follows. */
void app_launch_keep_host_monitors_on(void);
/** The answer for this launch; session_worker.c sends it only to a host that offers it (UmbraCaps bit 4). */
bool app_launch_host_monitor_off(void);
/** CHOOSE mode: does a launch offer the second, monitor-off start button? */
bool app_launch_host_monitor_choice_offered(void);

void app_launch_param_free(app_launch_params_t *param);
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
 * HOST MONITOR OFF for the launch about to start (app_settings.h host_monitor_mode). Called at every
 * user-started launch with what the user picked: @p chose_off is the "host monitor off" start button, and
 * only counts in CHOOSE mode -- OFF and ON answer for every launch. Published in the environment, like the
 * co-op codec, so the co-op secondary (a separate process spawned after this) streams its pane the same way.
 */
void app_launch_choose_host_monitor(bool chose_off);
/** The answer for this launch; session_worker.c sends it only to a host that offers it (UmbraCaps bit 4). */
bool app_launch_host_monitor_off(void);
/** CHOOSE mode: does a launch offer the second, monitor-off start button? */
bool app_launch_host_monitor_choice_offered(void);

void app_launch_param_free(app_launch_params_t *param);
#include "stream/audio/coop_audio.h"
#include "coop_session.h"

#include "app_settings.h"
#include "backend/pcmanager.h"
#include "logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern PCONFIGURATION app_configuration;
extern pcmanager_t *pcmanager;

/** The name the primary hands the resolved mode to the secondary under. An environment
 *  variable rather than a sentinel file or the ini, for the reason the PIN arm already
 *  uses one: the secondary is a separate process spawned LATER from this one, so it
 *  inherits this environment, while /tmp sentinels outlive the run that wrote them and
 *  the ini can say AUTO -- which the secondary has no dialog to resolve. */
#define COOP_STREAM_MODE_ENV "COOP_STREAM_MODE"
/** Same contract, separate variable, because the two answers are separate: a session can
 *  be H.264 with HDR irrelevant, or HEVC with HDR denied by one host's capability. */
#define COOP_HDR_ENV "COOP_HDR_ALLOWED"
/** Same contract again, for the orientation the session actually runs. Deliberately NOT
 *  named `COOP_ORIENTATION`: run_coop.sh already uses that name in the SHELL, carrying
 *  "horizontal"/"vertical" rather than the enum, and a scripted run that exported it
 *  would be read here as a number. This one is ours and holds the enum value. */
#define COOP_ORIENTATION_ENV "COOP_ORIENTATION_SEL"

/* File-static, like the rest of the co-op run state. Written by the UI thread before
 * the stream fragment is pushed and read by the video thread at session start -- an
 * ordering the fragment lifecycle already guarantees, so no lock is warranted here. */
static coop_session_t g_session;

int coop_session_resolve_app_id(const APP_LIST *apps, int preferred_id) {
    if (preferred_id > 0) {
        for (const APP_LIST *app = apps; app != NULL; app = app->next) {
            if (app->id == preferred_id && app->name != NULL &&
                strcmp(app->name, COOP_SESSION_APP_NAME) == 0) {
                return app->id;
            }
        }
    }
    for (const APP_LIST *app = apps; app != NULL; app = app->next) {
        if (app->id > 0 && app->name != NULL &&
            strcmp(app->name, COOP_SESSION_APP_NAME) == 0) {
            return app->id;
        }
    }
    return 0;
}

void coop_session_set(const coop_session_t *session) {
    if (session == NULL) {
        coop_session_clear();
        return;
    }
    g_session = *session;
    g_session.valid = true;
    setenv("COOP_GPU_HEVC", session->gpu_hevc ? "1" : "0", 1);
    /* Publish the orientation the same moment the codec is published, so the secondary --
     * a fresh process that reloads moonlight.ini -- runs the dialog's choice and not the
     * saved one. See coop_orientation_resolved(). */
    setenv(COOP_ORIENTATION_ENV,
           session->orientation == COOP_ORIENTATION_VERTICAL ? "1" : "0", 1);
}

const coop_session_t *coop_session_get(void) {
    return g_session.valid ? &g_session : NULL;
}

void coop_session_clear(void) {
    memset(&g_session, 0, sizeof(g_session));
    /* A pinned output device (this-PC-as-host co-op) belongs to that session only. */
    coop_audio_release_local_host();
    /* AND THE PUBLISHED CODEC WITH IT. This runs when a LATER launch is not a co-op one,
     * which is the same moment app_launch_set_coop_requested(false) happens
     * (apps.controller.c) -- so it cannot race the secondary, which was spawned long
     * before. Without it, a co-op session followed by a sentinel-driven co-op run in the
     * SAME app process would inherit the dialog's codec instead of resolving its own. */
    unsetenv(COOP_STREAM_MODE_ENV);
    unsetenv(COOP_HDR_ENV);
    unsetenv(COOP_ORIENTATION_ENV);
    unsetenv("COOP_GPU_HEVC");
    unsetenv("COOP_PIN_AUTOENTRY");
    unsetenv("COOP_PIN_WOKEN");
}

bool coop_gpu_hevc_resolved(void) {
    const coop_session_t *session = coop_session_get();
    if (session != NULL) return session->gpu_hevc;
    const char *v = getenv("COOP_GPU_HEVC");
    return v != NULL && strcmp(v, "1") == 0;
}

coop_stream_plan_t coop_session_plan_for_pair(const uuidstr_t *primary, const uuidstr_t *secondary,
                                             const SERVER_DATA *primary_server) {
    coop_host_facts_t facts[2];
    const uuidstr_t *ids[2] = {primary, secondary};
    for (int i = 0; i < 2; ++i) {
        const pclist_t *node = pcmanager != NULL && ids[i] != NULL
                               ? pcmanager_node(pcmanager, ids[i]) : NULL;
        const SERVER_DATA *server = i == 0 && primary_server != NULL
                                    ? primary_server : (node != NULL ? node->server : NULL);
        facts[i] = coop_host_facts_from_server(server, node != NULL
                    ? (coop_gpu_vendor_t) node->gpu_vendor : COOP_GPU_UNKNOWN);
    }
    return coop_stream_plan_choose(&facts[0], &facts[1], app_configuration != NULL
                                   ? (coop_encoder_choice_t) app_configuration->coop_encoder
                                   : COOP_ENCODER_AUTO,
                                   app_configuration != NULL && app_configuration->hdr);
}

bool coop_session_prepare_scripted(const SERVER_DATA *primary_server, const char *spawn_path) {
    if (coop_session_get() != NULL) {
        return true;
    }
    if (primary_server == NULL || primary_server->uuid == NULL || spawn_path == NULL) {
        return false;
    }
    coop_session_t session = {0};
    FILE *spawn = fopen(spawn_path, "r");
    if (spawn == NULL) {
        commons_log_error("Coop", "scripted launch has no co-op peer configuration");
        return false;
    }
    const int fields = fscanf(spawn, "%63s %d", session.secondary_uuid,
                              &session.secondary_app_id);
    fclose(spawn);
    if (fields != 2 || session.secondary_app_id <= 0 ||
        strlen(session.secondary_uuid) != UUIDSTR_LENGTH ||
        strlen(primary_server->uuid) != UUIDSTR_LENGTH) {
        commons_log_error("Coop", "invalid scripted co-op peer configuration");
        return false;
    }
    snprintf(session.primary_uuid, sizeof(session.primary_uuid), "%s", primary_server->uuid);
    /* The scripted sentinel predates per-host resolution and intentionally carries one
     * app id. Preserve that harness contract by applying it to both panes; UI launches
     * resolve the two live app lists independently. */
    session.primary_app_id = session.secondary_app_id;
    uuidstr_t primary, secondary;
    uuidstr_fromstr(&primary, session.primary_uuid);
    uuidstr_fromstr(&secondary, session.secondary_uuid);
    const coop_stream_plan_t plan = coop_session_plan_for_pair(&primary, &secondary, primary_server);
    session.stream_mode = plan.mode;
    session.gpu_hevc = plan.gpu_hevc;
    session.hdr_allowed = plan.hdr_allowed ? 1 : 2;
    session.orientation = coop_orientation_for_mode(app_configuration != NULL
                            ? app_configuration->coop_orientation : COOP_ORIENTATION_HORIZONTAL,
                            plan.mode);
    coop_stream_mode_publish(plan.mode, plan.hdr_allowed);
    coop_session_set(&session);
    /* Scripted launches bypass the dialog's PIN arm as well as its codec plan.
     * Honour the saved opt-in; the secondary still requires a fresh positive
     * lock report. This path did not wake the host, so unknown lock state must
     * not authorize typing a credential. Publish zero too to replace stale state. */
    setenv("COOP_PIN_AUTOENTRY", app_configuration != NULL &&
           app_configuration->pin_autoentry ? "1" : "0", 1);
    setenv("COOP_PIN_WOKEN", "0", 1);
    commons_log_info("Coop", "scripted pair resolved: mode=%d vendors_known=%d mixed=%d hdr=%d",
                    plan.mode, plan.vendors_known, plan.mixed_vendor, plan.hdr_allowed);
    return true;
}

void coop_stream_mode_publish(coop_stream_mode_t mode, bool hdr_allowed) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", (int) mode);
    setenv(COOP_STREAM_MODE_ENV, buf, 1);
    setenv(COOP_HDR_ENV, hdr_allowed ? "1" : "0", 1);
}

bool coop_hdr_allowed_resolved(void) {
    const coop_session_t *session = coop_session_get();
    if (session != NULL && session->hdr_allowed != 0) {
        return session->hdr_allowed == 1;
    }
    const char *env = getenv(COOP_HDR_ENV);
    if (env != NULL && (env[0] == '0' || env[0] == '1')) {
        return env[0] == '1';
    }
    return true;
}

coop_stream_mode_t coop_stream_mode_resolved(void) {
    return COOP_STREAM_CORE; /* Legacy persisted AVC values cannot revive that path. */
}

int coop_orientation_resolved(void) {
    const coop_session_t *session = coop_session_get();
    if (session != NULL) {
        return session->orientation == COOP_ORIENTATION_VERTICAL
                   ? COOP_ORIENTATION_VERTICAL
                   : COOP_ORIENTATION_HORIZONTAL;
    }
    /* The secondary has no session of its own -- this is the primary's published answer. */
    const char *env = getenv(COOP_ORIENTATION_ENV);
    if (env != NULL && (env[0] == '0' || env[0] == '1')) {
        return env[0] == '1' ? COOP_ORIENTATION_VERTICAL : COOP_ORIENTATION_HORIZONTAL;
    }
    /* Neither: a launch path with no dialog and no primary -- the standing setting, which
     * is what the ini exists for (`ares-launch "coop":true`, the sentinel harness). */
    return app_configuration != NULL && app_configuration->coop_orientation == COOP_ORIENTATION_VERTICAL
               ? COOP_ORIENTATION_VERTICAL
               : COOP_ORIENTATION_HORIZONTAL;
}

int coop_orientation_for_mode(int orientation, coop_stream_mode_t mode) {
    (void) mode;
    return orientation;
}

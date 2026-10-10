/* Exercise the real scripted launch snapshot with a small in-memory host cache.
 * The exec check models the secondary's fresh process and inherited environment. */
#include "coop_session.h"
#include "app_settings.h"
#include "backend/pcmanager.h"
#include "logging.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define EXPECT_MODE COOP_STREAM_CORE
#define EXPECT_ORIENTATION COOP_ORIENTATION_VERTICAL
#define EXPECT_HDR true
#define EXPECT_MODE_ENV "1"
static CONFIGURATION config;
PCONFIGURATION app_configuration = &config;
static pclist_t nodes[2];
pcmanager_t *pcmanager = (pcmanager_t *) nodes;

const pclist_t *pcmanager_node(pcmanager_t *manager, const uuidstr_t *uuid) {
    assert(manager == pcmanager);
    for (unsigned i = 0; i < 2; ++i) {
        if (uuidstr_t_equals_t(&nodes[i].id, uuid)) { return &nodes[i]; }
    }
    return NULL;
}

void commons_log_printf(commons_log_level level, const char *tag, const char *fmt, ...) {
    (void) level; (void) tag; (void) fmt;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--secondary") == 0) {
        config.coop_stream_mode = COOP_STREAM_CORE; /* inherited decision wins */
        assert(coop_session_get() == NULL);
        assert(coop_stream_mode_resolved() == EXPECT_MODE);
        assert(coop_hdr_allowed_resolved() == EXPECT_HDR);
        assert(strcmp(getenv("COOP_PIN_AUTOENTRY"), "1") == 0);
        assert(strcmp(getenv("COOP_PIN_WOKEN"), "0") == 0);
        /* THE ORIENTATION BUG THIS GUARDS (measured 2026-09-17 14:34): this fresh image has
         * the SAVED setting (zero-initialised here = HORIZONTAL) while the primary published
         * VERTICAL. What the user picked in the dialog must win, or the two halves run
         * different columns of the per-orientation dials -- one host encoded at
         * coop_bitrate_v and the other at coop_bitrate_h in the same session. */
        assert(config.coop_orientation == COOP_ORIENTATION_HORIZONTAL);
        assert(coop_orientation_resolved() == EXPECT_ORIENTATION);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--gpu-secondary") == 0) {
        assert(coop_session_get() == NULL && coop_gpu_hevc_resolved());
        return 0;
    }
    const char *nv_id = "11111111-1111-1111-1111-111111111111";
    const char *amd_id = "22222222-2222-2222-2222-222222222222";
    uuidstr_fromstr(&nodes[0].id, nv_id);
    uuidstr_fromstr(&nodes[1].id, amd_id);
    nodes[0].gpu_vendor = COOP_GPU_NVIDIA;
    nodes[1].gpu_vendor = COOP_GPU_AMD;
    /* Neither cached node has serverinfo yet: hosts.ini still identifies this pair. */
    SERVER_DATA nv = {.uuid = nv_id, .encoderVendorKnown = true, .encoderVendor = "nvidia",
                      .supportsHdr = true, .coopProfileCaps = COOP_PROFILE_CAP_MAIN_CONTROL};
    config.hdr = true;
    config.pin_autoentry = true;
    config.coop_orientation = COOP_ORIENTATION_VERTICAL;
    config.coop_stream_mode = COOP_STREAM_AUTO;

    char path[] = "/tmp/aurora-session-smoke.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    assert(f != NULL);
    fputs("invalid-peer -1\n", f);
    fclose(f);
    coop_session_clear();
    assert(!coop_session_prepare_scripted(&nv, path));
    assert(coop_session_get() == NULL && getenv("COOP_STREAM_MODE") == NULL);

    f = fopen(path, "w");
    assert(f != NULL);
    fprintf(f, "%s 12345\n", amd_id);
    fclose(f);
    assert(coop_session_prepare_scripted(&nv, path));
    const coop_session_t *session = coop_session_get();
    assert(session != NULL && strcmp(session->secondary_uuid, amd_id) == 0);
    assert(coop_gpu_hevc_resolved() == EXPECT_HDR);
    assert(session->primary_app_id == 12345 && session->secondary_app_id == 12345 &&
           session->orientation == EXPECT_ORIENTATION);
    assert(coop_stream_mode_resolved() == EXPECT_MODE && coop_hdr_allowed_resolved() == EXPECT_HDR);
    assert(strcmp(getenv("COOP_STREAM_MODE"), EXPECT_MODE_ENV) == 0);
    /* The session's own orientation wins here, and is published for the secondary. */
    assert(coop_orientation_resolved() == EXPECT_ORIENTATION);
    assert(strcmp(getenv("COOP_ORIENTATION_SEL"), "1") == 0);

    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) { execl(argv[0], argv[0], "--secondary", NULL); _exit(127); }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* The selected pair is snapshotted; later file changes cannot replace it. */
    assert(coop_session_prepare_scripted(&nv, path));
    coop_session_clear();
    assert(getenv("COOP_STREAM_MODE") == NULL && getenv("COOP_HDR_ALLOWED") == NULL);
    assert(getenv("COOP_PIN_AUTOENTRY") == NULL && getenv("COOP_PIN_WOKEN") == NULL);
    assert(getenv("COOP_ORIENTATION_SEL") == NULL);
    /* With no session and nothing published, the standing setting is what is left -- the
     * path `ares-launch "coop":true` and the sentinel harness take. */
    config.coop_orientation = COOP_ORIENTATION_HORIZONTAL;
    assert(coop_orientation_resolved() == COOP_ORIENTATION_HORIZONTAL);
    config.coop_orientation = COOP_ORIENTATION_VERTICAL;
    assert(coop_orientation_resolved() == COOP_ORIENTATION_VERTICAL);
    config.pin_autoentry = false;
    setenv("COOP_PIN_AUTOENTRY", "1", 1);
    setenv("COOP_PIN_WOKEN", "1", 1);
    assert(coop_session_prepare_scripted(&nv, path));
    assert(strcmp(getenv("COOP_PIN_AUTOENTRY"), "0") == 0);
    assert(strcmp(getenv("COOP_PIN_WOKEN"), "0") == 0);
    coop_session_clear();
    unlink(path);
    assert(!coop_session_prepare_scripted(&nv, path));

    /* App ids belong to a host, not to an app name globally. A stale configured id
     * that now names Desktop must never be accepted; exact name resolution finds each
     * host's independent Co-op Pane id and fails closed when it is absent. */
    APP_LIST primary_coop = {.name = COOP_SESSION_APP_NAME, .id = 4101};
    APP_LIST primary_desktop = {.name = "Desktop", .id = 12345, .next = &primary_coop};
    APP_LIST secondary_coop = {.name = COOP_SESSION_APP_NAME, .id = 9207};
    assert(coop_session_resolve_app_id(&primary_desktop, 12345) == 4101);
    assert(coop_session_resolve_app_id(&primary_desktop, 4101) == 4101);
    assert(coop_session_resolve_app_id(&secondary_coop, 12345) == 9207);
    assert(coop_session_resolve_app_id(&primary_desktop, 0) == 4101);
    assert(coop_session_resolve_app_id(&primary_desktop, 99999) == 4101);
    assert(coop_session_resolve_app_id(&primary_desktop, -1) == 4101);
    assert(coop_session_resolve_app_id(&primary_desktop, 12345) != 12345);
    assert(coop_session_resolve_app_id(&primary_desktop, 0) != 0);
    assert(coop_session_resolve_app_id(&primary_desktop, 0) != 12345);
    primary_desktop.next = NULL;
    assert(coop_session_resolve_app_id(&primary_desktop, 12345) == 0);
    APP_LIST almost_coop = {.name = "Co-op pane", .id = 7000};
    assert(coop_session_resolve_app_id(&almost_coop, 7000) == 0);

    coop_session_t ui = {.stream_mode = COOP_STREAM_CORE, .hdr_allowed = 1,
                         .orientation = COOP_ORIENTATION_VERTICAL};
    coop_session_set(&ui);
    assert(coop_session_prepare_scripted(&nv, path));
    assert(coop_stream_mode_resolved() == COOP_STREAM_CORE && coop_hdr_allowed_resolved());
    assert(!coop_gpu_hevc_resolved());
    ui.gpu_hevc = true;
    coop_session_set(&ui);
    assert(coop_gpu_hevc_resolved());
    child = fork(); assert(child >= 0);
    if (child == 0) { execl(argv[0], argv[0], "--gpu-secondary", NULL); _exit(127); }
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    coop_session_clear();
    assert(!coop_gpu_hevc_resolved() && getenv("COOP_GPU_HEVC") == NULL);
    puts("PASS: scripted AUTO resolution, host-local app ids, snapshot, UI priority and exec handoff");
    return 0;
}

#include "app.h"
#include "app_launch.h"
#include "util/path.h"
#include "logging.h"
#include "stream/video/coop_secondary.h"
#include "stream/video/coop_coord.h"

#if defined(TARGET_WEBOS) && !defined(DEBUG)

#include <sys/resource.h>

#endif

#if defined(TARGET_ANDROID)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform/android/android_platform.h"

#endif

static int settings_load(app_settings_t *settings);

int main(int argc, char *argv[]) {
#if defined(TARGET_ANDROID)
    /* libmain.so's main() is SDL_main, called by SDLActivity. The co-op peer is the same program built as a
     * plain executable (libeclipse_peer.so) and exec'd by the primary; SDL must be told its entry point
     * was handled, as SDL_main.h does for SDLActivity. */
    bool android_peer = false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != NULL && strcmp(argv[i], "--coop-secondary") == 0) {
            android_peer = true;
        }
    }
#if defined(ECLIPSE_ANDROID_PEER)
    SDL_SetMainReady();
#endif
    if (!android_platform_init(android_peer)) {
        return 1;
    }
#endif
#ifdef TARGET_WEBOS
    if (getenv("EGL_PLATFORM") == NULL) {
        setenv("EGL_PLATFORM", "wayland", 0);
    }
    if (getenv("XDG_RUNTIME_DIR") == NULL) {
        setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 0);
    }
#ifndef DEBUG
    // Don't generate core dumps in release builds
    struct rlimit rlim = {0, 0};
    setrlimit(RLIMIT_CORE, &rlim);
#endif
#endif
    /* Co-op split-screen: `--coop-secondary <socket>` makes this process the
     * SECOND session. It connects to the coordinator first, so a missing or
     * dead coordinator aborts before a pointless stream is started. */
    coop_secondary_parse_args(argc, argv);
    const char *coop_sock = coop_secondary_mode();
    if (coop_sock == NULL) {
        /* A crashed coordinator can leave a child that app-manager close cannot
         * reach. Reap it even when reopening only the launcher, before any new
         * connection or media initialization. The reaper checks executable,
         * socket and PPid=1; a child of a live coordinator is left alone. */
        coop_coord_reap_orphan_secondaries(COOP_COORD_SOCKET);
    }
    if (coop_sock != NULL && !coop_secondary_init(coop_sock)) {
        fprintf(stderr, "coop: cannot reach coordinator at %s\n", coop_sock);
        return 1;
    }

    app_t app;
    int ret = app_init(&app, settings_load, argc, argv);
    if (ret != 0) {
        return ret;
    }

    app_launch_params_t *params = app_handle_launch(&app, argc, argv);

    if (coop_sock != NULL) {
        /* HEADLESS. The secondary must never call app_ui_open: creating a window
         * would put it in the foreground, and webOS grants one foreground app --
         * taking it would suspend the coordinator's stream, which is exactly how
         * Multi-View ejected Eclipse when it tried to render. This process is
         * network plus socket only.
         *
         * It still needs app_init above for settings, certificates and host data.
         */
        const int secondary_result = coop_secondary_run_headless(&app);
        coop_secondary_shutdown();
        app_deinit(&app);
        return secondary_result;
    }

    app_ui_open(&app.ui, true, params);
#if defined(TARGET_ANDROID) && defined(ECLIPSE_ANDROID_SELFTEST)
    android_selftest_start();   /* test builds only */
#endif

    while (app.running) {
        app_run_loop(&app);
    }

    app_launch_param_free(params);

    app_deinit(&app);
#if defined(TARGET_ANDROID)
    /* SDLActivity may start SDL_main again in this same process (a relaunch after the activity finished),
     * and none of this program's static state would be reset. End the process with the program, as on every
     * other platform -- with _exit: exit() would also run the destructors of Android's own libraries while
     * their render threads still run, which Android 16 aborts on ("FORTIFY: pthread_mutex_lock called on a
     * destroyed mutex" in hwuiTask, a tombstone on every quit; emulator, 2026-10-15). Everything of ours is
     * closed by app_deinit above. */
    fflush(NULL);
    _exit(ret);
#endif
    return ret;
}


static int settings_load(app_settings_t *settings) {
    bool persistent = true;
    char *conf_dir = path_pref(&persistent);
    if (conf_dir == NULL) {
        commons_log_error("Settings", "Could not create the per-user configuration directory");
        return -1;
    }
    settings_initialize(settings, conf_dir);
    settings->conf_persistent = persistent;
    if (!settings_read(settings)) {
        commons_log_warn("Settings", "Failed to read settings %s", settings->ini_path);
    }
    return 0;
}

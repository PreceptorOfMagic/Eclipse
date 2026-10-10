#include <errno.h>
#include <stdlib.h>
#include "input_gamepad_mapping.h"
#include "app_settings.h"
#include "executor.h"
#include "gamecontrollerdb_updater.h"
#include "util/user_event.h"
#include "util/path.h"
#include "logging.h"


static void gcdb_updated(commons_gcdb_status_t result, void *context);

static char *gamecontrollerdb_builtin_path();

static char *gamecontrollerdb_extra_path();

#if defined(OS_WINDOWS) || (defined(OS_LINUX) && !defined(TARGET_WEBOS) && !defined(TARGET_STEAMLINK))
static char *portable_tls_ca_path();
#endif

void app_input_init_gamepad_mapping(app_input_t *input, executor_t *executor, const app_settings_t *settings) {
#if defined(OS_WINDOWS) || (defined(OS_LINUX) && !defined(TARGET_WEBOS) && !defined(TARGET_STEAMLINK))
    input->gcdb_ca_path = portable_tls_ca_path();
    input->gcdb_updater.ca_path = input->gcdb_ca_path;
#else
    input->gcdb_ca_path = NULL;
#endif
    input->gcdb_updater.callback = gcdb_updated;
    input->gcdb_updater.path = settings->condb_path;
    input->gcdb_updater.platform = GAMECONTROLLERDB_PLATFORM;
#ifdef GAMECONTROLLERDB_PLATFORM_USE
    input->gcdb_updater.platform_use = GAMECONTROLLERDB_PLATFORM_USE;
#endif
    char *condb_extra = gamecontrollerdb_extra_path();
    if (condb_extra != NULL) {
        int num_mapping = SDL_GameControllerAddMappingsFromRW(SDL_RWFromFile(condb_extra, "r"), SDL_TRUE);
        free(condb_extra);
        commons_log_debug("Input", "Added %d gamepad mapping from extra controller db", num_mapping);
    }
    commons_gcdb_updater_init(&input->gcdb_updater, executor);
    commons_gcdb_updater_update(&input->gcdb_updater);
}

void app_input_deinit_gamepad_mapping(app_input_t *input) {
    commons_gcdb_updater_deinit(&input->gcdb_updater);
    free(input->gcdb_ca_path);
    input->gcdb_ca_path = NULL;
}

void app_input_copy_initial_gamepad_mapping(const app_settings_t *settings) {
    char *builtin_path = gamecontrollerdb_builtin_path();
    if (builtin_path == NULL) {
        return;
    }
    if (path_file_readable(settings->condb_path)) {
        commons_log_debug("Input", "Skip copying initial controller db file");
    } else if (path_file_copy(builtin_path, settings->condb_path) == 0) {
        commons_log_info("Input", "Copied initial controller db file");
    } else {
        commons_log_warn("Input", "Failed to copy initial controller db file: %s", strerror(errno));
    }
    free(builtin_path);
}

void app_input_reload_gamepad_mapping(app_input_t *input) {
    int num_mapping = SDL_GameControllerAddMappingsFromRW(SDL_RWFromFile(input->gcdb_updater.path, "r"),
                                                          SDL_TRUE);
    commons_log_debug("Input", "Added %d gamepad mapping", num_mapping);
}

static void gcdb_updated(commons_gcdb_status_t result, void *context) {
    (void) context;
    if (result != COMMONS_GCDB_UPDATER_UPDATED) {
        return;
    }
    SDL_Event event = {
            .user = {
                    .type = SDL_USEREVENT,
                    .code = USER_INPUT_CONTROLLERDB_UPDATED,
            }
    };
    SDL_PushEvent(&event);
}


static char *gamecontrollerdb_builtin_path() {
    char *assetsdir = path_assets(), *condb = path_join(assetsdir, "gamecontrollerdb.txt");
    free(assetsdir);
    if (!path_file_readable(condb)) {
        free(condb);
        return NULL;
    }
    return condb;
}

static char *gamecontrollerdb_extra_path() {
    char *assetsdir = path_assets(), *condb = path_join(assetsdir, "gamecontrollerdb_extra.txt");
    free(assetsdir);
    if (!path_file_readable(condb)) {
        free(condb);
        return NULL;
    }
    return condb;
}

#if defined(OS_WINDOWS) || (defined(OS_LINUX) && !defined(TARGET_WEBOS) && !defined(TARGET_STEAMLINK))
static char *portable_tls_ca_path() {
    /* MSYS2's OpenSSL-backed libcurl is built with an MSYS2-local CA path. That
     * path does not exist on a machine which only has the portable package, so
     * use the CA bundle shipped beside the executable. The updater loads the
     * PEM through its UTF-16 file boundary and gives libcurl a memory blob, so
     * a Unicode install directory never passes through OpenSSL's narrow fopen.
     * The portable Linux libcurl has no CA store of its own either (no single
     * distribution's path is right on all of them); a desktop Linux build without
     * the bundled file -- a distribution package -- keeps libcurl's default. */
    const char *configured = getenv("SSL_CERT_FILE");
    if (configured != NULL && configured[0] != '\0' && path_file_readable(configured)) {
        commons_log_debug("GameControllerDB", "Using configured TLS CA bundle");
        return strdup(configured);
    }

    char *assetsdir = path_assets();
    if (assetsdir == NULL) {
        commons_log_warn("GameControllerDB", "Could not resolve the portable TLS CA bundle path");
        return NULL;
    }
    char *ca_bundle = path_join(assetsdir, "curl-ca-bundle.crt");
    free(assetsdir);
    if (ca_bundle == NULL || !path_file_readable(ca_bundle)) {
#ifdef OS_WINDOWS
        commons_log_warn("GameControllerDB", "Portable TLS CA bundle is missing");
#endif
        free(ca_bundle);
        return NULL;
    }
    return ca_bundle;
}
#endif

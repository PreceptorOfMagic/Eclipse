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

/* Android ships the desktops' pinned CA bundle too, so the controller updater trusts the same roots on every
 * client whatever the device's own store holds (and wherever a given Android release keeps it). */
#if defined(OS_WINDOWS) || defined(TARGET_ANDROID) || \
    (defined(OS_LINUX) && !defined(TARGET_WEBOS) && !defined(TARGET_STEAMLINK))
static char *portable_tls_ca_path();
#endif

void app_input_init_gamepad_mapping(app_input_t *input, executor_t *executor, const app_settings_t *settings) {
#if defined(OS_WINDOWS) || defined(TARGET_ANDROID) || \
    (defined(OS_LINUX) && !defined(TARGET_WEBOS) && !defined(TARGET_STEAMLINK))
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
#if TARGET_ANDROID
    app_input_add_missing_gamepad_mappings(input->gcdb_updater.path);
#else
    int num_mapping = SDL_GameControllerAddMappingsFromRW(SDL_RWFromFile(input->gcdb_updater.path, "r"),
                                                          SDL_TRUE);
    commons_log_debug("Input", "Added %d gamepad mapping", num_mapping);
#endif
}

#if TARGET_ANDROID
/*
 * ON ANDROID THE DOWNLOADED DATABASE ONLY FILLS GAPS. SDL's own table keys some Android mappings by Android version
 * (`sdk>=:29` / `sdk<=:28`: the key layouts changed with Android 10 and 11) and keeps the one for this device. The
 * community gamecontrollerdb.txt carries the old one with no version, and loaded over SDL's (as the config-file hint
 * does) it won: a Nintendo Switch Pro Controller on the SHIELD TV (Android 11) got Android 9's layout -- + as the
 * right stick, ZR as Start, L and R as the triggers, ZL as View -- so no button shortcut and no Start wheel worked on
 * it (logged mapping and raw buttons, 2026-10-22). A controller whose mapping SDL chose by Android version keeps it;
 * the database still adds every controller SDL does not know and replaces SDL's other mappings, as it always did.
 * Lines are filtered by platform exactly as SDL_GameControllerAddMappingsFromRW does.
 */
int app_input_add_missing_gamepad_mappings(const char *path) {
    SDL_RWops *rw = path != NULL ? SDL_RWFromFile(path, "r") : NULL;
    if (rw == NULL) {
        return 0;
    }
    const Sint64 size = SDL_RWsize(rw);
    char *buf = size > 0 ? SDL_malloc((size_t) size + 1) : NULL;
    if (buf == NULL || SDL_RWread(rw, buf, (size_t) size, 1) != 1) {
        SDL_free(buf);
        SDL_RWclose(rw);
        return 0;
    }
    SDL_RWclose(rw);
    buf[size] = '\0';
    char platform_field[64];
    SDL_snprintf(platform_field, sizeof(platform_field), "platform:%s,", SDL_GetPlatform());
    int added = 0, kept = 0;
    for (char *line = buf, *next; line != NULL && *line != '\0'; line = next) {
        next = SDL_strchr(line, '\n');
        if (next != NULL) {
            *next++ = '\0';
        }
        const size_t len = SDL_strlen(line);
        if (len > 0 && line[len - 1] == '\r') {
            line[len - 1] = '\0';
        }
        const char *comma = SDL_strchr(line, ',');
        if (line[0] == '#' || comma == NULL || comma - line >= 64 || SDL_strstr(line, platform_field) == NULL) {
            continue;
        }
        char guid_text[64];
        SDL_strlcpy(guid_text, line, (size_t) (comma - line) + 1);
        /* Only a mapping SDL chose by Android version keeps out the database's: the others the database still
         * replaces as before (the Xbox Series controller's Share button, misc1, is in the database's and not SDL's). */
        char *existing = SDL_GameControllerMappingForGUID(SDL_JoystickGetGUIDFromString(guid_text));
        const bool by_version = existing != NULL &&
                                (SDL_strstr(existing, "sdk>=:") != NULL || SDL_strstr(existing, "sdk<=:") != NULL);
        SDL_free(existing);
        if (by_version) {
            kept++;
            continue;
        }
        if (SDL_GameControllerAddMapping(line) > 0) {
            added++;
        }
    }
    SDL_free(buf);
    commons_log_info("Input", "Controller database: %d mappings applied, %d controllers kept SDL's Android-version "
                              "mapping", added, kept);
    return added;
}
#endif

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

#if defined(OS_WINDOWS) || defined(TARGET_ANDROID) || \
    (defined(OS_LINUX) && !defined(TARGET_WEBOS) && !defined(TARGET_STEAMLINK))
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

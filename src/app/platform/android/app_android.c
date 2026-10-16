/*
 * Android counterparts of platform/webos/app_webos.c: opening a URL, the UI language, and launch parameters.
 */
#include "app.h"
#include "app_launch.h"
#include "android_platform.h"

#include "logging.h"
#include "util/i18n.h"
#include "stream/video/coop_secondary.h"

#include <SDL.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char locale_system[16];

void app_open_url(const char *url) {
    SDL_OpenURL(url);
}

void app_init_locale() {
    if (coop_secondary_mode() != NULL) {
        /* The peer shows nothing, and has no Java VM to ask for the system's locales. */
        return;
    }
    /* As on webOS: the in-app language wins; "auto" follows the system's preferred locales in order and takes
     * the first one Eclipse has a catalogue for. */
    if (app_configuration->language[0] && strcmp(app_configuration->language, "auto") != 0) {
        commons_log_debug("APP", "Override language to %s", app_configuration->language);
        i18n_setlocale(app_configuration->language);
        return;
    }
    SDL_Locale *locales = SDL_GetPreferredLocales();
    if (locales == NULL) {
        return;
    }
    for (int i = 0; locales[i].language; i++) {
        if (locales[i].country) {
            snprintf(locale_system, sizeof(locale_system), "%s-%s", locales[i].language, locales[i].country);
        } else {
            snprintf(locale_system, sizeof(locale_system), "%s", locales[i].language);
        }
        const i18n_entry_t *entry = i18n_entry(locale_system);
        if (entry == NULL || strcmp(entry->locale, "auto") == 0) {
            continue;
        }
        i18n_setlocale(entry->locale);
        if (i18n_is_loaded()) {
            break;
        }
    }
    SDL_free(locales);
}

/*
 * Launch parameters. EclipseActivity turns the launching Intent's extras into these arguments -- the same
 * three fields as the webOS launch payload ({"host_uuid", "host_app_id", "coop"}):
 *
 *     --host-uuid <uuid>   --host-app-id <id>   --coop
 *
 * `adb shell am start -n <package>/.EclipseActivity --es host_uuid <uuid> --ei host_app_id <id> --ez coop true`
 * starts a stream directly, as `ares-launch -p` does on the TV.
 */
app_launch_params_t *app_handle_launch(app_t *app, int argc, char *argv[]) {
    (void) app;
    bool secondary = false;
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] != NULL && strcmp(argv[i], "--coop-secondary") == 0) {
            secondary = true;
            break;
        }
    }
    char stamp[32];
    const time_t now = time(NULL);
    struct tm tm_buf;
    if (localtime_r(&now, &tm_buf) == NULL ||
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_buf) == 0) {
        snprintf(stamp, sizeof(stamp), "%ld", (long) now);
    }
    commons_log_info("APP", "Run start %s pid=%d role=%s (tick 0 is this instant)",
                     stamp, (int) getpid(), secondary ? "COOP-SECONDARY" : "primary");
    commons_log_info("APP", "Device: %s, Android API %d", android_device_name(), android_sdk_int());
    if (secondary) {
        return NULL;
    }
#if defined(ECLIPSE_ANDROID_SELFTEST)
    android_selftest_parse(argc, argv);
#endif
    app_launch_params_t *params = NULL;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (arg == NULL) {
            continue;
        }
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(arg, "--host-uuid") == 0 && value != NULL) {
            if (params == NULL) params = calloc(1, sizeof(*params));
            if (params == NULL) return NULL;
            uuidstr_fromchars(&params->default_host_uuid, strlen(value), value);
            ++i;
        } else if (strcmp(arg, "--host-app-id") == 0 && value != NULL) {
            if (params == NULL) params = calloc(1, sizeof(*params));
            if (params == NULL) return NULL;
            const long id = strtol(value, NULL, 10);
            params->default_app_id = id > 0 && id < 0x7fffffff ? (int) id : 0;
            ++i;
        } else if (strcmp(arg, "--coop") == 0) {
            if (params == NULL) params = calloc(1, sizeof(*params));
            if (params == NULL) return NULL;
            params->coop_requested = true;
        }
    }
    if (params == NULL) {
        return NULL;
    }
    /* Per launch, never sticky -- see the note in app_webos.c. */
    app_launch_set_coop_requested(params->coop_requested);
    char uuidbuf[64] = {0};
    memcpy(uuidbuf, params->default_host_uuid.data,
           sizeof(params->default_host_uuid.data) < sizeof(uuidbuf) - 1
           ? sizeof(params->default_host_uuid.data) : sizeof(uuidbuf) - 1);
    commons_log_info("APP", "Launch parsed: app_id=%d host_uuid=%s coop=%d",
                     params->default_app_id, uuidbuf, (int) params->coop_requested);
    return params;
}

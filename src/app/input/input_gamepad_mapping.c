#include <errno.h>
#include <stdlib.h>
#include "input_gamepad_mapping.h"
#include "app_settings.h"
#include "executor.h"
#include "gamecontrollerdb_updater.h"
#include "util/user_event.h"
#include "util/path.h"
#include "logging.h"

#if TARGET_ANDROID
#include <android/keycodes.h>
#include "platform/android/android_platform.h"
#include "platform/android/controller_mapping_android.h"

static void free_gamepad_db(void);
#endif

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
#if TARGET_ANDROID
    free_gamepad_db();
#endif
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
    app_input_load_gamepad_db(input->gcdb_updater.path);
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        app_input_fit_gamepad_mapping(i);
    }
#else
    int num_mapping = SDL_GameControllerAddMappingsFromRW(SDL_RWFromFile(input->gcdb_updater.path, "r"),
                                                          SDL_TRUE);
    commons_log_debug("Input", "Added %d gamepad mapping", num_mapping);
#endif
}

#if TARGET_ANDROID
/*
 * ON ANDROID THE MAPPING IS CHOSEN PER CONTROLLER, by how it fits the buttons Android says that controller can
 * produce on this Android version (platform/android/controller_mapping_android.h says why the GUID cannot tell).
 * The database is kept here rather than loaded into SDL, where any line for the controller's model replaced SDL's
 * own whichever Android version it was written on. When a controller connects, SDL's own mapping (built in, or
 * made from the key codes), every database line for it and the key-code mapping are judged against the keys
 * EclipseActivity.controllerKeys reports, and the best is the one SDL uses, for that exact button set only.
 */

/* SDL 2.32.10 keycode_to_SDL (src/joystick/android/SDL_sysjoystick.c): the joystick button each key arrives as.
 * AKEYCODE_BUTTON_1..16 follow as b20..b35. */
static const struct {
    int keycode;
    int button;
} pad_keys[] = {
        {AKEYCODE_BUTTON_A,      0}, {AKEYCODE_DPAD_CENTER,   0}, {AKEYCODE_BUTTON_B,     1},
        {AKEYCODE_BUTTON_X,      2}, {AKEYCODE_BUTTON_Y,      3}, {AKEYCODE_BACK,         4},
        {AKEYCODE_BUTTON_SELECT, 4}, {AKEYCODE_BUTTON_MODE,   5}, {AKEYCODE_MENU,         6},
        {AKEYCODE_BUTTON_START,  6}, {AKEYCODE_BUTTON_THUMBL, 7}, {AKEYCODE_BUTTON_THUMBR, 8},
        {AKEYCODE_BUTTON_L1,     9}, {AKEYCODE_BUTTON_R1,     10}, {AKEYCODE_DPAD_UP,     11},
        {AKEYCODE_DPAD_DOWN,     12}, {AKEYCODE_DPAD_LEFT,    13}, {AKEYCODE_DPAD_RIGHT,  14},
        {AKEYCODE_BUTTON_L2,     15}, {AKEYCODE_BUTTON_R2,    16}, {AKEYCODE_BUTTON_C,    17},
        {AKEYCODE_BUTTON_Z,      18},
};
#define PAD_KEYS_COUNT ((int) (sizeof(pad_keys) / sizeof(pad_keys[0])))
#define PAD_NUMBERED_KEYS 16

/* The database's Android lines (pointers into db_text). */
static char *db_text;
static const char **db_lines;
static int db_count;

/* Per button set (GUID without its CRC): SDL's own mapping as it was before any of ours, and what we put in its
 * place (NULL while SDL's own is in force). */
#define FITTED_MAX 16
static struct {
    char guid[33];
    char *sdl_own;
    char *applied;
} fitted[FITTED_MAX];
static int fitted_count;

int app_input_load_gamepad_db(const char *path) {
    SDL_free(db_lines);
    SDL_free(db_text);
    db_lines = NULL;
    db_text = NULL;
    db_count = 0;
    SDL_RWops *rw = path != NULL ? SDL_RWFromFile(path, "r") : NULL;
    if (rw == NULL) {
        return 0;
    }
    const Sint64 size = SDL_RWsize(rw);
    db_text = size > 0 ? SDL_malloc((size_t) size + 1) : NULL;
    if (db_text == NULL || SDL_RWread(rw, db_text, (size_t) size, 1) != 1) {
        SDL_free(db_text);
        db_text = NULL;
        SDL_RWclose(rw);
        return 0;
    }
    SDL_RWclose(rw);
    db_text[size] = '\0';
    int lines = 1;
    for (const char *c = db_text; *c != '\0'; c++) {
        lines += *c == '\n';
    }
    db_lines = SDL_malloc(sizeof(*db_lines) * (size_t) lines);
    if (db_lines == NULL) {
        return 0;
    }
    /* Kept by platform exactly as SDL_GameControllerAddMappingsFromRW keeps them. */
    char platform_field[64];
    SDL_snprintf(platform_field, sizeof(platform_field), "platform:%s,", SDL_GetPlatform());
    for (char *line = db_text, *next; line != NULL && *line != '\0'; line = next) {
        next = SDL_strchr(line, '\n');
        if (next != NULL) {
            *next++ = '\0';
        }
        const size_t len = SDL_strlen(line);
        if (len > 0 && line[len - 1] == '\r') {
            line[len - 1] = '\0';
        }
        if (line[0] != '#' && SDL_strcasestr(line, platform_field) != NULL) {
            db_lines[db_count++] = line;
        }
    }
    commons_log_info("Input", "Controller database: %d %s mappings, chosen per controller as it connects", db_count,
                     SDL_GetPlatform());
    return db_count;
}

static bool sdl_own_by_version(const char *mapping) {
    return SDL_strstr(mapping, "sdk>=:") != NULL || SDL_strstr(mapping, "sdk<=:") != NULL;
}

static const char *source_name(controller_mapping_source_t source) {
    switch (source) {
        case CONTROLLER_MAPPING_SDL_BY_VERSION:
            return "SDL's for this Android version";
        case CONTROLLER_MAPPING_DATABASE:
            return "the database";
        case CONTROLLER_MAPPING_SDL:
            return "SDL's";
        default:
            return "the key codes";
    }
}

void app_input_fit_gamepad_mapping(int device_index) {
    SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(device_index);
    /* The CRC names the physical controller; a mapping is for every controller with these buttons. */
    guid.data[2] = guid.data[3] = 0;
    char guid_text[33];
    SDL_JoystickGetGUIDString(guid, guid_text, sizeof(guid_text));
    int slot = 0;
    while (slot < fitted_count && SDL_strcmp(fitted[slot].guid, guid_text) != 0) {
        slot++;
    }
    if (slot == fitted_count) {
        if (fitted_count == FITTED_MAX) {
            return;
        }
        fitted_count++;
        SDL_strlcpy(fitted[slot].guid, guid_text, sizeof(fitted[slot].guid));
        /* Before anything of ours: SDL's built-in mapping, or the one it makes from the key codes. */
        fitted[slot].sdl_own = SDL_GameControllerMappingForGUID(guid);
        fitted[slot].applied = NULL;
    }
    const int sdk = android_sdk_int();
    const char *name = SDL_JoystickNameForIndex(device_index);

    /* What this controller can press here: its key layout, or failing that the low 16 buttons in its GUID. */
    controller_buttons_t buttons = controller_mapping_guid_buttons(guid_text), known = 0xFFFF;
    int keycodes[PAD_KEYS_COUNT + PAD_NUMBERED_KEYS];
    bool has[PAD_KEYS_COUNT + PAD_NUMBERED_KEYS];
    for (int i = 0; i < PAD_KEYS_COUNT; i++) {
        keycodes[i] = pad_keys[i].keycode;
    }
    for (int i = 0; i < PAD_NUMBERED_KEYS; i++) {
        keycodes[PAD_KEYS_COUNT + i] = AKEYCODE_BUTTON_1 + i;
    }
    const bool from_layout = android_controller_keys(SDL_JoystickGetDeviceVendor(device_index),
                                                     SDL_JoystickGetDeviceProduct(device_index), name, keycodes,
                                                     PAD_KEYS_COUNT + PAD_NUMBERED_KEYS, has);
    if (from_layout) {
        known = ~(controller_buttons_t) 0;
        for (int i = 0; i < PAD_KEYS_COUNT + PAD_NUMBERED_KEYS; i++) {
            if (has[i]) {
                buttons |= (controller_buttons_t) 1 << (i < PAD_KEYS_COUNT ? pad_keys[i].button
                                                                           : 20 + (i - PAD_KEYS_COUNT));
            }
        }
    }

#define CANDIDATES_MAX 10
    controller_mapping_candidate_t candidates[CANDIDATES_MAX];
    char *owned[CANDIDATES_MAX] = {NULL};
    int count = 0;
    const char *sdl_own = fitted[slot].sdl_own;
    /* SDL names the mapping it makes from the key codes "*" (SDL_CreateMappingForAndroidController). */
    const bool sdl_own_generated = sdl_own != NULL && SDL_strncmp(sdl_own + 32, ",*,", 3) == 0;
    if (sdl_own != NULL) {
        candidates[count++] = (controller_mapping_candidate_t) {
                sdl_own, sdl_own_generated ? CONTROLLER_MAPPING_KEY_CODES
                                           : sdl_own_by_version(sdl_own) ? CONTROLLER_MAPPING_SDL_BY_VERSION
                                                                         : CONTROLLER_MAPPING_SDL};
    }
    /* Exact matches first, then those SDL would take with the button mask ignored; each is applied for this
     * controller's own GUID. */
    for (int exact = 2; exact >= 1; exact--) {
        for (int i = 0; i < db_count && count < CANDIDATES_MAX - 1; i++) {
            if (controller_mapping_guid_match(db_lines[i], guid_text) != exact ||
                !controller_mapping_conditions_hold(db_lines[i], sdk)) {
                continue;
            }
            char *line = SDL_strdup(db_lines[i]);
            if (line == NULL) {
                continue;
            }
            SDL_memcpy(line, guid_text, 32);
            owned[count] = line;
            candidates[count++] = (controller_mapping_candidate_t) {line, CONTROLLER_MAPPING_DATABASE};
        }
    }
    char generated[1024];
    if (!sdl_own_generated && controller_mapping_from_key_codes(generated, sizeof(generated), guid_text, sdk)) {
        candidates[count++] = (controller_mapping_candidate_t) {generated, CONTROLLER_MAPPING_KEY_CODES};
    }

    controller_mapping_fit_t fits[CANDIDATES_MAX];
    const int best = controller_mapping_choose(candidates, count, buttons, known, fits);
    if (best >= 0) {
        const char *chosen = candidates[best].mapping;
        const bool is_sdl_own = chosen == sdl_own;
        const bool in_force = is_sdl_own ? fitted[slot].applied == NULL
                                         : fitted[slot].applied != NULL && SDL_strcmp(fitted[slot].applied, chosen) == 0;
        if (!in_force) {
            if (SDL_GameControllerAddMapping(chosen) < 0) {
                commons_log_warn("Input", "Controller mapping not applied: %s", SDL_GetError());
            }
            SDL_free(fitted[slot].applied);
            fitted[slot].applied = is_sdl_own ? NULL : SDL_strdup(chosen);
        }
        char summary[512] = "";
        for (int i = 0; i < count; i++) {
            char item[64];
            SDL_snprintf(item, sizeof(item), "%s%s %d/%d", i > 0 ? ", " : "", source_name(candidates[i].source),
                         fits[i].dead, fits[i].unused);
            SDL_strlcat(summary, item, sizeof(summary));
        }
        commons_log_info("Input", "Controller %s (%s): mapping from %s; dead/unused bindings: %s; buttons %#llx "
                                  "from %s", name != NULL ? name : "?", guid_text,
                         source_name(candidates[best].source), summary, (unsigned long long) buttons,
                         from_layout ? "Android's key layout" : "the GUID only");
    }
    for (int i = 0; i < count; i++) {
        SDL_free(owned[i]);
    }
}

static void free_gamepad_db(void) {
    SDL_free(db_lines);
    SDL_free(db_text);
    db_lines = NULL;
    db_text = NULL;
    db_count = 0;
    for (int i = 0; i < fitted_count; i++) {
        SDL_free(fitted[i].sdl_own);
        SDL_free(fitted[i].applied);
    }
    fitted_count = 0;
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

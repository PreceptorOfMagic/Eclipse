#include "app_settings.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include <ini.h>
#include <SDL.h>

#if defined(TARGET_WEBOS)
#include <SDL_system.h>
#endif
#if defined(TARGET_ANDROID)
#include "platform/android/android_platform.h"
#endif

#include "util/ini_ext.h"
#include "util/nullable.h"
#include "util/path.h"
#include "util/i18n.h"

#include "ini_writer.h"

static bool known_pads_insert(app_settings_t *config, const char *name, bool front);

static bool coop_advanced_parse(app_settings_t *config, const char *name, const char *value);

static int find_ch_idx_by_config(int config);

static int find_ch_idx_by_value(const char *value);

static const char *serialize_audio_config(int config);

static int parse_audio_config(const char *value);

static int settings_parse(app_settings_t *config, const char *section, const char *name, const char *value);

static void set_string(char **field, const char *value);

static void set_int(int *field, const char *value);

static FILE *settings_open_read(const char *path);

void settings_sync_refresh_rate(app_settings_t *config) {
    if (config == NULL) {
        return;
    }

    /* This entry point is used immediately after the user commits an exact
     * fractional refresh rate.  The integer FPS sent alongside it must be the
     * same rounded value that the host derives from clientRefreshRateX100. */
    if (config->client_refresh_rate_x100 > 0) {
        config->stream.fps = (config->client_refresh_rate_x100 + 50) / 100;
        return;
    }

    settings_reconcile_refresh_rate(config);
}

void settings_reconcile_refresh_rate(app_settings_t *config) {
    if (!config) {
        return;
    }
#if defined(TARGET_WEBOS)
    int ntsc = settings_ntsc_refresh_rate_x100_for_fps(config->stream.fps);
    if (ntsc > 0) {
        config->client_refresh_rate_x100 = ntsc;
        return;
    }
#endif
    if (config->client_refresh_rate_x100 > 0) {
        int implied = (config->client_refresh_rate_x100 + 50) / 100;
        if (implied != config->stream.fps) {
            config->client_refresh_rate_x100 = 0;
#if !defined(TARGET_WEBOS)
        } else {
            config->stream.fps = implied;
        }
#else
        }
#endif
    }
}

int settings_ntsc_refresh_rate_x100_for_fps(int nominal_fps)
{
    /* 1000/1001 × nominal_fps, rounded to centihertz (Limelight clientRefreshRateX100). */
    switch (nominal_fps) {
    case 30:
        return 2997;
    case 60:
        return 5994;
    case 120:
        return 11988;
    case 240:
        return 23976;
    default:
        return 0;
    }
}

void settings_apply_ntsc_preset_refresh(app_settings_t *config, int nominal_fps)
{
#if defined(TARGET_WEBOS)
    (void) nominal_fps;
    settings_reconcile_refresh_rate(config);
#else
    (void) config;
    (void) nominal_fps;
#endif
}


const audio_config_entry_t audio_configs[] = {
        {AUDIO_CONFIGURATION_STEREO,      "stereo", translatable("Stereo")},
        {AUDIO_CONFIGURATION_51_SURROUND, "5.1ch",  translatable("5.1 Surround")},
        {AUDIO_CONFIGURATION_71_SURROUND, "7.1ch",  translatable("7.1 Surround")},
};
const size_t audio_config_len = sizeof(audio_configs) / sizeof(audio_config_entry_t);

void settings_initialize(app_settings_t *config, char *conf_dir) {
    memset(config, 0, sizeof(CONFIGURATION));
    LiInitializeStreamConfiguration(&config->stream);

    /* THE PANEL DECIDES THE DEFAULT, NOT A CONSTANT. 1280x720 here meant a client on a 4K
     * panel asked its host for 720p unless moonlight.ini happened to say otherwise -- and
     * co-op HALVES whatever this is into each pane, so every geometry derived downstream
     * started from a number nobody chose. Ask the panel, exactly as the settings screen
     * already does (settings.controller.c:166), and keep its 1080p fallback for when the
     * platform cannot answer.
     *
     * On webOS the panel can be asked right here. Everywhere else the display is SDL's to
     * report and its video subsystem is not up yet, so this leaves the fallback in place
     * and flags it: app_init() fills in the real size the moment video is initialised.
     * Without that the Windows client defaulted to 1080p on a 1440p monitor -- the same
     * hardcoded-resolution fault, just relocated. */
    int native_w = 1920, native_h = 1080;
#if defined(TARGET_WEBOS)
    if (!SDL_webOSGetPanelResolution(&native_w, &native_h) || native_w <= 0 || native_h <= 0) {
        native_w = 1920;
        native_h = 1080;
    }
    config->stream_size_defaulted = false;
#elif defined(TARGET_ANDROID)
    /* As on webOS, the panel itself: the display's physical mode, not the 1080p surface Android TV gives an
     * app (SDL would report that one). The activity answers this before SDL's video subsystem is up. */
    if (!android_display_mode(&native_w, &native_h, NULL, NULL, NULL) || native_w <= 0 || native_h <= 0) {
        native_w = 1920;
        native_h = 1080;
    }
    config->stream_size_defaulted = false;
#else
    config->stream_size_defaulted = true;
#endif
    config->stream.width = native_w;
    config->stream.height = native_h;
    config->stream.fps = 60;
    config->stream.bitrate = settings_optimal_bitrate(NULL, native_w, native_h, 60);
    config->stream_bitrate_defaulted = true;
    config->stream.packetSize = 1392;
    config->stream.streamingRemotely = STREAM_CFG_AUTO;
    config->stream.audioConfiguration = AUDIO_CONFIGURATION_STEREO;

    config->debug_level = 0;
    config->diag_logging = false;
    set_string(&config->language, "auto");
    set_string(&config->audio_backend, "auto");
    set_string(&config->decoder, "auto");
    set_string(&config->audio_device, "default");
    config->sops = true;
    config->localaudio = false;
    config->wired_pin_mode = WIRED_PIN_AUTO;
    config->host_monitor_mode = HOST_MONITOR_CHOOSE;
    config->nintendo_layout = -1;   /* resolved from swap_abxy once the file is read (settings_read) */
    config->fullscreen = true;
    /* Keep a valid restore rectangle even when the first launch is fullscreen. The
     * desktop setting can be toggled live; leaving fullscreen must never hand SDL
     * the zero-sized rectangle produced by the initial memset above. */
    config->window_state.x = SDL_WINDOWPOS_CENTERED;
    config->window_state.y = SDL_WINDOWPOS_CENTERED;
    config->window_state.w = 1280;
    config->window_state.h = 720;
    // TODO make this automatic
    config->unsupported = true;
    config->quitappafter = false;
    config->autoresume = false;
    config->pin_autoentry = false;
    config->pin_code = NULL;
    config->pin_predelay_ms = 4000;
    config->pin_dismiss = true;
    config->pin_dismiss_ms = 1500;
    config->pin_submit = true;
    config->pin_reconnect_delay_ms = 4000;
    config->pin_type_as_text = true;
    config->pin_hdr_relaunch = true;
    config->pin_relaunch_settle_ms = 3000;
    config->viewonly = false;
    config->rotate = 0;
    config->absmouse = true;
    config->virtual_mouse = false;
    config->hdr = false;
    config->force_full_color_range = false;
    config->hevc = true;
    config->idr_refresh_interval_sec = 0;
    config->av1 = false;
    config->show_stats_on_start = false;
    config->show_stats_compact = false;
    config->stick_deadzone = 7;
    config->client_refresh_rate_x100 = 0;
    /* Horizontal by default: the proven path, and the one that costs nothing extra. */
    config->coop_orientation = COOP_ORIENTATION_HORIZONTAL;
    /* Controller 1 -> player 1, controller 2 -> player 2, then repeat. A 2-player split
     * with 4 pads attached should still do something sensible without being configured. */
    for (int i = 0; i < 4; ++i) {
        config->coop_pad_pane[i] = (i % 2) ? COOP_PAD_SECONDARY : COOP_PAD_PRIMARY;
    }
    config->coop_audio_mode = COOP_AUDIO_MIX;
    config->coop_audio_filter = COOP_AUDIO_FILTER_ACCURATE;
    config->coop_audio_filter_ahead_ms = 40;
    /* AUTO: the pairing decides. A default of CORE would break every mixed pair on
     * first launch, and a default of COMPAT would quietly cost HEVC and HDR on the
     * same-vendor pairs that are the common case. */
    config->coop_stream_mode = COOP_STREAM_AUTO;
    config->coop_encoder = COOP_ENCODER_AUTO;
    config->coop_host_tuning = true;
    /* No pair chosen until the user launches one; the popup falls back to the
     * currently selected PC plus the first other paired one. */
    set_string(&config->coop_default_primary_uuid, "");
    set_string(&config->coop_default_secondary_uuid, "");
    set_string(&config->coop_primary_uuid, "");
    set_string(&config->coop_secondary_uuid, "");
    config->coop_app_id = COOP_APP_ID_DEFAULT;
    config->known_pads = NULL;
    config->known_pads_count = 0;
    settings_coop_advanced_defaults(config);
    config->coop_split_orientations = false;
    config->coop_kbm_assign = false;

#if defined(TARGET_WEBOS)
    settings_apply_ntsc_preset_refresh(config, config->stream.fps);
#endif

    config->conf_dir = conf_dir;
    config->settings_save_allowed = true;
    config->ini_path = path_join(conf_dir, CONF_NAME_MOONLIGHT);
    config->condb_path = path_join(conf_dir, "gamecontrollerdb.txt");
    config->key_dir = path_join(conf_dir, "key");
}

bool settings_read(app_settings_t *config) {
    if (config == NULL || config->ini_path == NULL) {
        return false;
    }
    errno = 0;
    FILE *fp = settings_open_read(config->ini_path);
    int ret = -1;
    if (fp != NULL) {
        ret = ini_parse_file(fp, (ini_handler) settings_parse, config);
        if (fclose(fp) != 0) {
            ret = -1;
        }
        config->settings_save_allowed = ret == 0;
    } else {
        /* ENOENT is first launch and is safe to create later. Any other error
         * means a user file may exist but be inaccessible; preserve it. */
        config->settings_save_allowed = errno == ENOENT;
    }
    if (ret == 0) {
        settings_reconcile_refresh_rate(config);
        settings_refresh_auto_bitrate(config, NULL);
    }
    if (config->nintendo_layout < 0) {
        /* First run with this setting: keep what Nintendo pads already did (see app_settings.h). */
        config->nintendo_layout = config->swap_abxy ? NINTENDO_LAYOUT_XBOX : NINTENDO_LAYOUT_AS_PRINTED;
    }
    return ret == 0;
}

static char *settings_staging_path(const char *path) {
    const size_t capacity = strlen(path) + 40;
    char *staging = malloc(capacity);
    if (staging == NULL) {
        return NULL;
    }
#if defined(_WIN32)
    const unsigned long process_id = GetCurrentProcessId();
#else
    const unsigned long process_id = (unsigned long) getpid();
#endif
    snprintf(staging, capacity, "%s.tmp.%lu", path, process_id);
    return staging;
}

static FILE *settings_open_read(const char *path) {
    return path_fopen(path, "r");
}

static FILE *settings_open_staging(const char *path) {
    /* BINARY: the ini writer ends every line with "\r\n" itself, and a Windows text-mode file turned each "\n" into
     * "\r\n" again -- every line of moonlight.ini ended "\r\r\n" (all 99 lines on VEGA, 2026-09-30; Notepad shows a
     * blank line between settings). Same bytes as before on Linux and webOS. */
    return path_fopen(path, "wb");
}

static bool settings_replace_file(const char *staging, const char *target) {
    return path_file_replace(staging, target);
}

static void settings_remove_staging(const char *path) {
    (void) path_file_remove(path);
}

bool settings_save(app_settings_t *config) {
    if (config->ini_path == NULL || !config->settings_save_allowed) {
        return false;
    }
    settings_reconcile_refresh_rate(config);
    char *staging_path = settings_staging_path(config->ini_path);
    if (staging_path == NULL) {
        return false;
    }
    FILE *fp = settings_open_staging(staging_path);
    if (!fp) {
        free(staging_path);
        return false;
    }
    ini_write_string(fp, "language", config->language);
    ini_write_bool(fp, "fullscreen", config->fullscreen);
    ini_write_int(fp, "debug_level", config->debug_level);
    ini_write_bool(fp, "diag_logging", config->diag_logging);

    ini_write_section(fp, "streaming");
    /* Only persist a size the USER chose. The default is derived from the panel/monitor at
     * startup, and writing that back would turn one machine's display into a permanent
     * setting: settings_parse() clears stream_size_defaulted the moment it reads a width,
     * so the next launch would stop deriving. That matters because this config is meant to
     * survive reinstalls and follow the user -- a new monitor, or the same file on another
     * box, would keep streaming at the old resolution with nothing in the UI to explain it. */
    if (!config->stream_size_defaulted) {
        ini_write_int(fp, "width", config->stream.width);
        ini_write_int(fp, "height", config->stream.height);
    }
    ini_write_int(fp, "net_fps", config->stream.fps);
    if (!config->stream_bitrate_defaulted) {
        ini_write_int(fp, "bitrate", config->stream.bitrate);
    }
    ini_write_int(fp, "packetsize", config->stream.packetSize);
    ini_write_int(fp, "rotate", config->rotate);

    ini_write_section(fp, "host");
    ini_write_bool(fp, "sops", config->sops);
    ini_write_bool(fp, "localaudio", config->localaudio);
    ini_write_int(fp, "wired_pin_mode", config->wired_pin_mode);
    ini_write_int(fp, "host_monitor_mode", config->host_monitor_mode);
    ini_write_bool(fp, "quitappafter", config->quitappafter);
    ini_write_bool(fp, "autoresume", config->autoresume);
    ini_write_bool(fp, "viewonly", config->viewonly);

    ini_write_section(fp, "wake");
    ini_write_bool(fp, "pin_autoentry", config->pin_autoentry);
    if (config->pin_code != NULL) {
        ini_write_string(fp, "pin_code", config->pin_code);
    }
    ini_write_int(fp, "pin_predelay_ms", config->pin_predelay_ms);
    ini_write_bool(fp, "pin_dismiss", config->pin_dismiss);
    ini_write_int(fp, "pin_dismiss_ms", config->pin_dismiss_ms);
    ini_write_bool(fp, "pin_submit", config->pin_submit);
    ini_write_int(fp, "pin_reconnect_delay_ms", config->pin_reconnect_delay_ms);
    ini_write_bool(fp, "pin_type_as_text", config->pin_type_as_text);
    ini_write_bool(fp, "pin_hdr_relaunch", config->pin_hdr_relaunch);
    ini_write_int(fp, "pin_relaunch_settle_ms", config->pin_relaunch_settle_ms);

    ini_write_section(fp, "input");
    ini_write_bool(fp, "absmouse", config->absmouse);
    ini_write_bool(fp, "virtual_mouse", config->virtual_mouse);
#if FEATURE_INPUT_EVMOUSE
    ini_write_bool(fp, "hardware_mouse", config->hardware_mouse);
#endif
    ini_write_bool(fp, "swap_abxy", config->swap_abxy);
    ini_write_int(fp, "nintendo_layout", config->nintendo_layout);
    ini_write_int(fp, "stick_deadzone", config->stick_deadzone);
    ini_write_bool(fp, "syskey_capture", config->syskey_capture);
    ini_write_int(fp, "coop_orientation", config->coop_orientation);
    ini_write_int(fp, "coop_pad1_pane", config->coop_pad_pane[0]);
    ini_write_int(fp, "coop_pad2_pane", config->coop_pad_pane[1]);
    ini_write_int(fp, "coop_pad3_pane", config->coop_pad_pane[2]);
    ini_write_int(fp, "coop_pad4_pane", config->coop_pad_pane[3]);
    ini_write_int(fp, "coop_audio_mode", config->coop_audio_mode);
    ini_write_bool(fp, "coop_audio_dedupe", config->coop_audio_filter != COOP_AUDIO_FILTER_OFF);   /* older builds */
    ini_write_string(fp, "coop_audio_filter", config->coop_audio_filter == COOP_AUDIO_FILTER_OFF ? "off"
                     : config->coop_audio_filter == COOP_AUDIO_FILTER_FAST ? "fast" : "accurate");
    ini_write_int(fp, "coop_audio_filter_ahead_ms", config->coop_audio_filter_ahead_ms);
    ini_write_int(fp, "coop_stream_mode", config->coop_stream_mode);
    ini_write_int(fp, "coop_encoder", config->coop_encoder);
    ini_write_bool(fp, "coop_host_tuning", config->coop_host_tuning);
    ini_write_string(fp, "coop_default_primary_uuid", config->coop_default_primary_uuid);
    ini_write_string(fp, "coop_default_secondary_uuid", config->coop_default_secondary_uuid);
    ini_write_string(fp, "coop_primary_uuid", config->coop_primary_uuid);
    ini_write_string(fp, "coop_secondary_uuid", config->coop_secondary_uuid);
    ini_write_int(fp, "coop_app_id", config->coop_app_id);
    /* One key per orientation, suffixed _h / _v. A plain `coop_slices` from an older
     * conf is still read, into BOTH columns -- see the parser. */
    ini_write_bool(fp, "coop_split_orientations", config->coop_split_orientations);
    ini_write_bool(fp, "coop_kbm_assign", config->coop_kbm_assign);
    for (int o = 0; o < 2; ++o) {
        const char *sfx = o == COOP_ORIENTATION_VERTICAL ? "_v" : "_h";
        char key[64];
#define COOP_WRITE(field, writer) \
        snprintf(key, sizeof(key), "coop_" #field "%s", sfx); \
        writer(fp, key, config->coop_##field[o])
        COOP_WRITE(fps_cap, ini_write_int);
        COOP_WRITE(fps_peer, ini_write_int);
        COOP_WRITE(slices, ini_write_int);
        COOP_WRITE(queue_depth, ini_write_int);
        COOP_WRITE(audio_depth_ms, ini_write_int);
        COOP_WRITE(bitrate, ini_write_int);
        COOP_WRITE(fps, ini_write_int);
        COOP_WRITE(height, ini_write_int);
        COOP_WRITE(skip_conceal, ini_write_bool);
        COOP_WRITE(catchup, ini_write_bool);
        COOP_WRITE(repeat_conceal, ini_write_bool);
        COOP_WRITE(repeat_pane, ini_write_bool);
        COOP_WRITE(measure, ini_write_bool);
        COOP_WRITE(pump, ini_write_bool);
        COOP_WRITE(self_mirror, ini_write_bool);
#undef COOP_WRITE
    }
    /* Repeated key, in list order, so the file itself stays the most-recent-first list
     * the settings screen shows. */
    for (size_t i = 0; i < config->known_pads_count; ++i) {
        ini_write_string(fp, "known_pad", config->known_pads[i]);
    }

    ini_write_section(fp, "video");
    ini_write_string(fp, "decoder", config->decoder);
    ini_write_bool(fp, "hdr", config->hdr);
    ini_write_bool(fp, "force_full_color_range", config->force_full_color_range);
    ini_write_bool(fp, "hevc", config->hevc);
    ini_write_int(fp, "idr_refresh_interval_sec", config->idr_refresh_interval_sec);
    ini_write_bool(fp, "av1", config->av1);
    ini_write_bool(fp, "show_stats_on_start", config->show_stats_on_start);
    ini_write_bool(fp, "show_stats_compact", config->show_stats_compact);
    ini_write_int(fp, "client_refresh_rate_x100", config->client_refresh_rate_x100);

    ini_write_section(fp, "audio");
    ini_write_string(fp, "backend", config->audio_backend);
    if (config->audio_device && config->audio_device[0]) {
        ini_write_string(fp, "device", config->audio_device);
    }
    ini_write_string(fp, "surround", serialize_audio_config(config->stream.audioConfiguration));

    /* Preserve the windowed restore rectangle even while the UI is fullscreen.
     * Otherwise saving fullscreen=true removes the only record of which monitor
     * the user selected, and leaving fullscreen after the next launch restores to
     * a desktop-sized or off-screen rectangle. */
    ini_write_section(fp, "window");
    ini_write_int(fp, "x", config->window_state.x);
    ini_write_int(fp, "y", config->window_state.y);
    ini_write_int(fp, "width", config->window_state.w);
    ini_write_int(fp, "height", config->window_state.h);
    ini_write_bool(fp, "maximized", config->window_state.maximized);

    bool success = ferror(fp) == 0;
    if (success && fflush(fp) != 0) {
        success = false;
    }
#if defined(_WIN32)
    if (success && _commit(_fileno(fp)) != 0) {
        success = false;
    }
#else
    if (success && fsync(fileno(fp)) != 0) {
        success = false;
    }
#endif
    if (fclose(fp) != 0) {
        success = false;
    }
    if (success) {
        success = settings_replace_file(staging_path, config->ini_path);
    }
    if (!success) {
        settings_remove_staging(staging_path);
    }
    free(staging_path);
    return success;
}

void settings_clear(app_settings_t *config) {
    free_nullable(config->decoder);
    free_nullable(config->audio_backend);
    free_nullable(config->audio_device);
    free_nullable(config->language);
    free_nullable(config->coop_default_primary_uuid);
    free_nullable(config->coop_default_secondary_uuid);
    free_nullable(config->coop_primary_uuid);
    free_nullable(config->coop_secondary_uuid);
    for (size_t i = 0; i < config->known_pads_count; ++i) {
        free_nullable(config->known_pads[i]);
    }
    free_nullable(config->known_pads);
    config->known_pads = NULL;
    config->known_pads_count = 0;
    free_nullable(config->ini_path);
    free_nullable(config->condb_path);
    free_nullable(config->key_dir);
    free_nullable(config->conf_dir);
    config->conf_dir = NULL;
}

/**
 * Read one per-orientation dial.
 *
 * `coop_slices_h` and `coop_slices_v` are the two columns; a BARE `coop_slices` is what
 * a conf written before the split contains, and it lands in BOTH -- that setting applied
 * to whatever orientation was run, so both columns is the only reading that preserves
 * what the user had.
 *
 * @return true when @p name was one of these keys.
 */
static bool coop_advanced_parse(app_settings_t *config, const char *name, const char *value) {
    static const char prefix[] = "coop_";
    if (strncmp(name, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    const char *field = name + sizeof(prefix) - 1;
    const size_t len = strlen(field);
    int only = -1;                                   /* -1 = both columns */
    size_t base = len;
    if (len > 2 && field[len - 2] == '_') {
        if (field[len - 1] == 'h') {
            only = COOP_ORIENTATION_HORIZONTAL;
            base = len - 2;
        } else if (field[len - 1] == 'v') {
            only = COOP_ORIENTATION_VERTICAL;
            base = len - 2;
        }
    }

#define COOP_READ_INT(f) \
    if (base == strlen(#f) && strncmp(field, #f, base) == 0) { \
        for (int o = 0; o < 2; ++o) { \
            if (only < 0 || only == o) { config->coop_##f[o] = (int) strtol(value, NULL, 10); } \
        } \
        return true; \
    }
#define COOP_READ_BOOL(f) \
    if (base == strlen(#f) && strncmp(field, #f, base) == 0) { \
        for (int o = 0; o < 2; ++o) { \
            if (only < 0 || only == o) { config->coop_##f[o] = INI_IS_TRUE(value); } \
        } \
        return true; \
    }
    COOP_READ_INT(fps_cap)
    COOP_READ_INT(fps_peer)
    COOP_READ_INT(slices)
    COOP_READ_INT(queue_depth)
    COOP_READ_INT(audio_depth_ms)
    COOP_READ_INT(bitrate)
    COOP_READ_INT(fps)
    COOP_READ_INT(height)
    COOP_READ_BOOL(skip_conceal)
    COOP_READ_BOOL(catchup)
    COOP_READ_BOOL(repeat_conceal)
    COOP_READ_BOOL(repeat_pane)
    COOP_READ_BOOL(measure)
    COOP_READ_BOOL(pump)
    COOP_READ_BOOL(self_mirror)
#undef COOP_READ_INT
#undef COOP_READ_BOOL
    return false;
}

static int known_pad_index(const app_settings_t *config, const char *name) {
    for (size_t i = 0; i < config->known_pads_count; ++i) {
        if (strcmp(config->known_pads[i], name) == 0) {
            return (int) i;
        }
    }
    return -1;
}

/**
 * Add @p name to the seen-controllers list, at the front when it has just been seen and
 * at the back when the conf file is being replayed (see the parser).
 *
 * @return true when the stored list changed.
 */
static bool known_pads_insert(app_settings_t *config, const char *name, bool front) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    const int at = known_pad_index(config, name);
    if (at >= 0) {
        if (!front || at == 0) {
            return false;
        }
        char *moved = config->known_pads[at];
        memmove(&config->known_pads[1], &config->known_pads[0], (size_t) at * sizeof(char *));
        config->known_pads[0] = moved;
        return true;
    }
    if (config->known_pads_count >= KNOWN_PADS_MAX) {
        /* Full. Only a pad that has ACTUALLY just been seen is worth evicting the
         * oldest for; replaying the file must never push out what it just read. */
        if (!front) {
            return false;
        }
        free_nullable(config->known_pads[config->known_pads_count - 1]);
        config->known_pads_count--;
    }
    char **grown = realloc(config->known_pads, (config->known_pads_count + 1) * sizeof(char *));
    if (grown == NULL) {
        return false;
    }
    config->known_pads = grown;
    char *copy = strdup(name);
    if (copy == NULL) {
        return false;
    }
    if (front) {
        memmove(&grown[1], &grown[0], config->known_pads_count * sizeof(char *));
        grown[0] = copy;
    } else {
        grown[config->known_pads_count] = copy;
    }
    config->known_pads_count++;
    return true;
}

bool settings_remember_pad(app_settings_t *config, const char *name) {
    if (config == NULL) {
        return false;
    }
    return known_pads_insert(config, name, true);
}

bool settings_forget_pad(app_settings_t *config, const char *name) {
    if (config == NULL || name == NULL) {
        return false;
    }
    const int at = known_pad_index(config, name);
    if (at < 0) {
        return false;
    }
    free_nullable(config->known_pads[at]);
    memmove(&config->known_pads[at], &config->known_pads[at + 1],
            (config->known_pads_count - (size_t) at - 1) * sizeof(char *));
    config->known_pads_count--;
    return true;
}

/**
 * THE DEFAULTS ARE THE MEASURED STATE, not "everything off": skip-conceal and catch-up
 * are what make two free-running hosts spliceable at all, and both are on unless their
 * sentinel says otherwise (session_video.c). Every numeric dial is 0 = "leave the
 * built-in alone", so a fresh conf changes nothing at all. Both orientations get the
 * same values, which is what makes the two columns start out identical.
 */
void settings_coop_advanced_defaults(app_settings_t *config) {
    for (int o = 0; o < 2; ++o) {
        config->coop_fps_cap[o] = 0;
        config->coop_fps_peer[o] = 0;
        config->coop_slices[o] = 0;
        config->coop_queue_depth[o] = 0;
        config->coop_audio_depth_ms[o] = 0;
        config->coop_bitrate[o] = 0;
        config->coop_fps[o] = 0;
        config->coop_height[o] = 0;
        config->coop_skip_conceal[o] = true;
        config->coop_catchup[o] = true;
        config->coop_repeat_conceal[o] = false;
        config->coop_repeat_pane[o] = false;
        config->coop_measure[o] = false;
        config->coop_pump[o] = false;
        config->coop_self_mirror[o] = false;
        /* ONE SLICE PER HOST is the measured configuration for both modes
         * (docs/coop-modes.md); the built-in 4 is the single-host default and is wrong
         * for a splice. Shipping 0 = "built-in" here would have quietly run every co-op
         * session at 4. */
        config->coop_slices[o] = 1;
    }
}

int settings_optimal_bitrate(const SS4S_VideoCapabilities *capabilities, int w, int h, int fps) {
    /* Match Moonlight Qt's established default curve. Keeping this policy here, below the
     * platform-specific display lookup, lets Windows and Linux share it without either UI
     * owning a second table. Explicit slider/INI values never pass through this function. */
    static const struct {
        int64_t pixels;
        int factor;
    } resolution_table[] = {
            {640LL * 360, 1},
            {854LL * 480, 2},
            {1280LL * 720, 5},
            {1920LL * 1080, 10},
            {2560LL * 1440, 20},
            {3840LL * 2160, 40},
    };
    (void) capabilities;

    if (w <= 0 || h <= 0) {
        w = 1920;
        h = 1080;
    }
    if (fps <= 0) {
        fps = 60;
    }

    const int64_t pixels = (int64_t) w * h;
    double resolution_factor = resolution_table[0].factor;
    const size_t entries = sizeof(resolution_table) / sizeof(resolution_table[0]);
    for (size_t i = 0; i < entries; ++i) {
        if (pixels == resolution_table[i].pixels) {
            resolution_factor = resolution_table[i].factor;
            break;
        }
        if (pixels < resolution_table[i].pixels) {
            if (i > 0) {
                const int64_t low_pixels = resolution_table[i - 1].pixels;
                const int low_factor = resolution_table[i - 1].factor;
                resolution_factor = low_factor +
                        (double) (pixels - low_pixels) /
                        (double) (resolution_table[i].pixels - low_pixels) *
                        (resolution_table[i].factor - low_factor);
            }
            break;
        }
        if (i + 1 == entries) {
            resolution_factor = resolution_table[i].factor;
        }
    }

    /* Above 60 FPS the established curve grows by sqrt(FPS), not linearly. */
    const double frame_rate_factor =
            (fps <= 60 ? fps : SDL_sqrt((double) fps / 60.0) * 60.0) / 30.0;
    return (int) (resolution_factor * frame_rate_factor + 0.5) * 1000;
}

void settings_refresh_auto_bitrate(app_settings_t *config,
                                   const SS4S_VideoCapabilities *capabilities) {
    if (config == NULL || !config->stream_bitrate_defaulted) {
        return;
    }
    config->stream.bitrate = settings_optimal_bitrate(capabilities,
                                                      config->stream.width,
                                                      config->stream.height,
                                                      config->stream.fps);
}


bool audio_config_valid(int config) {
    return find_ch_idx_by_config(config) >= 0;
}

const char *serialize_audio_config(int config) {
    return audio_configs[find_ch_idx_by_config(config)].value;
}

int parse_audio_config(const char *value) {
    int index = value ? find_ch_idx_by_value(value) : -1;
    if (index < 0) {
        index = 0;
    }
    return audio_configs[index].configuration;
}

int find_ch_idx_by_config(int config) {
    for (int i = 0; i < audio_config_len; i++) {
        if (audio_configs[i].configuration == config) {
            return i;
        }
    }
    return -1;
}

int find_ch_idx_by_value(const char *value) {
    for (int i = 0; i < audio_config_len; i++) {
        if (strcmp(audio_configs[i].value, value) == 0) {
            return i;
        }
    }
    return -1;
}

static int settings_parse(app_settings_t *config, const char *section, const char *name, const char *value) {
    if (INI_FULL_MATCH("streaming", "width")) {
        set_int(&config->stream.width, value);
        config->stream_size_defaulted = false;
    } else if (INI_FULL_MATCH("streaming", "height")) {
        set_int(&config->stream.height, value);
        config->stream_size_defaulted = false;
    } else if (INI_FULL_MATCH("streaming", "net_fps")) {
        set_int(&config->stream.fps, value);
    } else if (INI_FULL_MATCH("streaming", "bitrate")) {
        set_int(&config->stream.bitrate, value);
        /* Positive values are user-owned. Zero/negative values from an old or damaged
         * config fall back to the automatic curve after the whole file is parsed. */
        config->stream_bitrate_defaulted = config->stream.bitrate <= 0;
    } else if (INI_FULL_MATCH("streaming", "packetsize")) {
        set_int(&config->stream.packetSize, value);
    } else if (INI_FULL_MATCH("streaming", "rotate")) {
        set_int(&config->rotate, value);
    } else if (INI_FULL_MATCH("video", "idr_refresh_interval_sec")) {
        set_int(&config->idr_refresh_interval_sec, value);
        if (config->idr_refresh_interval_sec < 0) {
            config->idr_refresh_interval_sec = 0;
        } else if (config->idr_refresh_interval_sec > 60) {
            config->idr_refresh_interval_sec = 60;
        } else if (config->idr_refresh_interval_sec == 1) {
            config->idr_refresh_interval_sec = 2;
        }
    } else if (INI_NAME_MATCH("hevc")) {
        config->hevc = INI_IS_TRUE(value);
    } else if (INI_FULL_MATCH("video", "av1")) {
        config->av1 = INI_IS_TRUE(value);
    } else if (INI_FULL_MATCH("video", "video_simple_sdp")) {
        /* Legacy: ignored; client always negotiates RFI + slices when applicable. */
    } else if (INI_FULL_MATCH("video", "presentation_offset_ms")) {
        /* Legacy: ignored; tight sync uses a fixed presentation offset in the decoder. */
    } else if (INI_NAME_MATCH("show_stats_on_start")) {
        config->show_stats_on_start = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("show_stats_compact")) {
        config->show_stats_compact = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("hdr")) {
        config->hdr = INI_IS_TRUE(value);
    } else if (INI_FULL_MATCH("video", "client_refresh_rate_x100")) {
        set_int(&config->client_refresh_rate_x100, value);
        if (config->client_refresh_rate_x100 < 0) {
            config->client_refresh_rate_x100 = 0;
        } else if (config->client_refresh_rate_x100 > 24000) {
            config->client_refresh_rate_x100 = 24000;
        }
    } else if (INI_FULL_MATCH("video", "force_full_color_range")) {
        config->force_full_color_range = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("surround")) {
        config->stream.audioConfiguration = parse_audio_config(value);
    } else if (INI_NAME_MATCH("sops")) {
        config->sops = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("wired_pin_mode")) {
        const int mode = atoi(value);
        config->wired_pin_mode = mode >= WIRED_PIN_AUTO && mode <= WIRED_PIN_ON ? mode : WIRED_PIN_AUTO;
    } else if (INI_NAME_MATCH("host_monitor_mode")) {
        const int mode = atoi(value);
        config->host_monitor_mode = mode >= HOST_MONITOR_CHOOSE && mode <= HOST_MONITOR_ON ? mode : HOST_MONITOR_CHOOSE;
    } else if (INI_NAME_MATCH("localaudio")) {
        config->localaudio = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("quitappafter")) {
        config->quitappafter = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("autoresume")) {
        config->autoresume = INI_IS_TRUE(value);
    } else if (INI_FULL_MATCH("wake", "pin_autoentry")) {
        config->pin_autoentry = INI_IS_TRUE(value);
    } else if (INI_FULL_MATCH("wake", "pin_code")) {
        set_string(&config->pin_code, value);
    } else if (INI_FULL_MATCH("wake", "pin_predelay_ms")) {
        set_int(&config->pin_predelay_ms, value);
    } else if (INI_FULL_MATCH("wake", "pin_dismiss")) {
        config->pin_dismiss = INI_IS_TRUE(value);
    } else if (INI_FULL_MATCH("wake", "pin_dismiss_ms")) {
        set_int(&config->pin_dismiss_ms, value);
    } else if (INI_FULL_MATCH("wake", "pin_submit")) {
        config->pin_submit = INI_IS_TRUE(value);
    } else if (INI_FULL_MATCH("wake", "pin_reconnect_delay_ms")) {
        set_int(&config->pin_reconnect_delay_ms, value);
    } else if (INI_FULL_MATCH("wake", "pin_type_as_text")) {
        config->pin_type_as_text = INI_IS_TRUE(value);
    } else if (INI_FULL_MATCH("wake", "pin_hdr_relaunch")) {
        config->pin_hdr_relaunch = INI_IS_TRUE(value);
    } else if (INI_FULL_MATCH("wake", "pin_relaunch_settle_ms")) {
        set_int(&config->pin_relaunch_settle_ms, value);
    } else if (INI_NAME_MATCH("viewonly")) {
        config->viewonly = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("absmouse")) {
        config->absmouse = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("virtual_mouse")) {
        config->virtual_mouse = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("hardware_mouse")) {
#if FEATURE_INPUT_EVMOUSE
        config->hardware_mouse = INI_IS_TRUE(value);
#endif
    } else if (INI_NAME_MATCH("stick_deadzone")) {
        set_int(&config->stick_deadzone, value);
        if (config->stick_deadzone < 0) {
            config->stick_deadzone = 0;
        } else if (config->stick_deadzone > 100) {
            config->stick_deadzone = 100;
        }
    } else if (INI_NAME_MATCH("swap_abxy")) {
        config->swap_abxy = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("nintendo_layout")) {
        const int layout = atoi(value);
        config->nintendo_layout = layout >= NINTENDO_LAYOUT_OFF && layout <= NINTENDO_LAYOUT_XBOX
                                  ? layout : NINTENDO_LAYOUT_AS_PRINTED;
    } else if (INI_NAME_MATCH("syskey_capture")) {
        config->syskey_capture = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("coop_orientation")) {
        set_int(&config->coop_orientation, value);
        if (config->coop_orientation != COOP_ORIENTATION_VERTICAL) {
            config->coop_orientation = COOP_ORIENTATION_HORIZONTAL;
        }
    } else if (INI_NAME_MATCH("coop_pad1_pane") || INI_NAME_MATCH("coop_pad2_pane") ||
               INI_NAME_MATCH("coop_pad3_pane") || INI_NAME_MATCH("coop_pad4_pane")) {
        const int idx = name[8] - '1';          /* "coop_padN_pane" */
        if (idx >= 0 && idx < 4) {
            set_int(&config->coop_pad_pane[idx], value);
            /* A CONTROLLER GOES TO ONE PC. BOTH (2) was offered by the co-op dialog until
             * the user asked for it to go (2026-09-26); a conf saved before then may still
             * carry it, and coop_input would still send that pad to both hosts. It falls back
             * to PRIMARY like any other value outside the two routes, and the next save
             * writes the corrected value. */
            if (config->coop_pad_pane[idx] != COOP_PAD_PRIMARY &&
                config->coop_pad_pane[idx] != COOP_PAD_SECONDARY) {
                config->coop_pad_pane[idx] = COOP_PAD_PRIMARY;
            }
        }
    } else if (INI_NAME_MATCH("coop_audio_mode")) {
        set_int(&config->coop_audio_mode, value);
        if (config->coop_audio_mode < COOP_AUDIO_MIX || config->coop_audio_mode > COOP_AUDIO_SECONDARY_ONLY) {
            config->coop_audio_mode = COOP_AUDIO_MIX;
        }
    } else if (INI_NAME_MATCH("coop_audio_dedupe")) {
        /* The older on/off setting. Off still means off; on leaves the filter choice alone (default Accurate, or what
         * coop_audio_filter says -- this build writes both keys consistently, so their order does not matter). */
        if (!INI_IS_TRUE(value)) {
            config->coop_audio_filter = COOP_AUDIO_FILTER_OFF;
        }
    } else if (INI_NAME_MATCH("coop_audio_filter_ahead_ms")) {
        set_int(&config->coop_audio_filter_ahead_ms, value);
        int ms = (config->coop_audio_filter_ahead_ms + 2) / 5 * 5;
        config->coop_audio_filter_ahead_ms = ms < 0 ? 0 : ms > 100 ? 100 : ms;
    } else if (INI_NAME_MATCH("coop_audio_filter")) {
        if (strcmp(value, "off") == 0 || strcmp(value, "0") == 0) {
            config->coop_audio_filter = COOP_AUDIO_FILTER_OFF;
        } else if (strcmp(value, "fast") == 0 || strcmp(value, "1") == 0) {
            config->coop_audio_filter = COOP_AUDIO_FILTER_FAST;
        } else {
            config->coop_audio_filter = COOP_AUDIO_FILTER_ACCURATE;
        }
    } else if (INI_NAME_MATCH("coop_stream_mode")) {
        /* Retired stream-mode choices migrate to automatic HEVC GPU routing. */
        config->coop_stream_mode = COOP_STREAM_AUTO;
    } else if (INI_NAME_MATCH("coop_encoder")) {
        set_int(&config->coop_encoder, value);
        if (config->coop_encoder < COOP_ENCODER_AUTO || config->coop_encoder > COOP_ENCODER_GPU_HEVC) {
            config->coop_encoder = COOP_ENCODER_AUTO;
        }
    } else if (INI_NAME_MATCH("coop_host_tuning")) {
        config->coop_host_tuning = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("coop_default_primary_uuid")) {
        set_string(&config->coop_default_primary_uuid, value);
    } else if (INI_NAME_MATCH("coop_default_secondary_uuid")) {
        set_string(&config->coop_default_secondary_uuid, value);
    } else if (INI_NAME_MATCH("coop_primary_uuid")) {
        set_string(&config->coop_primary_uuid, value);
    } else if (INI_NAME_MATCH("coop_secondary_uuid")) {
        set_string(&config->coop_secondary_uuid, value);
    } else if (INI_NAME_MATCH("known_pad")) {
        /* APPEND while loading. The file is written most-recent-first, and pushing each
         * line to the front would hand the list back reversed -- the oldest pad would
         * end up at the top of the settings screen and be the first to be evicted. */
        known_pads_insert(config, value, false);
    } else if (INI_NAME_MATCH("coop_split_orientations")) {
        config->coop_split_orientations = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("coop_kbm_assign")) {
        config->coop_kbm_assign = INI_IS_TRUE(value);
    } else if (coop_advanced_parse(config, name, value)) {
        /* handled: one of the per-orientation dials */
    } else if (INI_NAME_MATCH("coop_app_id")) {
        set_int(&config->coop_app_id, value);
        /* A zero or negative id would spawn a secondary with nothing to launch, which
         * looks exactly like a host that never came up. Fall back rather than allow it. */
        if (config->coop_app_id <= 0) {
            config->coop_app_id = COOP_APP_ID_DEFAULT;
        }
    } else if (INI_FULL_MATCH("video", "decoder")) {
        set_string(&config->decoder, value);
    } else if (INI_FULL_MATCH("audio", "backend")) {
        set_string(&config->audio_backend, value);
    } else if (INI_FULL_MATCH("audio", "device")) {
        set_string(&config->audio_device, value);
    } else if (INI_NAME_MATCH("language")) {
        set_string(&config->language, value);
    } else if (INI_NAME_MATCH("fullscreen")) {
#if FEATURE_FORCE_FULLSCREEN
        config->fullscreen = true;
#else
        config->fullscreen = INI_IS_TRUE(value);
#endif
    } else if (INI_NAME_MATCH("debug_level")) {
        set_int(&config->debug_level, value);
    } else if (INI_NAME_MATCH("diag_logging")) {
        config->diag_logging = INI_IS_TRUE(value);
    } else if (INI_FULL_MATCH("window", "x")) {
        set_int(&config->window_state.x, value);
    } else if (INI_FULL_MATCH("window", "y")) {
        set_int(&config->window_state.y, value);
    } else if (INI_FULL_MATCH("window", "width")) {
        set_int(&config->window_state.w, value);
    } else if (INI_FULL_MATCH("window", "height")) {
        set_int(&config->window_state.h, value);
    } else if (INI_FULL_MATCH("window", "maximized")) {
        config->window_state.maximized = INI_IS_TRUE(value);
    }
    return 1;
}

void settings_set_string(char **field, const char *value) {
    free_nullable(*field);
    *field = value != NULL ? strdup(value) : NULL;
}

static void set_string(char **field, const char *value) {
    settings_set_string(field, value);
}

static void set_int(int *field, const char *value) {
    if (value == NULL) {
        *field = 0;
        return;
    }
    errno = 0;
    int val = (int) strtol(value, NULL, 10);
    if (errno != 0) {
        *field = 0;
        return;
    }
    *field = val;
}

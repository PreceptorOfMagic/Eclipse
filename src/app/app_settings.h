/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <Limelight.h>

#include <stdbool.h>
#include "ss4s/video.h"
#include "stream/video/coop_stream_mode.h"

typedef struct window_state_t {
    int x, y, w, h;      /**< the NORMAL (restored) rectangle -- never the maximized one */
    bool maximized;
} window_state_t;

typedef struct app_settings_t {
    STREAM_CONFIGURATION stream;
    int debug_level;
    /** Keep the app's log in rotating files under <conf>/logs, for the About pane's "Share logs" page
     *  (util/diag_logs.h). Off by default; takes effect at the next start. */
    bool diag_logging;
    char *decoder;
    char *audio_backend;
    char *audio_device;
    char *language;
    bool sops;
    bool localaudio;
    bool fullscreen;
    /* True while stream.width/height are still the built-in fallback -- no config file
     * supplied them. The app fills them in from the actual display once SDL's video
     * subsystem is up, which cannot happen this early (webOS forbids initialising video
     * before NDL/LGNC, see app_init). */
    bool stream_size_defaulted;
    /* True until the user moves the bitrate slider or an INI supplies a bitrate.
     * Auto values follow the effective resolution/FPS and are deliberately not persisted,
     * so moving the same profile to another monitor cannot fossilise an old default. */
    bool stream_bitrate_defaulted;
    window_state_t window_state;
    int rotate;
    bool unsupported;
    bool quitappafter;
    bool autoresume;
    /* PIN auto-entry: after a Wake-on-LAN, auto-type the host login PIN over the
     * stream to unlock it. Fires only on the connect that follows a Wake press. */
    bool pin_autoentry;
    char *pin_code;        /* PIN digits (config file; MVP plaintext) */
    int pin_predelay_ms;   /* wait after the stream opens before starting */
    bool pin_dismiss;      /* send Enter first to dismiss the Windows lock screen */
    int pin_dismiss_ms;    /* wait after the dismiss key before typing digits */
    bool pin_submit;       /* send Enter after the digits */
    int pin_reconnect_delay_ms; /* after login, wait this long then reconnect once so
                                 * the host re-negotiates with HDR (Apollo only enables
                                 * the Windows HDR display state at fresh stream setup,
                                 * which is skipped on the lock-screen/secure desktop) */
    /* Type the credential as TEXT rather than as US key presses.
     *
     * A VK code means "the key in that position", so what it produces is decided by
     * the layout selected on the HOST -- a password with symbols typed at a UK or
     * German host comes out wrong, and at a lock screen there is no feedback to say
     * so. LiSendUtf8TextEvent carries the characters themselves; Apollo injects them
     * with KEYEVENTF_UNICODE, which ignores the layout entirely. Default ON.
     *
     * Off falls back to US key codes, for a host that does not implement the text
     * channel (it is a Sunshine protocol extension; a host that ignores it types
     * nothing at all, which is why this is switchable rather than unconditional). */
    bool pin_type_as_text;
    /* Restart the stream once after signing in. Apollo re-runs its HDR display setup
     * only on a FRESH launch -- a resume does not -- so a stream that came up on the
     * lock screen stays non-HDR without this. It costs a visible reconnect, so it is
     * separable from auto-entry itself: someone not using HDR wants the login and not
     * the restart. */
    bool pin_hdr_relaunch;
    int pin_relaunch_settle_ms; /* gap between the reconnect quit and the fresh launch
                                 * (and between silent retries), so Apollo's display/
                                 * encoder finishes reinit after the HDR mode change —
                                 * avoids the 503 "is a display connected" launch error */
    bool viewonly;
    bool absmouse;
    bool hardware_mouse;
    bool virtual_mouse;
    bool swap_abxy;
    /** Nintendo controllers: NINTENDO_LAYOUT_OFF / _AS_PRINTED / _XBOX (input_gamepad.c).
     *
     *  DETECTED PER CONTROLLER. swap_abxy swaps EVERY pad, so a Switch Pro Controller beside an Xbox pad
     *  could not both be right; a detected Nintendo pad now follows this instead. AS_PRINTED: the button
     *  labelled A sends A. XBOX: by position -- the bottom button sends A (B = A). OFF: no detection,
     *  Nintendo pads follow swap_abxy like any other. Unset (-1) until read: an existing config gets the
     *  layout swap_abxy already gave these pads, a new one AS_PRINTED -- so detection is on either way. */
    int nintendo_layout;
    /** Controller shortcuts during a stream: CONTROLLER_SHORTCUTS_ALL (default) / _COMBOS / _WHEEL / _NONE.
     *
     *  COMBOS are the button combinations (stream/input/session_gamepad.c): View + Start + LB + RB the stream menu,
     *  LB + RS the virtual mouse, RB + RS the keyboard, LB + LS the performance panel, and on Android View + Start
     *  home and View held = Guide. The WHEEL is holding Start (stream/input/start_wheel.h), which every controller
     *  can do. Either can be turned off without the other (user, 2026-10-21). */
    int controller_shortcuts;
    bool syskey_capture;
    /** webOS: keep the stream on the TV's WIRED card (USB-Ethernet adapter): WIRED_PIN_AUTO / _OFF / _ON.
     *
     *  HOST-DEPENDENT, SO A TRI-STATE (standing policy, aurora_host_caps.h): the TV cannot choose its own path
     *  (no root, LS2 public group), so Umbra pins the TV's addresses to the wired MAC for the session
     *  (serverinfo UmbraCaps bit 0). AUTO asks only a host that advertises it; ON asks any host (stock
     *  hosts ignore the parameters); both need a wired card that is up (stream/wired_nic.h). Replaces the
     *  rig-only arp-pin.ps1 hook, which had the TV's MACs hard-coded and ran for every client. */
    int wired_pin_mode;
    /** HOST MONITOR WHILE STREAMING: HOST_MONITOR_CHOOSE / _OFF / _ON (user, 2026-10-04).
     *
     *  Whether the streamed PC's own physical monitor is switched off for the stream. A per-launch request to
     *  Umbra (umbraMonitorOff, serverinfo UmbraCaps bit 4), never sent to a host that does not advertise it or
     *  to a host that is this PC (its monitor is where the client window is). CHOOSE (default) offers a second
     *  start button -- "Start streaming - host monitor off" -- beside the normal one, which leaves it on; OFF
     *  and ON keep the single button and apply that answer to every launch. Replaces the rig-only dispwake
     *  hook, which switched every host's monitor off for every stream. */
    int host_monitor_mode;
    bool hdr;   /* HDR10 (PQ) over HEVC Main10 or AV1 Main10 when host and decoder support it */
    bool force_full_color_range; /* SDR only: request full-range YUV (0-255) from host. No effect when HDR is on. */
    bool hevc;
    /** Periodic HEVC IDR refresh interval in seconds (0 = off, min 2 when enabled). */
    int idr_refresh_interval_sec;
    /** Sunshine: negotiate AV1 Main8/Main10 when decoder exposes SS4S_VIDEO_AV1. */
    bool av1;
    bool show_stats_on_start;
    bool show_stats_compact;
    int stick_deadzone;
    /**
     * Sent to host as STREAM_CONFIGURATION.clientRefreshRateX100 (Hz * 100, e.g. 11994 = 119.94 Hz).
     * 0 = omit (host default frame pacing).
     */
    int client_refresh_rate_x100;
    /** CO-OP SPLIT-SCREEN.
     *
     * Orientation is not cosmetic -- it selects a different composition path in the client
     * and a different slice layout from the host. HORIZONTAL splices one slice per pane
     * (the cheap, long-proven path). VERTICAL builds a two-tile-column picture, which is
     * the only way a side-by-side split is expressible in HEVC: a slice must be a
     * contiguous run in RASTER order, so it can only ever be a horizontal band.
     */
    int coop_orientation;              /* COOP_ORIENTATION_HORIZONTAL / _VERTICAL */
    /** Which host each controller drives: COOP_PAD_PRIMARY / _SECONDARY. Index is the
     *  controller number - 1. BOTH is no longer offered (user, 2026-09-26: a controller
     *  goes to one PC); a stored BOTH is read back as PRIMARY. */
    int coop_pad_pane[4];
    /** Which host(s) are heard: COOP_AUDIO_MIX / _PRIMARY_ONLY / _SECONDARY_ONLY.
     *  Mixing is the default because couch co-op is a shared soundstage -- one TV,
     *  both games, the way local co-op sounded on a 360. */
    int coop_audio_mode;
    /** PLAY A SOUND BOTH HOSTS MAKE ONCE (user, 2026-10-03): in co-op both games often play the same narration,
     *  cutscene audio or music, and the two copies arrive slightly apart, so the mix echoes. The mixer turns down the
     *  LATER copy wherever the two streams carry the same sound (the passive gate, stream/audio/coop_audio_pgate.c;
     *  /tmp/coop-audio-gate off = the older coop_audio_dedupe.c for A/B); each player's own sounds that fall in a muted
     *  band are muted with it, and the gate balances the two hosts' levels (up to +-6 dB each).
     *  COOP_AUDIO_FILTER_ACCURATE (default, user 2026-10-10): 40 ms look-ahead, no predictions -- 45 ms of audio delay
     *  on both streams equally. _FAST: decides on the newest 5 ms and predicts onsets -- 5 ms delay, more artefacts.
     *  _OFF: no filter. Accurate runs as Fast where Eclipse itself paces the speakers (pull output: Windows, Linux
     *  PulseAudio). ini `coop_audio_filter` = off|fast|accurate; the older bool `coop_audio_dedupe` is still written
     *  (= filter != off) for older builds, and a stored `coop_audio_dedupe = false` reads as off. Only applies when
     *  mixing; /tmp/coop-audio-dedupe (off|fast|accurate|on) overrides it live for A/B. */
    int coop_audio_filter;
    /** Accurate filter's look-ahead in ms (user 2026-10-10: a slider "so I can test it fully and in the future give users
     *  more choice"; and below 40 too): 0..100 in 5 ms steps, default 40 (the evaluated value); 0 = no look-ahead but
     *  still no predictions. Audio delay = this + 5 ms. ini
     *  `coop_audio_filter_ahead_ms`. */
    int coop_audio_filter_ahead_ms;
    /** WHICH CODEC PATH CO-OP USES: COOP_STREAM_AUTO / _CORE / _COMPAT.
     *
     *  AUTO reads both hosts' encoders out of serverinfo and picks: same vendor -> the
     *  core path (HEVC, HDR available), mixed vendor -> H.264 compatibility mode, which
     *  is the only way an NVENC pane and an AMF pane can share one SPS/PPS. The full
     *  reasoning, and the measurements behind it, are in coop_stream_mode.h.
     *
     *  The override exists because detection can only be as good as what the hosts say,
     *  and a stock Sunshine host says nothing at all -- someone who KNOWS their pair is
     *  mixed must be able to say so without installing anything. */
    int coop_stream_mode;
    /** CO-OP ENCODER OVERRIDE (COOP_ENCODER_*), the co-op counterpart of the Video codec setting.
     *  Co-op cannot use the Video codec choice: both halves are ONE HEVC picture, so both PCs must
     *  produce HEVC with matching coding blocks. AUTO (default, recommended) picks from the pair:
     *  NVIDIA+NVIDIA -> each PC's own encoder, any AMD -> the GPU HEVC encoder on both. The override
     *  exists for a pair Eclipse cannot identify; coop_stream_plan_choose() and the pane explain why
     *  changing it otherwise breaks the picture. Replaces the retired, ignored coop_stream_mode. */
    int coop_encoder;
    /** CO-OP HOST TUNING: send umbraCoop=1 on co-op launches so an Umbra host applies its co-op
     *  encoder settings (NVENC constrained encoding, gop_length, coop_pad_rows) to that session
     *  only. Default ON; off leaves each host on its ordinary settings (a fresh NVIDIA host then
     *  draws a damaged seam). Stock hosts ignore the request either way. */
    bool coop_host_tuning;
    /** WHICH TWO PCs take part, remembered from the last co-op session so the launch
     *  popup opens on what was used last rather than making the choice every time.
     *  Empty until a session has been launched. These are what turned the dev-time
     *  /tmp/coop-spawn sentinel into a real setting. */
    char *coop_primary_uuid;
    char *coop_secondary_uuid;
    /** THE PAIR THE LAUNCH POPUP SHOULD OPEN ON, when the user does not want "whatever
     *  was used last". Empty (the default) means exactly that -- fall back to the last
     *  session, then to the selected PC, then to the first paired ones.
     *
     *  Separate keys rather than writing coop_primary_uuid above, because these two
     *  answer different questions: one is a RECORD of what happened, the other is an
     *  INSTRUCTION. Folding them together would mean every launch silently rewrote the
     *  user's stated default, which is the behaviour someone sets a default to escape.
     *  Only meaningful with more than two paired PCs; with exactly two there is nothing
     *  to choose and the popup shows no chooser at all. */
    char *coop_default_primary_uuid;
    char *coop_default_secondary_uuid;
    /** Legacy/preferred id for the Apollo co-op application. It is never trusted by
     *  itself: launch resolves each host's current app list independently and accepts
     *  this id only when the entry is named exactly "Co-op Pane". */
    int coop_app_id;

    /** EVERY CONTROLLER THIS APP HAS EVER SEEN, by name, most recent first.
     *
     * A controller has to be assignable to a PC while it is switched OFF -- that is the
     * whole point of a DEFAULT -- so the choice cannot be built from what is plugged in
     * right now. Names, because that is what the user picks from, what the assignment
     * engine matches on, and what survives re-pairing; the list is capped and the oldest
     * entry falls off, so a house full of borrowed pads cannot grow it without bound. */
    char **known_pads;
    size_t known_pads_count;
    /** CO-OP: ASSIGN THE MOUSE AND KEYBOARD LIKE CONTROLLERS (issue #37). Off (the default): both
     *  follow the pointer to whichever PC's pane it is over. On: each appears beside the
     *  controllers -- as COOP_KBM_MOUSE / COOP_KBM_KEYBOARD, claimed per PC in hosts.ini the way
     *  a pad is -- and belongs to one PC for the whole session. */
    bool coop_kbm_assign;

    /** CO-OP ADVANCED DIALS, ONE VALUE PER ORIENTATION.
     *
     * Every one of these was a /tmp sentinel first -- that is how they were all measured
     * -- and coop_tunables.c still writes those files, so the runtime reading them is
     * untouched and the test scripts keep overriding. 0 on a numeric dial means "leave
     * the built-in default alone", never "zero".
     *
     * INDEXED BY COOP_ORIENTATION_*, because the two modes are not variants of one
     * setup: horizontal is GPU-heavy and vertical is CPU-heavy, they compose by
     * different paths (slices vs tiles), and a value measured in one has never been
     * evidence for the other. The settings screen keeps both in step until they are
     * deliberately parted, so the common case is still one number to think about.
     * coop_tunables.c only ever applies the CURRENT orientation's column. */
    int coop_fps_cap[2];          /**< 0 = both hosts free-running (the requirement) */
    int coop_fps_peer[2];         /**< 0 = cap - 5; only meaningful with a cap */
    int coop_slices[2];           /**< 0 = VDEC_STREAM_SLICES_PER_FRAME */
    int coop_queue_depth[2];      /**< 0 = the per-mode default in coop_coord.c */
    int coop_audio_depth_ms[2];   /**< 0 = COOP_AUDIO_DEFAULT_DEPTH_MS */
    int coop_bitrate[2];          /**< 0 = the shared stream bitrate */
    int coop_fps[2];              /**< 0 = the shared stream frame rate */
    int coop_height[2];           /**< 0 = the shared stream height */
    bool coop_skip_conceal[2];    /**< default ON: holds a tile bit-exactly */
    bool coop_catchup[2];         /**< default ON: composite runs at max(primary, peer) */
    bool coop_repeat_conceal[2];  /**< legacy repeat-the-last-slice concealer */
    bool coop_repeat_pane[2];     /**< repeat the peer pane instead of concealing */
    bool coop_measure[2];         /**< frame-arrival jitter measurement */
    bool coop_pump[2];            /**< pumped GLib context for starfish callbacks */
    bool coop_self_mirror[2];     /**< compose THIS desktop into both panes (one-host test) */
    /** Show both columns even where the two orientations still agree. A dial that has
     *  been parted shows both regardless -- hiding a difference would be a lie. */
    bool coop_split_orientations;

    char *conf_dir;
    char *ini_path;
    char *condb_path;
    char *key_dir;
    bool conf_persistent;
    /* False only when moonlight.ini existed but could not be opened or parsed.
     * An in-memory default/partial config must never overwrite that recoverable file. */
    bool settings_save_allowed;
} CONFIGURATION, *PCONFIGURATION, app_settings_t;

/** Co-op split orientation. Horizontal is the default: it is the proven path and costs
 *  nothing extra, while vertical needs tile columns and constrained encoding. */
enum {
    COOP_ORIENTATION_HORIZONTAL = 0,
    COOP_ORIENTATION_VERTICAL = 1,
};

/** Controller routing. PRIMARY and SECONDARY keep their old 0/1 values so an
 *  existing conf file stays valid; BOTH (2) is no longer offered and reads back as PRIMARY. */
enum {
    COOP_PAD_PRIMARY = 0,     /**< player 1 pane: the host this process streams from */
    COOP_PAD_SECONDARY = 1,   /**< player 2 pane: routed to the peer over coop_input */
    COOP_PAD_BOTH = 2,        /**< sent to both hosts -- NOT OFFERED any more; parsed as PRIMARY */
};

/** Self-mirror sources: compose the second pane from THIS desktop so co-op can be tested
 *  with one host. Never a recording -- see CLAUDE.md on /tmp/coop-playfile. */
enum {
    COOP_MIRROR_OFF = 0,
    COOP_MIRROR_HORIZONTAL = 1,
    COOP_MIRROR_VERTICAL = 2,
};

/** Cap on [known_pads]: enough for every pad a living room accumulates, small enough
 *  that the settings list stays navigable with a d-pad. */
#define KNOWN_PADS_MAX 24

/** The mouse and the keyboard as ASSIGNABLE items (coop_kbm_assign): reserved names stored in a
 *  PC's hosts.ini pad claims exactly like a controller's, which no real device reports. They
 *  are never in [known_pads]; the screens add them while the setting is on. */
#define COOP_KBM_MOUSE "@mouse"
#define COOP_KBM_KEYBOARD "@keyboard"

/**
 * Record @p name as a controller that exists. Moves an existing entry to the front so
 * the list reads most-recently-seen first. Returns true if the stored list changed,
 * which is the caller's cue to save.
 */
bool settings_remember_pad(app_settings_t *config, const char *name);

/** Drop @p name from the seen-controllers list. The caller is responsible for clearing
 *  it from any host that claimed it -- a name left in hosts.ini would come back the next
 *  time the pad is plugged in, which is not what "forget" means. */
bool settings_forget_pad(app_settings_t *config, const char *name);

/** Put every co-op advanced dial back to its shipped value, both orientations. */
void settings_coop_advanced_defaults(app_settings_t *config);

/** Which host(s) are heard on the TV's one output. Mixing is the default: couch
 *  co-op is a shared soundstage, and the TV has a single audio out anyway -- a
 *  second PCM output is not grantable (co-op spike #3). */
enum {
    HOST_MONITOR_CHOOSE = 0,
    HOST_MONITOR_OFF = 1,
    HOST_MONITOR_ON = 2,
};

enum {
    WIRED_PIN_AUTO = 0,
    WIRED_PIN_OFF = 1,
    WIRED_PIN_ON = 2,
};

enum {
    NINTENDO_LAYOUT_OFF = 0,
    NINTENDO_LAYOUT_AS_PRINTED = 1,
    NINTENDO_LAYOUT_XBOX = 2,
};

enum {
    CONTROLLER_SHORTCUTS_ALL = 0,
    CONTROLLER_SHORTCUTS_COMBOS = 1,
    CONTROLLER_SHORTCUTS_WHEEL = 2,
    CONTROLLER_SHORTCUTS_NONE = 3,
};

enum {
    COOP_AUDIO_FILTER_OFF = 0,
    COOP_AUDIO_FILTER_FAST = 1,
    COOP_AUDIO_FILTER_ACCURATE = 2,
};

enum {
    COOP_AUDIO_MIX = 0,
    COOP_AUDIO_PRIMARY_ONLY = 1,
    COOP_AUDIO_SECONDARY_ONLY = 2,
};

/** Preferred legacy id for "Co-op Pane". Host-local ids are discovered at launch;
 *  this value is only a fast-path candidate and must match the exact app name. */
#define COOP_APP_ID_DEFAULT 2079486299

typedef struct audio_config_entry_t {
    int configuration;
    const char *value;
    const char *name;
} audio_config_entry_t;

extern const audio_config_entry_t audio_configs[];
extern const size_t audio_config_len;

#define CONF_NAME_MOONLIGHT "moonlight.ini"
#define CONF_NAME_HOSTS "hosts.ini"

#define RES_MERGE(w, h) (((w) & 0xFFFF) << 16 | ((h) & 0xFFFF))

#define RES_720P RES_MERGE(1280, 720)
#define RES_1080P RES_MERGE(1920, 1080)
#define RES_1440P RES_MERGE(2560, 1440)
/** ~90% of 4K (3584×2016); stable on recent LG TVs without full-4K input delay */
#define RES_3_6K RES_MERGE(3584, 2016)
#define RES_1800P RES_MERGE(3200, 1800)
#define RES_4K RES_MERGE(3840, 2160)

/** Fixed decode-unit reassembly buffer (megabytes). */
#define VDEC_REASSEMBLY_BUFFER_MB 2

void settings_initialize(app_settings_t *config, char *conf_dir);

bool settings_read(app_settings_t *config);

bool settings_save(app_settings_t *config);

/** Keep stream.fps aligned with client_refresh_rate_x100 when a fractional rate is set. */
void settings_sync_refresh_rate(app_settings_t *config);

/** webOS: apply NTSC x100 only for 30/60/120/240; drop stale x100 for 144/90/etc. */
void settings_reconcile_refresh_rate(app_settings_t *config);

/** NTSC refresh rate (Hz × 100) for a nominal FPS preset, or 0 if not mapped (e.g. 144). */
int settings_ntsc_refresh_rate_x100_for_fps(int nominal_fps);

/** On webOS, set client_refresh_rate_x100 from a preset FPS (60→5994, 120→11988, …). */
void settings_apply_ntsc_preset_refresh(app_settings_t *config, int nominal_fps);

void settings_clear(app_settings_t *config);

/** Replace an owned string field, freeing the old value. Public because the co-op
 *  launch popup persists the pair of hosts it just used, and duplicating the
 *  allocator choice at the call site is how a mismatched free gets written. */
void settings_set_string(char **field, const char *value);

int settings_optimal_bitrate(const SS4S_VideoCapabilities *capabilities, int w, int h, int fps);

/** Recompute bitrate for the effective stream geometry only while it is still automatic. */
void settings_refresh_auto_bitrate(app_settings_t *config, const SS4S_VideoCapabilities *capabilities);

bool audio_config_valid(int config);

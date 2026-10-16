#include <unistd.h>
#include "app.h"
#include "app_settings.h"
#include "app_launch.h"
#include "session_priv.h"

#include "stream/session.h"

#include "backend/pcmanager/priv.h"

#include <Limelight.h>

#include "libgamestream/errors.h"

#include "logging.h"
#include "ss4s.h"
#include "input/input_gamepad.h"
#include "app_session.h"
#include "session_worker.h"
#include "stream/input/session_virt_mouse.h"
#include "backend/aurora_host_caps.h"
#include "stream/video/coop_secondary.h"
#include "stream/video/coop_tunables.h"
#include "stream/video/coop_session.h"
#include "stream/video/coop_secondary.h"
#if defined(TARGET_ANDROID)
#include "platform/android/android_platform.h"
#endif

#include <stdio.h>

// Expected luminance values in SEI are in units of 0.0001 cd/m2
#define LUMINANCE_SCALE 10000

int streaming_errno = GS_OK;
char streaming_errmsg[1024];

static bool streaming_sops_supported(PDISPLAY_MODE modes, int w, int h, int fps);

static void session_config_init(app_t *app, session_config_t *config, const SERVER_DATA *server,
                                const CONFIGURATION *app_config);

static void populate_hdr_info_vui(SS4S_VideoHDRInfo *info, const STREAM_CONFIGURATION *config);

session_t *session_create(app_t *app, const CONFIGURATION *config, const SERVER_DATA *server, const APP_LIST *gs_app) {
    if (app_launch_coop_requested() && coop_secondary_mode() == NULL &&
        !coop_session_prepare_scripted(server, "/tmp/coop-spawn")) {
        streaming_errno = GS_FAILED;
        snprintf(streaming_errmsg, sizeof(streaming_errmsg), "Invalid co-op peer configuration");
        return NULL;
    }
    if (app_launch_coop_requested() &&
            (app->ss4s.video_cap.codecs & SS4S_VIDEO_H265) == 0) {
        streaming_errno = GS_FAILED;
#ifdef OS_WINDOWS
        snprintf(streaming_errmsg, sizeof(streaming_errmsg),
                 "Co-op requires hardware HEVC Main decoding to NV12. This GPU or driver "
                 "did not expose the required D3D11VA capability. Update the GPU driver, "
                 "or use regular H.264 streaming on this PC.");
#else
        snprintf(streaming_errmsg, sizeof(streaming_errmsg),
                 "Co-op requires HEVC hardware decoding, which the selected video decoder "
                 "does not support.");
#endif
        commons_log_error("Session", "%s", streaming_errmsg);
        return NULL;
    }
    session_t *session = malloc(sizeof(session_t));
    SDL_memset(session, 0, sizeof(session_t));
    session_config_init(app, &session->config, server, config);
    session->app = app;
    session->display_width = app->ui.width;
    session->display_height = app->ui.height;
    session->audio_cap = app->ss4s.audio_cap;
    session->video_cap = app->ss4s.video_cap;
    session->server = serverdata_clone(server);
    // The flags seem to be the same to supportedVideoFormats, use it for now...
    session->server->serverInfo.serverCodecModeSupport = 0;
    if (session->config.stream.supportedVideoFormats & VIDEO_FORMAT_H264) {
        session->server->serverInfo.serverCodecModeSupport |= SCM_H264;
    }
    if (session->config.stream.supportedVideoFormats & VIDEO_FORMAT_H265) {
        session->server->serverInfo.serverCodecModeSupport |= SCM_HEVC;
        if (session->config.stream.supportedVideoFormats & VIDEO_FORMAT_H265_MAIN10) {
            session->server->serverInfo.serverCodecModeSupport |= SCM_HEVC_MAIN10;
        }
    }
    if (session->config.stream.supportedVideoFormats & VIDEO_FORMAT_AV1_MAIN8) {
        session->server->serverInfo.serverCodecModeSupport |= SCM_AV1_MAIN8;
    }
    if (session->config.stream.supportedVideoFormats & VIDEO_FORMAT_AV1_MAIN10) {
        session->server->serverInfo.serverCodecModeSupport |= SCM_AV1_MAIN10;
    }
    session->app_id = gs_app->id;
    session->app_name = strdup(gs_app->name);
    session->mutex = SDL_CreateMutex();
    session->cond = SDL_CreateCond();
#if FEATURE_EMBEDDED_SHELL
    if (!app_is_decoder_valid(app)) {
        session->embed = app_has_embedded(session->app);
    }
#endif
    session_input_init(&session->input, session, &app->input, &session->config);
    SDL_ThreadFunction worker_fn = (SDL_ThreadFunction) session_worker;
#if FEATURE_EMBEDDED_SHELL
    if (session_use_embedded(session)) {
        worker_fn = (SDL_ThreadFunction) session_worker_embedded;
    }
#endif
    session->thread = SDL_CreateThread(worker_fn, "session", session);
    return session;
}

stream_input_t *session_get_input(session_t *session) {
    if (!session) {
        return NULL;
    }
    return &session->input;
}

int session_get_app_id(session_t *session) {
    return session != NULL ? session->app_id : 0;
}

void session_destroy(session_t *session) {
    session_interrupt(session, false, STREAMING_INTERRUPT_QUIT);
    session_input_deinit(&session->input);
    SDL_WaitThread(session->thread, NULL);
    serverdata_free(session->server);
    SDL_DestroyCond(session->cond);
    SDL_DestroyMutex(session->mutex);
    free(session->app_name);
    free(session);
}

bool session_is_streaming(const session_t *session) {
    if (!session) {
        return false;
    }
    SDL_LockMutex(session->mutex);
    bool streaming = (session->state == STREAMING_STREAMING);
    SDL_UnlockMutex(session->mutex);
    return streaming;
}

void session_interrupt(session_t *session, bool quitapp, streaming_interrupt_reason_t reason) {
    if (!session) {
        return;
    }
    SDL_LockMutex(session->mutex);
    if (session->interrupted) {
        SDL_UnlockMutex(session->mutex);
        return;
    }
    /* `quitappafter` was parsed into the config but never read, so a stream that
     * ended any way other than the user pressing Quit left the app RUNNING on the
     * host. Apollo then only PAUSES the session -- app, virtual display and any
     * detached commands stay alive waiting for a resume that never comes, the
     * session-end prep-cmd never runs, and the next connect has to reconcile the
     * stale session. Honour the setting for deliberate teardowns only.
     *
     * ONLY where the user expressed NO preference: BACKGROUND (webOS pulled focus)
     * and QUIT. Deliberately NOT USER.
     *
     * STREAMING_INTERRUPT_USER covers BOTH UI actions -- Disconnect passes
     * quitapp=false and Quit passes quitapp=true -- so the caller has ALREADY stated
     * the intent in the parameter. Overriding it here collapsed the two buttons into
     * one: Disconnect started quitting the app too, which loses the whole point of
     * disconnecting (leaving the game running to resume into). That was a real
     * regression in ordinary streaming, reported 2026-07-29. Never second-guess an
     * explicit user choice from a config default.
     *
     * Also NOT the >= STREAMING_INTERRUPT_ERROR reasons (watchdog, network, decoder),
     * where the stream dropped unexpectedly and keeping the app alive to resume into
     * is the point -- quitting there would kill a running game on a transient blip.
     * NOT HOST either: the host ended it, so there is nothing left to ask it to quit.
     *
     * Applied BEFORE the diag line below so the log records what actually happens. */
    if (!quitapp && app_configuration != NULL && app_configuration->quitappafter) {
        switch (reason) {
            case STREAMING_INTERRUPT_BACKGROUND:
            case STREAMING_INTERRUPT_QUIT:
                quitapp = true;
                break;
            default:
                break;
        }
    }
    /* Record WHY the stream stopped. A clean teardown with no [CONN] line means the
     * interrupt came from us, not the network/host — this separates USER (0) from
     * BACKGROUND (1, webOS pulled focus) from QUIT (2) / HOST (3). */
    FILE *diag = fopen(AURORA_DIAG_LOG, "a");
    if (diag) {
        fprintf(diag, "[INT@%u] session_interrupt reason=%d quitapp=%d\n",
                (unsigned) SDL_GetTicks(), (int) reason, (int) quitapp);
        fclose(diag);
    }
    session_input_interrupt(&session->input);
    session->quitapp = quitapp;
    session->interrupt_reason = reason;
    session->interrupted = true;
#if FEATURE_EMBEDDED_SHELL
    if (session->embed && session->embed_process) {
        embed_interrupt(session->embed_process);
    }
#endif
    if (reason >= STREAMING_INTERRUPT_ERROR) {
        switch (reason) {
            case STREAMING_INTERRUPT_WATCHDOG:
                streaming_error(session, reason, "Stream stalled");
                break;
            case STREAMING_INTERRUPT_NETWORK:
                streaming_error(session, reason, "Network error happened");
                break;
            case STREAMING_INTERRUPT_DECODER:
                streaming_error(session, reason, "Decoder reported error");
                break;
            default:
                streaming_error(session, reason, "Error occurred while in streaming");
                break;
        }
    }
    SDL_CondSignal(session->cond);
    SDL_UnlockMutex(session->mutex);
}

bool session_accepting_input(session_t *session) {
    return session->input.started && !ui_should_block_input();
}

bool session_start_input(session_t *session) {
#if FEATURE_EMBEDDED_SHELL
    if (session->embed) {
        return false;
    }
#endif
    session_input_started(&session->input);
    return true;
}

void session_stop_input(session_t *session) {
    session_input_stopped(&session->input);
}

bool session_has_input(session_t *session) {
    return session->input.started;
}

void session_toggle_vmouse(session_t *session) {
    bool value = session->config.vmouse && !session_input_is_vmouse_active(&session->input.vmouse);
    session_input_set_vmouse_active(&session->input.vmouse, value);
}

#if TARGET_ANDROID
void session_send_guide(session_t *session) {
    stream_input_send_guide(&session->input);
}
#endif

void session_screen_keyboard_opened(session_t *session) {
    session_input_screen_keyboard_opened(&session->input);
}

void session_screen_keyboard_closed(session_t *session) {
    session_input_screen_keyboard_closed(&session->input);
}

void streaming_display_size(session_t *session, short width, short height) {
    session->display_width = width;
    session->display_height = height;
}

void streaming_enter_fullscreen(session_t *session) {
    if (!session->player) {
        return;
    }
    if ((session->video_cap.transform & SS4S_VIDEO_CAP_TRANSFORM_UI_COMPOSITING) == 0) {
        SS4S_PlayerVideoSetDisplayArea(session->player, NULL, NULL);
    }
}

void streaming_enter_overlay(session_t *session, int x, int y, int w, int h) {
    app_set_mouse_grab(&session->app->input, false);
    SS4S_VideoRect dst = {x, y, w, h};
    if ((session->video_cap.transform & SS4S_VIDEO_CAP_TRANSFORM_UI_COMPOSITING) == 0) {
        SS4S_PlayerVideoSetDisplayArea(session->player, NULL, &dst);
    }
}

void streaming_set_hdr(session_t *session, bool hdr) {
    commons_log_info("Session", "HDR is %s", hdr ? "enabled" : "disabled");
    SS_HDR_METADATA hdr_metadata;
    if (!hdr) {
        SS4S_PlayerVideoSetHDRInfo(session->player, NULL);
    } else if (LiGetHdrMetadata(&hdr_metadata)) {
        SS4S_VideoHDRInfo info = {
                .displayPrimariesX = {
                        hdr_metadata.displayPrimaries[0].x,
                        hdr_metadata.displayPrimaries[1].x,
                        hdr_metadata.displayPrimaries[2].x
                },
                .displayPrimariesY = {
                        hdr_metadata.displayPrimaries[0].y,
                        hdr_metadata.displayPrimaries[1].y,
                        hdr_metadata.displayPrimaries[2].y
                },
                .whitePointX = hdr_metadata.whitePoint.x,
                .whitePointY = hdr_metadata.whitePoint.y,
                .maxDisplayMasteringLuminance = hdr_metadata.maxDisplayLuminance * LUMINANCE_SCALE,
                .minDisplayMasteringLuminance = hdr_metadata.minDisplayLuminance,
                .maxContentLightLevel = hdr_metadata.maxContentLightLevel,
                .maxPicAverageLightLevel = hdr_metadata.maxFrameAverageLightLevel,
        };
        populate_hdr_info_vui(&info, &session->config.stream);
        SS4S_PlayerVideoSetHDRInfo(session->player, &info);
    } else {
        SS4S_VideoHDRInfo info = {
                .displayPrimariesX = {34000, 13250, 7500},
                .displayPrimariesY = {16000, 34500, 3000},
                .whitePointX = 15635,
                .whitePointY = 16450,
                .maxDisplayMasteringLuminance = 1000 * LUMINANCE_SCALE,
                .minDisplayMasteringLuminance = 50,
                .maxContentLightLevel = 1000,
                .maxPicAverageLightLevel = 400,
        };
        populate_hdr_info_vui(&info, &session->config.stream);
        SS4S_PlayerVideoSetHDRInfo(session->player, &info);
    }
}

void streaming_error(session_t *session, int code, const char *fmt, ...) {
    SDL_LockMutex(session->mutex);
    streaming_errno = code;
    va_list arglist;
    va_start(arglist, fmt);
    vsnprintf(streaming_errmsg, sizeof(streaming_errmsg) / sizeof(char), fmt, arglist);
    va_end(arglist);
    SDL_UnlockMutex(session->mutex);
}

bool streaming_sops_supported(PDISPLAY_MODE modes, int w, int h, int fps) {
    for (PDISPLAY_MODE cur = modes; cur != NULL; cur = cur->next) {
        if (cur->width == w && cur->height == h && cur->refresh == fps) {
            return true;
        }
    }
    return false;
}

void session_config_init(app_t *app, session_config_t *config, const SERVER_DATA *server,
                         const CONFIGURATION *app_config) {
    CONFIGURATION resolved = *app_config;
    settings_reconcile_refresh_rate(&resolved);

    config->stream = resolved.stream;
    config->stream.enableIntraRefresh = app_launch_coop_requested() ? 1 : 0;

    /* PER-ORIENTATION PICTURE OVERRIDES, applied before any of the co-op geometry below
     * so the halving works off the composite the user asked for. 0 means "use the shared
     * stream setting", which is what a fresh conf has -- so this changes nothing until
     * somebody parts the two columns in the co-op settings. Horizontal is GPU-heavy and
     * vertical CPU-heavy, and they have never wanted the same numbers. */
    if (!app_launch_coop_requested()) {
        /* AN ORDINARY STREAM MUST NOT INHERIT A CO-OP MODE. `/tmp/coop-vmirror` halves
         * the requested WIDTH whether or not this is co-op, and `/tmp/coop-slices`
         * rewrites the slice count in the SDP for every stream -- so a switch left over
         * from a co-op run silently reshapes this one. That is the documented trap: a
         * sentinel from another mode making a broken run look like a working one. */
        coop_tunables_disarm();
    }
    if (app_launch_coop_requested()) {
        /* ARM THE MODE SELECTOR FROM THE SETTINGS, because the shell cannot.
         *
         * /tmp is sticky and the app is uid 6584 while every script here is `prisoner`
         * (6122), so neither can remove the other's sentinels -- the comment in
         * app_webos.c already records that direction of it. The other direction is what
         * broke vertical: run_coop.sh's `touch /tmp/coop-vmirror; rm -f /tmp/coop-hh-tiles`
         * silently half-succeeded, leaving BOTH selectors on disk. Doing it here, from
         * `coop_orientation` in moonlight.ini (which prisoner DOES own and can write),
         * gives the mode exactly one writer.
         *
         * Placed above the geometry below on purpose: session.c:420 halves the width on
         * /tmp/coop-vmirror and session_video.c latches both selectors at stream setup, so
         * this must land before either reads them. */
        /* THE UI SESSION WINS WHEN THERE IS ONE, and reading the ini instead was a
         * regression that broke horizontal outright (2026-08-15).
         *
         * The co-op dialog does NOT write `coop_orientation` to moonlight.ini -- it puts the
         * chosen orientation in the coop_session (coop.dialog.c start_stream) and arms from
         * that. The ini key is the SCRIPT path's channel: run_coop.sh writes it because
         * `prisoner` cannot create the sentinels itself. So the two carry the same meaning
         * from different places, and only the scripts keep them in step.
         *
         * Reading the ini here therefore overrode the user's dialog choice with whatever the
         * last scripted run happened to leave behind. Measured: ini = 1 (vertical, left by a
         * vertical matrix run), user picked HORIZONTAL, the dialog armed hh-tiles and cleared
         * vmirror, and this line then armed vmirror and removed hh-tiles a moment later. The
         * client composed tile COLUMNS while both hosts sent 3840x1088 half-height rows --
         * extreme artefacts, and only in horizontal, because vertical agreed with the stale
         * ini by luck. Prefer the session; fall back to the ini for `ares-launch "coop":true`,
         * which has no session and is the path the ini exists for. */
        /* THE PRIMARY ARMS. THE SECONDARY READS. Exactly one writer for the sentinels.
         *
         * This function runs in BOTH processes -- the secondary is a fork with its own
         * globals, so coop_session_get() is NULL there and the fallback below reads the ini.
         * That made the secondary RE-ARM the mode from a stale ini and clobber what the
         * primary had just set: measured 2026-08-15 with the ini at 1 (vertical) and the
         * dialog set to horizontal, the log carries both
         *   [COOP] half-height: requesting 3840x1088   (primary, correct)
         *   [COOP-VT] half-width: requesting 1920x2160 (secondary, from the stale ini)
         * in the same session. The two halves disagreed about the geometry, so the peer's
         * pane was a 1920x2160 column spliced into a 3840x1088 row -- the whole bottom half
         * of the panel shredded, user-reported.
         *
         * Ordering makes the read safe: the primary arms here, in session_config_init, and
         * only spawns the secondary later at stream setup (session_video.c:2532), so the
         * sentinels are already correct by the time the child resolves its own config from
         * them. */
        if (coop_secondary_mode() == NULL) {
            /* Clamped by the CODEC, exactly as the dialog does before it. This is the path a
             * sentinel launch takes -- no dialog, orientation straight from the ini -- and it
             * must not arm a vertical mode an H.264 session has no composer for.
             *
             * ONE RESOLVER FOR THE WHOLE PROCESS: coop_orientation_resolved() is the dialog's
             * choice, else the primary's published one, else the ini. It used to be spelled out
             * here and differently again in coop_tunables.c, which is how the two ended up
             * disagreeing across the primary/secondary boundary (see coop_session.h). */
            coop_tunables_arm_mode(coop_orientation_for_mode(coop_orientation_resolved(),
                                                             coop_stream_mode_resolved()));
        }
        const int coop_bitrate = coop_tunables_bitrate();
        const int coop_fps_override = coop_tunables_fps();
        const int coop_height_override = coop_tunables_height();
        /* BITRATE IS DERIVED FROM THE VIDEO SETTING, NOT COPIED. Each PC sends half of the
         * picture, so each gets HALF the Video bitrate: the two together use what one ordinary
         * stream would, at the same bits per pixel. Copying it (the old behaviour) put twice the
         * user's chosen rate on their network and into the client's one decoder. The co-op dial
         * (coop_bitrate, per PC) overrides this, exactly as before. */
        if (coop_bitrate > 0) {
            config->stream.bitrate = coop_bitrate;
        } else if (config->stream.bitrate > 0) {
            config->stream.bitrate = config->stream.bitrate / 2;
        }
        if (coop_fps_override > 0) {
            config->stream.fps = coop_fps_override;
        }
        if (coop_height_override > 0 && config->stream.height > 0) {
            /* Keep the aspect the hosts are asked for: the width follows the height. */
            config->stream.width = config->stream.width * coop_height_override / config->stream.height;
            config->stream.height = coop_height_override;
        }
    }

    /* HALF-HEIGHT CO-OP: ask each host for HALF the composite, not all of it.
     *
     * Until now both hosts streamed a FULL 3840x2160 desktop and the client simply took
     * half of each, which has two consequences the user hit directly: their content sits
     * in the wrong place (PC1's taskbar is at y~2100 of its own 4K desktop, inside the
     * OTHER pane, so it is blanked away and never visible), and -- the reason this
     * matters far more -- each encoder's reference picture CONTAINS THE WHOLE 4K FRAME,
     * so a motion vector can reach into the other host's half and a missing pane
     * corrupts it. That is the ghosting, and no fallback policy can fully fix it:
     * conceal/repeat/skip only bound the damage.
     *
     * With each host encoding only its own 1080 rows, its reference contains ONLY its
     * own pane.
     *
     * BE PRECISE ABOUT WHAT THAT BUYS, because an earlier version of this comment
     * overclaimed it: cross-pane SPATIAL prediction is ALREADY prevented by NVENC
     * constrained encoding, which is live on both hosts and is what removed the guard
     * band. The ghosting that remains is TEMPORAL -- a dropped pane leaves the peer's
     * encoder advancing a reference chain the decoder never received -- and half-height
     * does not change that.
     *
     * What it does deliver is the requirement itself: each PC's desktop really is
     * 3840x1080, so its taskbar sits in its own pane and games render at the right
     * aspect. Halving the pixels per host also raises the pair rate, which reduces
     * ghosting indirectly.
     *
     * The geometry lands exactly: CTB=32, so 1080 pads to 1088 = 34 CTU rows, and
     * 34+34 = 68 rows = the same 2176 the current 4K composite already uses (cropped to
     * 2160 for display). The pane boundary stays at y=1088 and the splice geometry is
     * unchanged -- only the SOURCE height changes.
     *
     * Applied to BOTH sessions because both come through here, and they must agree:
     * a 1080-row picture spliced against a 2160-row one has no valid composition. */
    /* VERTICAL MIRROR: halve the WIDTH instead. The decoder is set up from the ORIGINAL
     * config (3840x2160) while the host is asked for 1920x2160, so two panes side by side
     * fill the composite. Without this the decoder is configured 1920 wide and the 3840-wide
     * tiled composite never decodes -- the stream simply sits at "Connecting...". */
    if (access("/tmp/coop-vmirror", F_OK) == 0 && config->stream.width > 1) {
        const int full = config->stream.width;
        config->stream.width = full / 2;
        if (coop_gpu_hevc_resolved()) config->stream.height = (config->stream.height + 31) & ~31;
        FILE *lf = fopen(AURORA_DIAG_LOG, "a");
        if (lf != NULL) {
            fprintf(lf, "[COOP-VT] half-width: requesting %dx%d from this host (composite %dx%d)\n",
                    config->stream.width, config->stream.height, full, config->stream.height);
            fclose(lf);
        }
    } else if (app_launch_coop_requested() && config->stream.height > 1) {
        const int full = config->stream.height;
        /* ROUND THE PANE UP TO A WHOLE NUMBER OF CTB ROWS, AND THE REASON IS VISIBLE ON THE
         * PANEL AT THE SEAM.
         *
         * 2160/2 = 1080, which is not a multiple of the 32-pixel CTB. An HEVC encoder cannot
         * code a partial CTB row, so it codes 1088 and sets a conformance window cropping the
         * 8 rows it invented. That is invisible in an ordinary stream -- they are at the
         * bottom of the picture and the decoder drops them.
         *
         * STACKED, THEY ARE NOT AT THE BOTTOM ANY MORE. The composite is our pane over the
         * peer's, so those 8 invented rows land in the MIDDLE, at the seam, and
         * coop_sps_double_height() doubles the crop -- which is right for one picture
         * stretched and wrong for two padded panes stacked. Measured 2026-08-01: the
         * composite codes 2176 and displays 2160, so our 8 padding rows are DISPLAYED at the
         * seam, the peer's pane sits 8 rows lower than intended, and its bottom 8 rows of
         * real content are cropped away instead.
         *
         * Asking for 1088 removes the whole problem by construction: the host codes exactly
         * what we asked for, its conformance window is empty, and doubling an empty window
         * stays empty -- so the composite is 2176 rows of real content with nothing invented
         * anywhere. The picture is then 16 rows taller than the panel and is scaled to fit,
         * which costs 0.7% vertical squash against a band of encoder padding through the
         * middle of the screen.
         *
         * This is one of three mechanisms that all land at the seam (see
         * docs/stack-profile.md section 8); it is the only one removable without touching a
         * host, and removing it is what makes measuring the other two mean anything. */
        const int ctb = 32;
        int half = full / 2;
        half = ((half + ctb - 1) / ctb) * ctb;
        /* GUARD ROWS: ask each host to ENCODE more rows than it DISPLAYS.
         *
         * WHAT THIS ACTUALLY PRODUCES WAS MEASURED 2026-09-05 AND IT IS NOT WHAT THE REST OF
         * THIS COMMENT SAYS. The extra rows are NOT the host's own captured content: Apollo
         * derives the virtual display's size from the size the client REQUESTS, the VDD never
         * adopts the taller mode, and Apollo centres an 1088-row capture inside the 1120-row
         * frame -- so what arrives is BLACK LETTERBOX PADDING, 16 rows per EDGE. Read directly
         * from the peer's delivered stream: rows 0-15 and 1104-1119 at mean 0.00, min 0, max 0.
         *
         * THE PROTECTION IS REAL BUT THE MECHANISM IS DIFFERENT, and this matters because the
         * sizing rule below is written for the wrong one. H.264 edge extension assumes the row
         * above a picture's first row REPEATS it; with black padding on BOTH panes that
         * assumption comes true at the seam, so the mismatch vanishes rather than being
         * out-reached. Measured: the pad rows reproduce inside the composite bit-exactly and the
         * peak seam ratio in the first content rows fell 1.025 -> 0.628. Nothing here is
         * "bounding how far a motion vector reaches", and the 1.8-rows-per-frame sizing rule
         * does not describe it.
         *
         * AND THE PROTECTION IS ACCIDENTAL. Nothing in this file asks for padding; it exists
         * only because the VDD did not adopt the requested mode. A driver that DID adopt it
         * gives ZERO padding while this knob still reads as armed. SUPERSEDED by Apollo's
         * `coop_pad_rows`, which shrinks the virtual display deliberately and leaves the encode
         * at the requested size -- same protection, stated rather than accidental, and without
         * making the composite taller than the panel (which costs 64-74 px of pillarboxing).
         * NOT ADDITIVE with it: set both and you get coop_pad_rows per edge and pay the
         * pillarboxing for nothing.
         *
         * This is the dual guard band, and it is a DIFFERENT mechanism from the
         * `guard_band.sh` desktop-blacking overlay that was retired on 2026-08-31. That one
         * blacks rows of the DESKTOP and has to out-reach the motion vector; measured
         * contamination reached 495 rows against H.264's MaxVmvR of +/-512, so no practical
         * depth could bound it. THIS one adds rows to the ENCODE, so a cross-seam motion
         * vector resolves to THAT HOST'S OWN reconstructed pixels in both the standalone
         * decode and the composite -- which is bit-exact by construction rather than by
         * being deep enough. Measured 2026-08-30 with 64-row guards and two genuinely
         * different MOVING sources: the peer half went from 46-58% wrong to bit-exact at
         * every frame (scripts/coop/avc/README.md:350).
         *
         * Sizing rule, measured: guard_rows >= 1.8 x frames_between_refresh (contamination
         * climbs through the band at ~1.8 rows/frame under aggressive synthetic motion; a
         * real desktop pane measured ZERO, because static content codes as zero-MV SKIP and
         * never invokes the interpolation filter). So 64 rows holds ~35 frames.
         *
         * NOTHING ELSE NEEDS TO CHANGE for this to work. coop_avc.c already derives the
         * composite height as src_sps.height_mbs + peer_sps.height_mbs and peer_first_mb as
         * width_mbs * src_sps.height_mbs (coop_avc.c:522-536), and session_video.c doubles
         * the negotiated height for the decoder -- so the taller pane propagates by itself.
         * Keep the total a multiple of 16 (H.264 MB) AND 32 (the CTB rounding above) or the
         * host re-introduces the conformance-window padding this block exists to avoid:
         * 1088 + 64 = 1152 = 72 MB rows exactly, composite 2304.
         *
         * DEFAULT 0 = byte-identical to every run before 2026-09-01. */
        int guard = 0;
        FILE *gf = fopen("/tmp/coop-guard", "r");
        if (gf != NULL) {
            if (fscanf(gf, "%d", &guard) != 1) { guard = 0; }
            fclose(gf);
        }
        /* CLAMP TO 64. THE BINDING CONSTRAINT IS BITRATE, NOT MACROBLOCK RATE -- measured
         * 2026-09-01, and it took three wrong clamps to find out.
         *
         * I first blamed MaxMBPS: at 60 fps a 3840x2304 composite is 240x144 = 34,560 MBs =
         * 2,073,600 MB/s, EXACTLY the Level 5.2 limit, and guard=64 did fail. The arithmetic
         * landing on 100.0% was a COINCIDENCE. Same geometry, same peer mode, bitrate the only
         * variable:
         *     composite 2304 @ 60 fps, coop_bitrate_h = 30000 -> panel shows 2130 of 2160  OK
         *                                              40000 -> 2127                       OK
         *                                              55000 -> 2125                       OK
         *                                              80000 -> 496                        FAILS
         * At 80 Mbps it does not fail in a fixed way either (1154 rows in one run, 496 in
         * another), which is a decoder falling over on throughput, not a geometric mis-size.
         * So geometry alone cannot be clamped safely -- 64 rows is fine when the bitrate is
         * kept at or below ~55 Mbps, and 32 rows is NOT required. Keep the geometric limit at
         * the largest value actually tested and let the bitrate carry the constraint. */
        /* RAISED 64 -> 128 on 2026-09-01. The 64 cap came from a 2304-row composite failing at
         * 60 fps, which I first attributed to MaxMBPS -- WRONG: an A/B/A with bitrate as the
         * only variable showed 2304 works at <=55 Mbps and fails at 80, so the limit is
         * BITRATE, not geometry, and a deeper guard is affordable if the bitrate comes down.
         * 128 gives 1088+128 = 1216 per pane (76 MB rows, 38 CTB rows -- exact on both) and a
         * 2432-row composite = 240x152 = 36,480 MBs, inside Level 5.2's MaxFS of 36,864.
         * WHY DEEPER AT ALL: the guard bounds how far cross-seam prediction can reach, and
         * the residual damage is measured CONCENTRATED IN THE FIRST ~50 ROWS BELOW THE SEAM,
         * i.e. the peer's longest motion vectors are still clearing a 64-row band. Sizing
         * rule: guard_rows >= 1.8 x frames_between_refresh, so 128 doubles the drift the
         * band absorbs before intra refresh has to clean up after it. */
        if (guard < 0 || guard > 128) { guard = 0; }
        guard = (guard / ctb) * ctb;                        /* keep CTB alignment: 40 -> 32 */
        half += guard;
        config->stream.height = half;
        FILE *lf = fopen(AURORA_DIAG_LOG, "a");
        if (lf != NULL) {
            fprintf(lf, "[COOP] half-height: requesting %dx%d from this host "
                        "(composite %dx%d; %d rounded up to a whole CTB row so no padding "
                        "lands at the seam; guard=%d extra rows -- MEASURED 2026-09-05 to be "
                        "BLACK LETTERBOX PADDING, 16 per EDGE, not captured content; they ARE "
                        "displayed and they make the composite TALLER THAN THE PANEL)\n",
                    config->stream.width, config->stream.height,
                    config->stream.width, half * 2, full / 2, guard);
            fclose(lf);
        }
    }

    if (resolved.client_refresh_rate_x100 > 0) {
        config->stream.clientRefreshRateX100 = resolved.client_refresh_rate_x100;
        config->stream.fps = (resolved.client_refresh_rate_x100 + 50) / 100;
    } else {
        config->stream.clientRefreshRateX100 = 0;
    }

    /* CO-OP FRAME RATE: NO CAP BY DEFAULT.
     *
     * REQUIREMENT (user, 2026-08-05): each side of the split must behave as its OWN 165Hz
     * half-4K monitor. Each host produces as many frames as it can, up to the refresh rate,
     * or as few as it likes, down to a completely static desktop. Neither side's rate
     * constrains the other.
     *
     * WHAT THIS BLOCK USED TO DO, AND WHY IT IS NOW OPT-IN. It capped BOTH sessions to a
     * shared rate (default 60, `/tmp/coop-fps`) and biased the secondary to `coop_fps - 5`,
     * to force the two streams 1:1. That was the correct fix for its time: composition
     * spliced one slice from each host, so a picture the primary produced without a matching
     * peer picture went out with its half UNCODED -- measured 4669 of 10800 pictures, which
     * was the mush on the bottom pane. Two free-running encoders could not be spliced.
     *
     * THEY CAN NOW, and the two mechanisms that make it possible are both live in tiles mode
     * as of 2026-08-05: catch-up emits extra composite pictures when the PEER runs ahead (so
     * the composite rate is max(primary, peer), not the primary's), and skip-conceal HOLDS a
     * tile bit-exactly when a host has no new frame. A held tile is not a defect under this
     * requirement -- a static desktop is supposed to show the same pixels.
     *
     * The cap therefore costs frame rate ([[latency-minimization]]) to solve a problem that
     * no longer exists. Writing `/tmp/coop-fps` still restores the old behaviour verbatim,
     * which is the A/B for judging this. */
    if (app_launch_coop_requested() && access("/tmp/coop-fps", F_OK) == 0) {
        int coop_fps = 60;
        FILE *ff = fopen("/tmp/coop-fps", "r");
        if (ff != NULL) {
            int v = 0;
            if (fscanf(ff, "%d", &v) == 1 && v >= 24 && v <= 240) {
                coop_fps = v;
            }
            fclose(ff);
        }
        /* THE PEER MUST RUN SLIGHTLY SLOWER THAN THE COMPOSITE, NEVER FASTER.
         *
         * Two free-running 165Hz sources cannot stay rate-locked -- separate GPUs, separate
         * display clocks -- so the pane queue must drift one way or the other. Which way is
         * NOT a matter of taste: a queue that overruns DROPS the oldest pane (coop_coord.c
         * :178) and tears a hole in the peer's prediction chain, while a queue that underruns
         * merely HOLDS the tile for one picture. Measured 2026-08-03 with both hosts at 165:
         * the peer ran ~3% fast, which is ~9 evictions a second, and the peer's high-detail
         * content carried 33% more mid-tone smear than ours on identical input -- visible as
         * chewed glyph edges and coloured speckle, while every counter read healthy.
         *
         * So bias the SECONDARY down a few fps and let the queue underrun on purpose. This is
         * the same "holds are safe / DROPS corrupt" rule the fps floor work already
         * established, applied to the one clock we can still choose. Override with
         * /tmp/coop-fps-peer. */
        if (coop_secondary_mode() != NULL) {
            int peer_fps = coop_fps > 8 ? coop_fps - 5 : coop_fps;
            FILE *pf = fopen("/tmp/coop-fps-peer", "r");
            if (pf != NULL) {
                int pv = 0;
                if (fscanf(pf, "%d", &pv) == 1 && pv >= 24 && pv <= 240) {
                    peer_fps = pv;
                }
                fclose(pf);
            }
            coop_fps = peer_fps;
        }
        if (config->stream.fps > coop_fps) {
            config->stream.fps = coop_fps;
            /* Keep the two consistent: sops/mode matching keys off the refresh rate, and
             * a host that switches its display to 165Hz while encoding 60 reintroduces
             * the mismatch we are removing. */
            config->stream.clientRefreshRateX100 = coop_fps * 100;
            FILE *lf = fopen(AURORA_DIAG_LOG, "a");
            if (lf != NULL) {
                fprintf(lf, "[COOP] fps cap: requesting %dfps from this host\n", coop_fps);
                fclose(lf);
            }
        }
    }
#if defined(TARGET_ANDROID)
    /* THE RATE THE TV WILL SHOW. The webOS build asks the host for its panel's NTSC rate (60 -> 59.94,
     * settings_reconcile_refresh_rate). An Android TV switches modes for the stream, so ask for the frame rate
     * itself unless the TV has no mode that shows it evenly but has the NTSC one. */
    {
        int rates[32];
        const int count = android_display_refresh_rates(rates, 32);
        const int x100 = android_stream_refresh_x100(config->stream.fps, config->stream.clientRefreshRateX100,
                                                     rates, count);
        if (x100 != config->stream.clientRefreshRateX100) {
            commons_log_info("Session", "The display shows %d FPS only as %.2f Hz: asking the host for that",
                             config->stream.fps, x100 / 100.0);
            config->stream.clientRefreshRateX100 = x100;
        }
    }
#endif
    config->vmouse = app_config->virtual_mouse;
    config->hardware_mouse = app_config->hardware_mouse;
    config->local_audio = app_config->localaudio;
    config->view_only = app_config->viewonly;
    config->sops = app_config->sops;
    if (app_config->stick_deadzone < 0) {
        config->stick_deadzone = 0;
    } else if (app_config->stick_deadzone > 100) {
        config->stick_deadzone = 100;
    } else {
        config->stick_deadzone = (uint8_t) app_config->stick_deadzone;
    }

    SS4S_VideoCapabilities video_cap = app->ss4s.video_cap;
    SS4S_AudioCapabilities audio_cap = app->ss4s.audio_cap;

    if (config->stream.bitrate < 0) {
        config->stream.bitrate = settings_optimal_bitrate(&video_cap, config->stream.width, config->stream.height,
                                                          config->stream.fps);
    }
#if !defined(TARGET_WEBOS)
    /* On webOS, ss4s drivers (e.g. NDL) advertise conservative maxBitrate; capping here undoes high-bitrate forks. */
    if (video_cap.maxBitrate && config->stream.bitrate > video_cap.maxBitrate) {
        config->stream.bitrate = (int) video_cap.maxBitrate;
    }
#endif
    if (video_cap.codecs & SS4S_VIDEO_H264) {
        config->stream.supportedVideoFormats |= VIDEO_FORMAT_H264;
    }
    if (app_config->hevc && video_cap.codecs & SS4S_VIDEO_H265) {
        config->stream.supportedVideoFormats |= VIDEO_FORMAT_H265;
        if (app_config->hdr && video_cap.hdr) {
            config->stream.supportedVideoFormats |= VIDEO_FORMAT_H265_MAIN10;
        }
    }
    if (app_config->av1 && video_cap.codecs & SS4S_VIDEO_AV1) {
        config->stream.supportedVideoFormats |= VIDEO_FORMAT_AV1_MAIN8;
        if (app_config->hdr && video_cap.hdr) {
            config->stream.supportedVideoFormats |= VIDEO_FORMAT_AV1_MAIN10;
        }
    }
    // If no video format is supported, default to H.264
    if (config->stream.supportedVideoFormats == 0) {
        config->stream.supportedVideoFormats = VIDEO_FORMAT_H264;
    }
    /* CO-OP: TWO INDEPENDENT GATES, AND KEEPING THEM APART IS THE POINT.
     *
     * (1) THE CODEC. A mixed-vendor pairing cannot share an SPS/PPS in HEVC at all
     * (NVENC codes 32x32 CTBs, AMF 64x64, neither is configurable), so the composite has
     * to be AVC. Restricted with a MASK rather than assigned, so any H.264 variant the
     * client and host agree on survives; clearing HEVC and AV1 is the whole intent, as
     * two panes returned in different codecs is not a degraded picture but a splice that
     * cannot happen.
     *
     * (2) HDR. Decided on its own evidence -- whether BOTH hosts advertise a 10-bit
     * codec -- and never inferred from (1). The user's `hdr` setting is overruled by the
     * pairing only when a participating host cannot do it. That a compatibility-mode
     * session ends up without HDR anyway is a consequence of the FORMAT LIST, not a
     * policy applied here: every 10-bit format in the protocol is HEVC or AV1
     * (VIDEO_FORMAT_MASK_10BIT), so restricting to H.264 in (1) leaves nothing 10-bit to
     * advertise. Written as two statements so that if a 10-bit H.264 ever exists, or a
     * mixed pair ever becomes HEVC-splicable, only the gate that changed has to move.
     *
     * BOTH GATES ARE RESOLVED BEFORE THE STREAM STARTS and never re-derived here: the
     * dialog decides from both hosts' reported facts, hands the answers to the secondary
     * through the environment, and a mid-session change would leave the two halves
     * disagreeing about the bitstream they are jointly building. */
    if (app_launch_coop_requested()) {
        config->stream.supportedVideoFormats &= VIDEO_FORMAT_MASK_H265;
        if (config->stream.supportedVideoFormats == 0 &&
                (video_cap.codecs & SS4S_VIDEO_H265) != 0)
            config->stream.supportedVideoFormats = VIDEO_FORMAT_H265;

        if (!coop_hdr_allowed_resolved()) {
            config->stream.supportedVideoFormats &= ~VIDEO_FORMAT_MASK_10BIT;
        }
    }
    if (video_cap.colorSpace & SS4S_VIDEO_CAP_COLORSPACE_BT2020 &&
        (config->stream.supportedVideoFormats & ~VIDEO_FORMAT_MASK_H264)) {
        config->stream.colorSpace = COLORSPACE_REC_2020;
    } else if (video_cap.colorSpace & SS4S_VIDEO_CAP_COLORSPACE_BT709) {
        config->stream.colorSpace = COLORSPACE_REC_709;
    } else {
        config->stream.colorSpace = COLORSPACE_REC_601;
    }
    if (app_launch_coop_requested() && coop_gpu_hevc_resolved() &&
            (video_cap.codecs & SS4S_VIDEO_H265) != 0) {
        // Both hosts must negotiate the same HEVC depth; no AVC/AV1 fallback.
        config->stream.supportedVideoFormats = VIDEO_FORMAT_H265;
        if (app_config->hdr && video_cap.hdr && coop_hdr_allowed_resolved())
            config->stream.supportedVideoFormats |= VIDEO_FORMAT_H265_MAIN10;
        config->stream.colorSpace = COLORSPACE_REC_709;
    }
    /* Full range (0–255) when HDR is on, like Moonlight Android; SDR uses limited range. */
    /* HDR10 (PQ/SMPTE ST 2084) always uses limited range by standard.
     * For SDR, honor force_full_color_range if the user has enabled it. */
    config->stream.colorRange = (!app_config->hdr && app_config->force_full_color_range &&
                                 video_cap.fullColorRange)
        ? COLOR_RANGE_FULL : COLOR_RANGE_LIMITED;
    if (!app_config->hdr && app_config->force_full_color_range && !video_cap.fullColorRange) {
        commons_log_warn("Session", "Full-range SDR requested but %s only supports limited range; "
                                    "using limited range",
                         SS4S_ModuleInfoGetName(app->ss4s.selection.video_module));
    }
    if (app_launch_coop_requested() && coop_gpu_hevc_resolved()) config->stream.colorRange = COLOR_RANGE_LIMITED;
#if FEATURE_SURROUND_SOUND
    if (audio_cap.maxChannels < CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(config->stream.audioConfiguration)) {
        switch (audio_cap.maxChannels) {
            case 2:
                config->stream.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
                break;
            case 6:
                config->stream.audioConfiguration = AUDIO_CONFIGURATION_51_SURROUND;
                break;
            case 8:
                config->stream.audioConfiguration = AUDIO_CONFIGURATION_71_SURROUND;
                break;
        }
    }
    if (!config->stream.audioConfiguration) {
        config->stream.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    }
#endif
    /* CO-OP IS STEREO, ON BOTH HOSTS.
     *
     * The two games' audio is summed sample by sample in the client (the TV grants
     * only one PCM out -- co-op spike #3), so the two streams must have the same
     * shape or there is nothing to add. Surround also cannot survive the mix in any
     * meaningful way: two independent 5.1 beds summed into one bed is not a
     * soundstage anybody wants out of a TV. Applied here because both sessions come
     * through this function, so they cannot disagree. */
    if (app_launch_coop_requested()) {
        config->stream.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    }
    config->stream.encryptionFlags = ENCFLG_AUDIO;
}

/**
 * Populate HDR info from stream configuration.
 * Corresponds to @p avcodec_colorspace_from_sunshine_colorspace in video_colorspace.cpp in Sunshine.
 *
 * @param info Info for SS4S_PlayerVideoSetHDRInfo
 * @param config Moonlight stream configuration
 */
static void populate_hdr_info_vui(SS4S_VideoHDRInfo *info, const STREAM_CONFIGURATION *config) {
    switch (config->colorSpace) {
        case COLORSPACE_REC_601:
            info->colorPrimaries = 6 /* SMPTE 170M */;
            info->transferCharacteristics = 6 /* SMPTE 170M */;
            info->matrixCoefficients = 6 /* SMPTE 170M */;
            break;
        case COLORSPACE_REC_709:
            info->colorPrimaries = 1 /* BT.709 */;
            info->transferCharacteristics = 1 /* BT.709 */;
            info->matrixCoefficients = 1 /* BT.709 */;
            break;
        case COLORSPACE_REC_2020: {
            info->colorPrimaries = 9 /* BT.2020 */;
            info->transferCharacteristics = 16 /* SMPTE ST 2084 */;
            info->matrixCoefficients = 9 /* BT.2020 NCL */;
            break;
        }
    }
    info->videoFullRange = config->colorRange == COLOR_RANGE_FULL;
}

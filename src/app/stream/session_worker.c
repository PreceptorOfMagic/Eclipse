#include "session_worker.h"
#include "session_priv.h"
#include "video/coop_secondary.h"
#include "video/coop_session.h"
#include "app.h"
#include "app_launch.h"
#include "util/bus.h"
#include "logging.h"
#include "util/diag_logs.h"
#include "errors.h"
#include "util/user_event.h"
#include "input/input_gamepad.h"
#include "stream/connection/session_connection.h"
#include "stream/audio/session_audio.h"
#include "stream/audio/coop_audio.h"
#include "stream/input/coop_input.h"
#include "stream/video/session_video.h"
#include "stream/video/coop_coord.h"
#include "app_session.h"
#include "backend/pcmanager/worker/worker.h"

#include <limits.h>
#include <string.h>
#include "wired_nic.h"
#include "backend/aurora_host_caps.h"
#include "platform/this_pc.h"
#include "backend/pcmanager.h"
#if TARGET_ANDROID
#include "platform/android/android_platform.h"
#endif
#ifdef _WIN32
#include "local_host_win.h"

#endif

/* ---- SUPPORT LOGS (2026-09-30): the stream's setup and its end, in words, for a log read blind ---- */

static const char *interrupt_reason_text(streaming_interrupt_reason_t reason) {
    switch (reason) {
        case STREAMING_INTERRUPT_USER:
            return "stopped by the user";
        case STREAMING_INTERRUPT_BACKGROUND:
            return "app went to the background";
        case STREAMING_INTERRUPT_QUIT:
            return "user quit the app on the host";
        case STREAMING_INTERRUPT_HOST:
            return "host ended the stream";
        case STREAMING_INTERRUPT_WATCHDOG:
            return "watchdog: the stream stopped responding";
        case STREAMING_INTERRUPT_NETWORK:
            return "connection lost";
        case STREAMING_INTERRUPT_DECODER:
            return "video decoder failed";
        default:
            return "error";
    }
}

static const char *conn_error_text(int code) {
    switch (code) {
        case ML_ERROR_GRACEFUL_TERMINATION:
            return "graceful termination";
        case ML_ERROR_NO_VIDEO_TRAFFIC:
            return "NO VIDEO TRAFFIC ever arrived (host did not send, or a firewall/route dropped it)";
        case ML_ERROR_NO_VIDEO_FRAME:
            return "video arrived but no complete frame could be assembled";
        case ML_ERROR_UNEXPECTED_EARLY_TERMINATION:
            return "host ended the stream right after it started (encoder/capture failure on the host?)";
        case ML_ERROR_PROTECTED_CONTENT:
            return "host is showing protected content";
        case ML_ERROR_FRAME_CONVERSION:
            return "host failed to convert a frame";
        default:
            return "network/transport error";
    }
}

static void session_log_start(const session_t *session, const SERVER_DATA *server, int app_id) {
    const STREAM_CONFIGURATION *c = &session->config.stream;
    const int f = c->supportedVideoFormats;
    commons_log_info("Session", "Session start: host %s at %s (%s), app %d \"%s\"; %dx%d@%d, %d kbps, packet %d, "
                                "remote %d, codecs offered 0x%x (%s%s%s%s%s%s), colour %d/%d, audio %d ch, host audio "
                                "%s, optimise %s",
                     server->hostname ? server->hostname : "?",
                     server->serverInfo.address ? server->serverInfo.address : "?", server->uuid ? server->uuid : "?",
                     app_id, session->app_name ? session->app_name : "?", c->width, c->height, c->fps, c->bitrate,
                     c->packetSize, c->streamingRemotely, f, (f & VIDEO_FORMAT_H264) ? " H.264" : "",
                     (f & VIDEO_FORMAT_H265) ? " HEVC" : "", (f & VIDEO_FORMAT_H265_MAIN10) ? " HEVC-Main10" : "",
                     (f & VIDEO_FORMAT_AV1_MAIN8) ? " AV1" : "", (f & VIDEO_FORMAT_AV1_MAIN10) ? " AV1-Main10" : "",
                     (f & (VIDEO_FORMAT_H264_HIGH8_444 | VIDEO_FORMAT_H265_REXT8_444 | VIDEO_FORMAT_H265_REXT10_444))
                     ? " 4:4:4" : "", c->colorSpace, c->colorRange,
                     CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(c->audioConfiguration),
                     session->config.local_audio ? "yes" : "no", session->config.sops ? "yes" : "no");
}

/* THE HOST'S SIDE OF THE STORY (Umbra /umbralog): the encoder, capture, audio and display lines that explain a
 * failure are only in the host's log -- e.g. 2026-09-29 "hevc_amf: dynamic range not supported" behind a TV's
 * "no video traffic". Fetched into the kept log at the end of every stream, so the one file a user sends has it. */
static void session_log_host(GS_CLIENT client, const SERVER_DATA *server) {
    if (!diag_logs_active() || client == NULL || server == NULL) {
        return;
    }
    if (!server->umbraCapsKnown || (server->umbraCaps & UMBRA_CAP_HOST_LOG) == 0) {
        commons_log_info("Session", "Host log: %s does not offer its log (needs Umbra with host-log support)",
                         server->hostname ? server->hostname : "the host");
        return;
    }
    char *text = NULL;
    gs_set_timeout(client, 8);
    const int rc = gs_get_host_log(client, server, 262144, &text);
    if (rc != GS_OK || text == NULL) {
        const char *err = NULL;
        gs_get_error(&err);
        commons_log_warn("Session", "Host log: could not fetch it from %s (%d %s)",
                         server->hostname ? server->hostname : "the host", rc, err ? err : "");
        free(text);
        return;
    }
    fprintf(stderr, "===== HOST LOG: %s (%s), fetched at the end of this stream =====\n%s%s===== END HOST LOG =====\n",
            server->hostname ? server->hostname : "?", server->serverInfo.address ? server->serverInfo.address : "?",
            text, text[0] != '\0' && text[strlen(text) - 1] != '\n' ? "\n" : "");
    fflush(stderr);
    free(text);
}

static void session_log_end(const session_t *session, int start_result, Uint32 started_ms) {
    const double secs = (SDL_GetTicks() - started_ms) / 1000.0;
    if (start_result == INT_MIN) {
        commons_log_info("Session", "Session end: FAILED before connecting, after %.1f s (launch refused or "
                                    "failed -- see the lines above)", secs);
    } else if (start_result != 0) {
        commons_log_info("Session", "Session end: FAILED TO START after %.1f s (moonlight %d)", secs, start_result);
    } else {
        commons_log_info("Session", "Session end after %.1f s: %s; connection %s%s%s", secs,
                         session->interrupted ? interrupt_reason_text(session->interrupt_reason) : "not interrupted",
                         session->conn_error_set ? "error " : "closed normally",
                         session->conn_error_set ? conn_error_text(session->conn_error) : "",
                         session->quitapp ? "; app quit on the host" : "");
        if (session->conn_error_set) {
            commons_log_info("Session", "  moonlight connection code %d (0x%x)", session->conn_error,
                             (unsigned) session->conn_error);
        }
    }
    vdec_stats_log_totals();
}

int session_worker(session_t *session) {
    app_t *app = session->app;
    session_set_state(session, STREAMING_CONNECTING);
    bus_pushevent(USER_STREAM_CONNECTING, NULL, NULL);
    streaming_error(session, GS_OK, "");
    PSERVER_DATA server = session->server;
    int appId = session->app_id;
    session->player = NULL;
    const Uint32 session_started_ms = SDL_GetTicks();
    int session_start_result = INT_MIN;   /* INT_MIN = never got as far as connecting (launch failed) */

#if FEATURE_INPUT_EVMOUSE
    if (!session->config.view_only && session->config.hardware_mouse) {
        session_evmouse_wait_ready(&session->input.evmouse);
    }
#endif

    commons_log_info("Session", "Launch app %d (host currentGame=%d)...", appId, server->currentGame);
    session_log_start(session, server, appId);
    if (session->config.stream.clientRefreshRateX100 > 0) {
        commons_log_info("Session",
                         "Stream mode %dx%dx%d, clientRefreshRateX100=%d (%.2f Hz), bitrate %d kbps",
                         session->config.stream.width, session->config.stream.height,
                         session->config.stream.fps, session->config.stream.clientRefreshRateX100,
                         session->config.stream.clientRefreshRateX100 / 100.0,
                         session->config.stream.bitrate);
    } else {
        commons_log_info("Session", "Stream mode %dx%dx%d, bitrate %d kbps (no fractional refresh rate)",
                         session->config.stream.width, session->config.stream.height,
                         session->config.stream.fps, session->config.stream.bitrate);
    }
#if TARGET_ANDROID
    /* The TV's mode follows the stream, as the webOS panel follows it by itself. Asked for before the host
     * starts sending, so the HDMI resync of a mode change is over before the first picture. Back to the
     * system's choice when the stream closes (ui/root.c). The co-op peer has no display of its own. */
    if (coop_secondary_mode() == NULL) {
        android_display_request_refresh(session->config.stream.clientRefreshRateX100 > 0
                                        ? session->config.stream.clientRefreshRateX100
                                        : session->config.stream.fps * 100);
    }
#endif
    /* app_gs_client_new asserts on app->backend.gs_client_mutex, which the
     * headless secondary never initialises -- it aborted here silently, before
     * LiStartConnection, which looked exactly like "connected but no frames".
     * Create the client directly for that path. */
    GS_CLIENT client = coop_secondary_mode() != NULL
            ? gs_new(app_configuration->key_dir)
            : app_gs_client_new(app);
    gs_set_gpu_hevc(client, app_launch_coop_requested() && coop_gpu_hevc_resolved());
    /* CO-OP-ONLY HOST TUNING, per launch: an Umbra host applies its co-op encoder settings to this
     * session and no other (UmbraCaps bit 2). Off in Settings > Co-op leaves the host as it is. */
    const bool host_tuning = app_launch_coop_requested() && app_configuration->coop_host_tuning;
    gs_set_coop(client, app_launch_coop_requested(), host_tuning);
    /* HOST MONITOR OFF (app_settings.h host_monitor_mode): only to a host that offers it (UmbraCaps bit 4), and
     * never to this PC -- its monitor is where this client's window is. */
    {
        const bool wanted = app_launch_host_monitor_off();
        const bool offered = server->umbraCapsKnown && (server->umbraCaps & UMBRA_CAP_MONITOR_OFF) != 0;
        const bool this_pc = local_host_is_this_pc(server->serverInfo.address);
        gs_set_host_monitor_off(client, wanted && offered && !this_pc);
        if (wanted) {
            commons_log_info("Session", "Host monitor off: %s", !offered ? "asked for, but this host does not offer it "
                                                                           "(needs Umbra) -- not sent"
                                                                : this_pc ? "asked for, but the host is this PC -- not sent"
                                                                          : "sent (umbraMonitorOff=1)");
            FILE *lf = fopen(coop_secondary_mode() != NULL ? AURORA_SECONDARY_LOG : AURORA_DIAG_LOG, "a");
            if (lf != NULL) {
                fprintf(lf, "[HOST-MONITOR] host=%s off_requested=1 offered=%d this_pc=%d sent=%d\n",
                        server->serverInfo.address, offered ? 1 : 0, this_pc ? 1 : 0, offered && !this_pc ? 1 : 0);
                fclose(lf);
            }
        }
    }
    if (app_launch_coop_requested()) {
        FILE *route_log = fopen(coop_secondary_mode() != NULL ? AURORA_SECONDARY_LOG : AURORA_DIAG_LOG, "a");
        if (route_log != NULL) {
            fprintf(route_log, "[COOP-ENCODER] role=%s encoder=%s formats=0x%x host=%s host_tuning=%d umbra_caps=%s%d bitrate=%d\n",
                    coop_secondary_mode() != NULL ? "secondary" : "primary",
                    coop_gpu_hevc_resolved() ? "gpu-hevc-beta" : "native-hevc",
                    session->config.stream.supportedVideoFormats, server->uuid, host_tuning ? 1 : 0,
                    server->umbraCapsKnown ? "" : "unreported/", server->umbraCaps,
                    session->config.stream.bitrate);
            fclose(route_log);
        }
    }
    const char *surround_params = NULL;
#ifdef _WIN32
    /* BEFORE the launch: the host's display hook reads it when this client connects. A host
     * that is this very machine keeps its panel on for our window (local_host_win.c). */
    if (local_host_session_begin(server->serverInfo.address)) {
        /* THIS PC PLAYS ITS OWN SOUND. Asked to stream without host audio, the host moves this
         * machine's default output to its virtual sink for the session -- the user's speakers go
         * silent, and they stayed on the virtual sink afterwards whenever the restore failed. With
         * host audio the default is never touched; the co-op mixer drops this host's copy of the
         * sound (coop_audio.c), since the speakers already play it. */
        /* ...EXCEPT IN CO-OP WITH A VIRTUAL SINK: then this PC's sound is streamed like any host's and mixed
         * with the other PC's (coop_audio_prepare_local_host pinned our output to the real speakers first). */
        session->config.local_audio = !(app_launch_coop_requested() && coop_audio_local_streamed());
        commons_log_info("Session", "Host %s is this machine: input to it blocked, local-host session announced, "
                         "its sound %s", server->serverInfo.address,
                         session->config.local_audio ? "plays locally" : "is streamed and mixed");
        FILE *lf = fopen(coop_secondary_mode() != NULL ? AURORA_SECONDARY_LOG : AURORA_DIAG_LOG, "a");
        if (lf != NULL) {
            fprintf(lf, "[LOCAL-HOST] host=%s is this machine -> input to it BLOCKED, local-host-session announced\n",
                    server->serverInfo.address);
            fclose(lf);
        }
    }
#endif
#if TARGET_WEBOS
    /* THE WIRED PIN (wired_nic.h, app_settings.h wired_pin_mode): resolved here, at the point of use. */
    if (app_configuration->wired_pin_mode != WIRED_PIN_OFF) {
        wired_nic_t nic;
        uuidstr_t host_id;
        uuidstr_fromstr(&host_id, server->uuid);
        const bool host_offers = umbra_wired_pin_on_host(&host_id);
        if (!wired_nic_discover(&nic)) {
            commons_log_info("Session", "Wired pin: no wired network card is up -- not asked for");
        } else if (app_configuration->wired_pin_mode == WIRED_PIN_AUTO && !host_offers) {
            commons_log_info("Session", "Wired pin: %s does not offer it (needs Umbra) -- not asked for", server->hostname);
        } else {
            gs_set_wired_pin(client, nic.mac, nic.own_macs, nic.own_ips);
            commons_log_info("Session", "Wired pin: asking %s to keep the stream on %s (%s); own %s / %s",
                             server->hostname, nic.name, nic.mac, nic.own_macs, nic.own_ips);
        }
    }
    if (session->config.stream.audioConfiguration == AUDIO_CONFIGURATION_51_SURROUND) {
        // 6 channels, 4 streams, 2 coupled streams, FL, FR, SL, SR, FC, LFE
        surround_params = "642014523";
    }
#endif
    int ret = gs_start_app(client, server, &session->config.stream, appId, server->isGfe, session->config.sops,
                           session->config.local_audio,
                           app_input_gamepads_mask(&app->input), surround_params);
    if (coop_secondary_mode() != NULL) {
        FILE *lf = fopen(AURORA_SECONDARY_LOG, "a");
        if (lf != NULL) {
            const char *ge = NULL;
            gs_get_error(&ge);
            fprintf(lf, "gs_start_app(app=%d) -> %d %s\n", appId, ret, ge ? ge : "");
            fclose(lf);
        }
    }
    if (ret != GS_OK) {
        session_set_state(session, STREAMING_ERROR);
        const char *gs_error = NULL;
        gs_get_error(&gs_error);
        if (coop_secondary_mode() != NULL) {
            /* HEADLESS: nothing here reaches the screen, so hand the reason to the primary. */
            coop_secondary_launch_failed(gs_error);
        }
        if (gs_error && strncmp(gs_error, "Bad status 403", 14) == 0) {
            /* APOLLO'S CLIENT PERMISSIONS. A newly paired client may list apps and watch but not
             * launch (Apollo's default perm 50331648 = list|view, MEASURED 2026-09-27), so the
             * first stream after pairing fails here -- say who refused and what the owner does. */
            /* Apollo keeps per-device permissions under PIN > Device Management, buttons "Launch
             * Apps" and "Controller Input" (Apollo source: Navbar.vue, pin.html, en.json -- there
             * is no Clients page; diff-sentinel 2026-09-27). */
            streaming_error(session, ret,
                            "%s refused to start the app (403 Permission denied). If it runs Umbra, open its "
                            "web UI, go to PIN > Device Management and allow Launch Apps and Controller Input "
                            "for this device.",
                            server->hostname ? server->hostname : "The computer");
        } else if (gs_error) {
            streaming_error(session, ret, "Failed to launch session: %s (code %d)", gs_error, ret);
        } else {
            streaming_error(session, ret, "Failed to launch session: gamestream returned %d", ret);
        }
        commons_log_error("Session", "Failed to launch session: gamestream returned %d, gs_error=%s", ret, gs_error);
        goto thread_cleanup;
    }

    commons_log_info("Session", "Audio %d channels",
                     CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(session->config.stream.audioConfiguration));

    /* Co-op secondary: this process feeds a pane to the coordinator and must
     * NOT open a player, claim a media pipeline or touch a surface -- that
     * shape is the only one that ever worked alongside a live primary. Its
     * video callbacks forward reassembled access units over the socket. */
    const bool coop_sec = coop_secondary_mode() != NULL;
    if (!coop_sec) {
        session->player = SS4S_PlayerOpen();
        SS4S_PlayerSetWaitAudioVideoReady(session->player, true);
        SS4S_PlayerSetViewportSize(session->player, app->ui.width, app->ui.height);
        SS4S_PlayerSetUserdata(session->player, app);
        session_audio_prepare_coop_output(session);
    }

    session_video_prepare_stream();

    /* CONTROLLER ROUTING. The coordinator listens BEFORE the connection so the
     * secondary can attach as soon as it starts; the secondary connects after,
     * and retries are not needed because the coordinator's socket outlives it. */
    if (!coop_sec) {
        coop_input_coord_start(&app->input);
    }

    int startResult = LiStartConnection(&server->serverInfo, &session->config.stream,
                                        coop_sec ? &coop_secondary_conn_callbacks
                                                 : session_connection_callbacks_prepare(session),
                                        coop_sec ? &coop_secondary_callbacks : &ss4s_dec_callbacks,
                                        /* The secondary used to pass NULL here and moonlight-common-c
                                         * filled in no-op fakes -- it received this host's audio off
                                         * the wire and threw it away. It now decodes it and ships PCM
                                         * to the coordinator to be mixed. Still no player and no media
                                         * pipeline: the second PCM out is what SPIKE #3 proved
                                         * impossible. */
                                        coop_sec ? &coop_secondary_aud_callbacks : &ss4s_aud_callbacks,
                                        session, 0, session, 0);
    session_start_result = startResult;
    if (coop_sec && startResult == 0) {
        /* Attach the input link only after the connection succeeded: LiSend* on a
         * dead session is meaningless, and a failed connect should not leave a
         * half-open socket the coordinator would keep feeding. */
        coop_input_secondary_start();
    }
    if (coop_sec) {
        /* The secondary has no UI to surface errors, and commons_log is not
         * reachable on the TV -- without this a failed connection is silent and
         * looks identical to "started but no frames". */
        FILE *lf = fopen(AURORA_SECONDARY_LOG, "a");
        if (lf != NULL) {
            fprintf(lf, "LiStartConnection -> %d (addr=%s)\n", startResult,
                    server->serverInfo.address ? server->serverInfo.address : "(null)");
            fclose(lf);
        }
    }
    if (startResult != 0) {
        session_set_state(session, STREAMING_ERROR);
        switch (startResult) {
            case CALLBACKS_SESSION_ERROR_VDEC_UNSUPPORTED:
                streaming_error(session, GS_WRONG_STATE, "Unsupported video codec.");
                break;
            case CALLBACKS_SESSION_ERROR_VDEC_ERROR:
                streaming_error(session, GS_WRONG_STATE, "Failed to open video decoder.");
                break;
            case CALLBACKS_SESSION_ERROR_COOP_START:
                streaming_error(session, GS_WRONG_STATE, "Could not start co-op: %s",
                                coop_coord_start_error_message());
                break;
            case CALLBACKS_SESSION_ERROR_ADEC_UNSUPPORTED:
                streaming_error(session, GS_WRONG_STATE, "Unsupported audio codec.");
                break;
            case CALLBACKS_SESSION_ERROR_ADEC_ERROR:
                streaming_error(session, GS_WRONG_STATE, "Failed to open audio backend.");
                break;
            default: {
                if (!streaming_errno) {
                    streaming_error(session, GS_WRONG_STATE, "Failed to start connection: Limelight returned %d (%s)",
                                    startResult, strerror(startResult));
                }
                break;
            }
        }
        commons_log_error("Session", "Failed to start connection: Limelight returned %d", startResult);
        goto thread_cleanup;
    }
    session_set_state(session, STREAMING_STREAMING);
    bus_pushevent(USER_STREAM_OPEN, NULL, NULL);
    SDL_LockMutex(session->mutex);
    while (!session->interrupted) {
        // Wait until interrupted
        SDL_CondWait(session->cond, session->mutex);
    }
    SDL_UnlockMutex(session->mutex);
    bus_pushevent(USER_STREAM_CLOSE, NULL, NULL);

    session_set_state(session, STREAMING_DISCONNECTING);

    /* Quit BEFORE LiStopConnection(), not after. webOS kills the app shortly after
     * it acknowledges a close request, and LiStopConnection() is slow when the link
     * is already unhealthy (it waits on ENet teardown). We were being killed part
     * way through it, so gs_quit_app() never ran -- "Sending app quit request" had
     * literally never appeared in the log. The host then never learned the app
     * should stop, so Apollo only PAUSED the session and left the app, its virtual
     * display and its detached commands running.
     *
     * Sending it first costs nothing: the host tears the session down anyway, and
     * LiStopConnection() still runs immediately after to close our side cleanly. */
    if (session->quitapp) {
        commons_log_info("Session", "Sending app quit request ...");
        /* Teardown tracing goes to the diag log: commons_log output is not
         * retrievable from the TV, and this window is short enough that "how far
         * did we get before webOS killed us" is the whole question. */
        FILE *qd = fopen(AURORA_DIAG_LOG, "a");
        if (qd) { fprintf(qd, "[QUIT@%u] primary quit ->\n", (unsigned) SDL_GetTicks()); fclose(qd); }
        /* BOUND IT TIGHTLY, and separately from the client's own timeout, which is
         * whatever gs_start_app last set (30 s on a fresh launch, 120 s on a resume).
         * This request is the last thing between the user and a closed stream, so
         * minutes of "Disconnecting..." is not an acceptable failure mode -- and a
         * quit the host never answers is not worth waiting on anyway, because
         * LiStopConnection() below closes our side regardless and Apollo tears the
         * session down when the client goes.
         *
         * 15 s, not less: a healthy quit is sub-second, but one measured here took
         * 8.2 s and still succeeded. */
        gs_set_timeout(client, 15);
        int qrc = gs_quit_app(client, server);
        qd = fopen(AURORA_DIAG_LOG, "a");
        if (qd) { fprintf(qd, "[QUIT@%u] primary quit rc=%d\n", (unsigned) SDL_GetTicks(), qrc); fclose(qd); }
        /* CO-OP: QUIT ENDS BOTH PANES. The co-op pane is resumable, so the secondary host only PAUSES
         * when its stream drops -- and the secondary process asks its host to quit only when
         * `quitappafter` is set. Quit is the user saying "end the session", so the primary quits the
         * peer's pane too, with the same paired client. Disconnect (quitapp=false) leaves both paused
         * for a resume. */
        if (!coop_sec && app_launch_coop_requested()) {
            const coop_session_t *cs = coop_session_get();
            if (cs != NULL && cs->secondary_uuid[0] != '\0') {
                uuidstr_t peer_id;
                uuidstr_fromstr(&peer_id, cs->secondary_uuid);
                const pclist_t *peer = pcmanager_node(pcmanager, &peer_id);
                int prc = -1;
                if (peer != NULL && peer->server != NULL) {
                    prc = gs_quit_app(client, (PSERVER_DATA) peer->server);
                }
                qd = fopen(AURORA_DIAG_LOG, "a");
                if (qd) {
                    fprintf(qd, "[QUIT@%u] co-op peer %s quit rc=%d%s\n", (unsigned) SDL_GetTicks(),
                            cs->secondary_uuid, prc, peer == NULL || peer->server == NULL ? " (peer not known)" : "");
                    fclose(qd);
                }
            }
        }
    }

    /* Release the input link before the connection goes: the secondary's receive
     * thread calls LiSend*, so it must be joined while the session is still valid. */
    if (coop_sec) {
        coop_input_secondary_stop();
    } else {
        coop_input_coord_stop();
    }
    LiStopConnection();
    worker_context_t update_ctx = {
            .app = app,
            .manager = pcmanager,
    };
    uuidstr_fromstr(&update_ctx.uuid, server->uuid);
    pcmanager_update_by_host(&update_ctx, server->serverInfo.address, server->extPort, true);

    // Don't always reset status as error state should be kept
    session_set_state(session, STREAMING_NONE);
    thread_cleanup:
    session_log_end(session, session_start_result, session_started_ms);
    if (coop_secondary_mode() == NULL) {
        session_log_host(client, server);
    }
#ifdef _WIN32
    local_host_session_end();
#endif
    session_connection_callbacks_reset(session);
    if (session->player != NULL) {
        session_audio_finish_output(session->player);
        SS4S_PlayerClose(session->player);
    }
    gs_destroy(client);
    bus_pushevent(USER_STREAM_FINISHED, NULL, NULL);
    app_bus_post(app, (bus_actionfunc) app_session_destroy, app);
    return 0;
}

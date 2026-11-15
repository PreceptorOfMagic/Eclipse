#include "app.h"
#include "app_session.h"
#include "streaming.controller.h"
#include "soft_keyboard.h"

#include <SDL.h>
#include "stream/video/session_video.h"
#include "stream/video/coop_trace.h"
#include "app_launch.h"
#include "pin_autoentry.h"
#include "ui/launcher/apps.controller.h"
#include "ui/root.h"
#include "ui/common/progress_dialog.h"
#include "lvgl/lv_ext_utils.h"
#include "lvgl/util/lv_app_utils.h"

#include "util/bus.h"
#include "stream/session.h"
#include "stream/input/session_input.h"
#include "util/user_event.h"
#include "util/i18n.h"
#include "logging.h"
#include "backend/pcmanager.h"
#include "platform/this_pc.h"
#include "platform/local_pane_session.h"
#include "stream/video/coop_session.h"

#include <stdio.h>
#include <string.h>
#if defined(TARGET_ANDROID)
#include "platform/android/android_platform.h"
#endif

static void exit_streaming(lv_event_t *event);

static void suspend_streaming(lv_event_t *event);

static void open_keyboard(lv_event_t *event);

static void toggle_vmouse(lv_event_t *event);
#if TARGET_ANDROID
static void send_guide(lv_event_t *event);
#endif

static void stream_fragment_del_timer_cb(lv_timer_t *timer);

static void hide_overlay(lv_event_t *event);
static void hide_overlay_impl(streaming_controller_t *controller);

static void on_cancel_key(lv_event_t *event);

static void soft_keyboard_close_cb(void *userdata);

static bool show_overlay(streaming_controller_t *controller);

static void close_start_wheel(streaming_controller_t *controller);

static void on_view_created(lv_fragment_t *self, lv_obj_t *view);

static void on_delete_obj(lv_fragment_t *self, lv_obj_t *view);

static void on_obj_deleted(lv_fragment_t *self, lv_obj_t *view);

static bool on_event(lv_fragment_t *self, int code, void *userdata);

static void constructor(lv_fragment_t *self, void *args);

static void controller_dtor(lv_fragment_t *self);

static void overlay_key_cb(lv_event_t *e);

static void update_buttons_layout(streaming_controller_t *controller);

static void pin_toggle(lv_event_t *e);

static void streaming_set_stats_pinned(streaming_controller_t *controller, bool pinned);

/** Pretty codec label for stats (matches session_video video_format_name strings). */
static const char *streaming_codec_display(const char *fmt) {
    if (fmt == NULL || fmt[0] == '\0') {
        return "-";
    }
    if (strcmp(fmt, "H264") == 0) {
        return "H.264";
    }
    if (strcmp(fmt, "H265") == 0) {
        return "H.265";
    }
    if (strcmp(fmt, "H265 10bit") == 0) {
        return "H.265 10bit";
    }
    if (strcmp(fmt, "AV1 8bit") == 0) {
        return "AV1";
    }
    if (strcmp(fmt, "AV1 10bit") == 0) {
        return "AV1 10bit";
    }
    return fmt;
}

/** Compact codec label matching Moonlight Qt (e.g. "AV1 10-bit", "H.265"). */
static const char *streaming_codec_compact_text(const char *fmt) {
    if (fmt == NULL || fmt[0] == '\0') {
        return "-";
    }
    if (strcmp(fmt, "H264") == 0) {
        return "H.264";
    }
    if (strcmp(fmt, "H265") == 0) {
        return "H.265";
    }
    if (strcmp(fmt, "H265 10bit") == 0) {
        return "H.265 10-bit";
    }
    if (strcmp(fmt, "AV1 8bit") == 0) {
        return "AV1";
    }
    if (strcmp(fmt, "AV1 10bit") == 0) {
        return "AV1 10-bit";
    }
    return fmt;
}

static float streaming_render_fps(float decodedFps) {
    float renderFps = decodedFps;
#if defined(TARGET_WEBOS)
    int displayRate = 60;
    if (SDL_webOSGetRefreshRate(&displayRate) && displayRate > 0 && renderFps > (float) displayRate) {
        renderFps = (float) displayRate;
    }
#elif defined(TARGET_ANDROID)
    /* The mode the TV is in NOW: the activity switches it for the stream, which SDL's display list does not
     * follow (platform/android). */
    int refresh_x100 = 0;
    if (android_display_mode(NULL, NULL, &refresh_x100, NULL, NULL) && refresh_x100 > 0 &&
        renderFps > (float) refresh_x100 / 100.0f) {
        renderFps = (float) refresh_x100 / 100.0f;
    }
#else
    SDL_DisplayMode mode;
    int display = 0;
    if (global != NULL && global->ui.window != NULL) {
        const int current = SDL_GetWindowDisplayIndex(global->ui.window);
        if (current >= 0) {
            display = current;
        }
    }
    if (SDL_GetCurrentDisplayMode(display, &mode) == 0 && mode.refresh_rate > 0
        && renderFps > (float) mode.refresh_rate) {
        renderFps = (float) mode.refresh_rate;
    }
#endif
    return renderFps;
}

static void network_test_timer_cb(lv_timer_t *timer) {
    streaming_controller_t *controller = (streaming_controller_t *) timer->user_data;
    if (!controller) {
        return;
    }
    if (controller->network_test_timer) {
        lv_timer_del(controller->network_test_timer);
        controller->network_test_timer = NULL;
    }

    const VIDEO_STATS *dst = &vdec_summary_stats;
    const VIDEO_INFO *info = &vdec_stream_info;
    (void) info;

    uint32_t measuredBps = dst->currentBitrateKbps;
    float dropPct = 0.0f;
    if (dst->totalFrames > 0) {
        dropPct = (float) dst->networkDroppedFrames * 100.0f / (float) dst->totalFrames;
    }

    char body[256];
    if (measuredBps == 0) {
        snprintf(body, sizeof(body),
                 "%s\n\n%s",
                 locstr("Network speed test could not gather enough data."),
                 locstr("Check your connection and try again."));
    } else {
        /* Converter para Mbps e aplicar margem de segurança de ~20% */
        float measuredMbps = (float) measuredBps / 1000000.0f;
        float recommendedMbps = measuredMbps * 0.8f;
        snprintf(body, sizeof(body),
                 "%s\n\n"
                 "%s: %.1f Mbps\n"
                 "%s: %.1f Mbps\n"
                 "%s: %u ms\n"
                 "%s: %.2f%%",
                 locstr("Network speed test finished."),
                 locstr("Measured throughput"), measuredMbps,
                 locstr("Recommended max bitrate"), recommendedMbps,
                 locstr("Network RTT (avg)"), dst->rtt,
                 locstr("Network frame drop"), dropPct);
    }

    static const char *btns[] = { "OK", "" };
    lv_obj_t *dialog = lv_msgbox_create_i18n(NULL,
                                             locstr("Network speed test"),
                                             body,
                                             btns,
                                             false);
    lv_obj_center(dialog);

    /* Encerra o streaming após o teste */
    if (controller->global && controller->global->session) {
        session_interrupt(controller->global->session, true, STREAMING_INTERRUPT_USER);
    }
}

static void network_test_timer_cb(lv_timer_t *timer);

const lv_fragment_class_t streaming_controller_class = {
        .constructor_cb = constructor,
        .destructor_cb = controller_dtor,
        .create_obj_cb = streaming_scene_create,
        .obj_created_cb = on_view_created,
        .obj_will_delete_cb = on_delete_obj,
        .obj_deleted_cb = on_obj_deleted,
        .event_cb = on_event,
        .instance_size = sizeof(streaming_controller_t),
};

static bool overlay_showing = false, overlay_pinned = false;

/* CO-OP WITH THIS PC'S SIDE RUN LOCALLY: that half of the window is a window of its own above everything drawn
 * here (platform/local_pane_session.h), so it is hidden for as long as something of ours must be seen across the
 * whole picture -- the stream menu, the on-screen keyboard, the Start wheel. Called wherever one of the three
 * appears or goes; it only acts on a change. */
static void local_view_sync(streaming_controller_t *controller) {
    if (!app_launch_local_pane()) {
        return;
    }
    const bool wheel = controller->wheel.root != NULL && !lv_obj_has_flag(controller->wheel.root, LV_OBJ_FLAG_HIDDEN);
    local_pane_session_ui_overlay(controller->global, overlay_showing || controller->soft_kbd != NULL || wheel);
}

/* Poll cadence and backstop for holding the loading dialog until the co-op composite emits.
   250ms is imperceptible against a startup that already takes seconds; 15s is far longer than
   any measured bootstrap (the live horizontal session spliced within ~1s of stream open) and
   exists only so a composite that never starts cannot pin the dialog and trap the user. */
#define COOP_FIRST_PICTURE_POLL_MS 250
#define COOP_FIRST_PICTURE_TIMEOUT_MS 15000

/* HOW TO LEAVE, SAID UP FRONT (user 2026-11-03: in host-and-client co-op "things like alt tab wouldn't work we
 * need fallbacks to close a stream to avoid them being stuck"). The Windows key and Alt+Tab now stay inside the
 * session (local_input_win.c), so the way out has to be shown, not discovered. */
#define EXIT_HINT_MS 10000

static bool coop_host_is_this_pc(const char *uuid) {
    if (uuid == NULL || uuid[0] == '\0') {
        return false;
    }
    uuidstr_t id;
    uuidstr_fromstr(&id, uuid);
    const pclist_t *node = pcmanager_node(pcmanager, &id);
    return node != NULL && node->server != NULL && local_host_is_this_pc(node->server->serverInfo.address);
}

static void exit_hint_close(streaming_controller_t *controller) {
    if (controller->exit_hint_timer != NULL) {
        lv_timer_del(controller->exit_hint_timer);
        controller->exit_hint_timer = NULL;
    }
    if (controller->exit_hint != NULL) {
        lv_obj_del(controller->exit_hint);
        controller->exit_hint = NULL;
    }
}

static void exit_hint_timer_cb(lv_timer_t *timer) {
    exit_hint_close(timer->user_data);
}

static void exit_hint_show(streaming_controller_t *controller) {
    const coop_session_t *pair = coop_session_get();
    /* This PC's side run locally (app_launch.h) is the same situation -- this PC's game has the screen -- reached
     * without a pair of streams. */
    const bool local_pane = app_launch_local_pane();
    if (controller->exit_hint != NULL) {
        return;
    }
    if (!local_pane && (!app_launch_coop_requested() || pair == NULL ||
                        !(coop_host_is_this_pc(pair->primary_uuid) || coop_host_is_this_pc(pair->secondary_uuid)))) {
        return;
    }
    lv_obj_t *hint = lv_label_create(controller->detached_root);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    /* IN PIXELS, NOT A PERCENTAGE OF THE PARENT. MEASURED 2026-11-13 on SKYNET (ffmpeg capture of the stream's half):
     * in a local-pane session this label came out ~40 px wide -- a black, rounded, full-height bar in the middle of
     * the stream's half for its 10 s, the "line" users saw at every start. */
    lv_obj_set_width(hint, lv_disp_get_hor_res(NULL) * (local_pane && app_launch_local_pane_vertical() ? 2 : 3) / 5);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(hint, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(hint, LV_OPA_80, 0);
    lv_obj_set_style_radius(hint, LV_DPX(10), 0);
    lv_obj_set_style_pad_all(hint, LV_DPX(14), 0);
    lv_label_set_text(hint, locstr("To leave this co-op session: Ctrl+Alt+Shift+Q, or LB + RB + View + Start on a "
                                   "controller for the stream menu, or right-click Eclipse on the taskbar. "
                                   "Ctrl+Alt+Shift+Z brings the mouse back from this PC's game."));
    if (local_pane) {
        /* This PC's half is covered by its own display's view (local_view_sync above), so the note goes where it
         * can be read: over the stream's half. */
        const int side = app_launch_local_pane_first() ? 1 : -1;
        if (app_launch_local_pane_vertical()) {
            lv_obj_align(hint, LV_ALIGN_TOP_MID, side * lv_disp_get_hor_res(NULL) / 4, LV_DPX(24));
        } else {
            lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, (side > 0 ? lv_disp_get_ver_res(NULL) / 2 : 0) + LV_DPX(24));
        }
    } else {
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, LV_DPX(24));
    }
    controller->exit_hint = hint;
    controller->exit_hint_timer = lv_timer_create(exit_hint_timer_cb, EXIT_HINT_MS, controller);
    lv_timer_set_repeat_count(controller->exit_hint_timer, 1);
}

/* A SLOW SECOND PC IS ASKED ABOUT, NOT DROPPED (user 2026-11-10: "a period in-between the hard timeout and the
 * expected connection time where a prompt appears to the user ... X pc is taking longer than expected to connect with
 * two buttons ... wait or end the attempt right away"). The stream no longer dies after ~8 s (the second PC is
 * awaited beside it, session_video.c); past COOP_SECONDARY_SLOW_MS this asks, and COOP_SECONDARY_READY_TIMEOUT_MS
 * still ends the attempt with the co-op error if nobody answers. The prompt owns the input while it is up
 * (ui_should_block_input), so keyboard, mouse and controllers reach it instead of the host. */
static bool slow_prompt_showing = false;

bool streaming_coop_slow_prompt_shown() {
    return slow_prompt_showing;
}

static const char *coop_slow_pc_name(void) {
    const coop_session_t *pair = coop_session_get();
    if (pair != NULL && pair->secondary_uuid[0] != '\0') {
        uuidstr_t id;
        uuidstr_fromstr(&id, pair->secondary_uuid);
        const pclist_t *node = pcmanager_node(pcmanager, &id);
        if (node != NULL && node->server != NULL && node->server->hostname != NULL &&
            node->server->hostname[0] != '\0') {
            return node->server->hostname;
        }
    }
    return locstr("The second PC");
}

static void slow_prompt_set_text(streaming_controller_t *controller, long waited_ms) {
    long left_s = (COOP_SECONDARY_READY_TIMEOUT_MS - waited_ms + 999) / 1000;
    if (left_s < 0) {
        left_s = 0;
    }
    if (left_s == controller->slow_shown_left_s) {
        return;
    }
    controller->slow_shown_left_s = left_s;
    char text[384];
    snprintf(text, sizeof(text), locstr("%s is taking longer than expected to connect.\n\n"
                                        "Keep waiting, or end this co-op attempt now? "
                                        "If it has not connected in %ld s, the attempt ends by itself."),
             coop_slow_pc_name(), left_s);
    lv_label_set_text(lv_msgbox_get_text(controller->slow_prompt), text);
}

static void slow_prompt_close(streaming_controller_t *controller, bool regrab) {
    if (controller->slow_prompt == NULL) {
        return;
    }
    lv_obj_t *prompt = controller->slow_prompt;
    controller->slow_prompt = NULL;
    slow_prompt_showing = false;
    app_input_set_group(&controller->global->ui.input, controller->group);
    if (controller->slow_group != NULL) {
        lv_group_del(controller->slow_group);
        controller->slow_group = NULL;
    }
    lv_msgbox_close_async(prompt);
    if (regrab) {
        app_set_mouse_grab(&controller->global->input, true);
    }
}

static void slow_prompt_answer(streaming_controller_t *controller, bool end) {
    commons_log_info("Streaming", "Co-op: the second PC was slow to connect -- user chose %s",
                     end ? "End" : "Wait");
    controller->slow_answered = true;
    slow_prompt_close(controller, !end);
    if (end && controller->global->session != NULL) {
        session_interrupt(controller->global->session, false, STREAMING_INTERRUPT_USER);
    }
}

static void slow_prompt_clicked_cb(lv_event_t *e) {
    streaming_controller_t *controller = lv_event_get_user_data(e);
    lv_obj_t *prompt = lv_event_get_current_target(e);
    if (prompt != controller->slow_prompt) {
        return;
    }
    const uint16_t picked = lv_btnmatrix_get_selected_btn(lv_msgbox_get_btns(prompt));
    slow_prompt_answer(controller, picked == 1);
}

/* B / Esc closes the question as Wait: backing out of a dialog must never end a session the user may want. */
static void slow_prompt_cancel_cb(lv_event_t *e) {
    streaming_controller_t *controller = lv_event_get_user_data(e);
    if (controller->slow_prompt != NULL) {
        slow_prompt_answer(controller, false);
    }
}

static void slow_prompt_show(streaming_controller_t *controller, long waited_ms) {
    static const char *btns[] = {translatable("Wait"), translatable("End"), ""};
    /* Not "": LVGL's msgbox creates no text label for empty text, so lv_msgbox_get_text() was NULL and the next
     * line crashed every client the moment the prompt opened (SIGSEGV in lv_label_set_long_mode, measured
     * 2026-11-14 on the Linux Flatpak with a second PC 28 s slow). slow_prompt_set_text() replaces it at once. */
    lv_obj_t *prompt = lv_msgbox_create_i18n(NULL, locstr("Still connecting"), " ", btns, false);
    lv_obj_set_width(prompt, LV_PCT(60));
    lv_obj_t *text = lv_msgbox_get_text(prompt);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(text, LV_PCT(100));
    controller->slow_prompt = prompt;
    controller->slow_shown_left_s = -1;
    slow_prompt_set_text(controller, waited_ms);
    lv_obj_center(prompt);
    lv_obj_add_event_cb(prompt, slow_prompt_clicked_cb, LV_EVENT_VALUE_CHANGED, controller);
    lv_obj_t *buttons = lv_msgbox_get_btns(prompt);
    lv_obj_add_event_cb(buttons, slow_prompt_cancel_cb, LV_EVENT_CANCEL, controller);
    controller->slow_group = lv_group_create();
    lv_group_add_obj(controller->slow_group, buttons);
    lv_btnmatrix_set_selected_btn(buttons, 0);
    app_input_set_group(&controller->global->ui.input, controller->slow_group);
    lv_group_focus_obj(buttons);
    slow_prompt_showing = true;
    app_set_mouse_grab(&controller->global->input, false);
    commons_log_info("Streaming", "Co-op: %s has not connected after %ld ms -- asking the user",
                     coop_slow_pc_name(), waited_ms);
}

static void coop_first_picture_cb(lv_timer_t *timer) {
    streaming_controller_t *controller = (streaming_controller_t *) timer->user_data;
    if (controller == NULL) {
        lv_timer_del(timer);
        return;
    }
    const long awaited_ms = coop_secondary_wait_elapsed_ms();
    if (awaited_ms >= 0) {
        /* The composite cannot anchor before the second PC sends a frame, so the backstop below would only
         * uncover a half-empty picture: it starts counting once the second PC is in. */
        if (controller->slow_prompt != NULL) {
            slow_prompt_set_text(controller, awaited_ms);
        } else if (awaited_ms >= COOP_SECONDARY_SLOW_MS && !controller->slow_answered) {
            slow_prompt_show(controller, awaited_ms);
        }
        return;
    }
    if (controller->slow_prompt != NULL) {
        commons_log_info("Streaming", "Co-op: the second PC connected while the slow-connect prompt was up");
        slow_prompt_close(controller, true);
    }
    controller->coop_wait_ms += COOP_FIRST_PICTURE_POLL_MS;
    const bool started = coop_composite_started();
    if (!started && controller->coop_wait_ms < COOP_FIRST_PICTURE_TIMEOUT_MS) {
        return;
    }
    commons_log_info("Streaming", "Co-op loading dialog released after %u ms (%s)",
                     controller->coop_wait_ms, started ? "composite anchored" : "backstop");
    if (controller->progress != NULL) {
        lv_msgbox_close(controller->progress);
        controller->progress = NULL;
    }
    lv_obj_add_flag(controller->hint, LV_OBJ_FLAG_HIDDEN);
    controller->coop_wait_timer = NULL;
    lv_timer_del(timer);
}
static streaming_controller_t *current_controller = NULL;

bool streaming_overlay_shown() {
    return overlay_showing;
}

bool streaming_soft_keyboard_shown() {
    return current_controller != NULL && current_controller->soft_kbd != NULL;
}


bool streaming_stats_shown() {
    return overlay_showing || overlay_pinned;
}

bool streaming_refresh_stats() {
    streaming_controller_t *controller = current_controller;
    if (!controller) { return false; }
    if (!streaming_stats_shown()) {
        return false;
    }
    app_t *app = controller->global;
    const struct VIDEO_STATS *dst = &vdec_summary_stats;
    const struct VIDEO_INFO *info = &vdec_stream_info;

    if (controller->stats_compact_label != NULL) {
        char stats_line[384];

        const char *codec = streaming_codec_compact_text(vdec_stream_info.format);
        const char *hdr_suffix = app_configuration->hdr ? " HDR" : "";
        int w = info->width > 0 ? info->width : 0;
        int h = info->height > 0 ? info->height : 0;
        float renderFps = streaming_render_fps(dst->decodedFps);
        float lossPct = (dst->totalFrames > 0)
            ? (float) dst->networkDroppedFrames / (float) dst->totalFrames * 100.0f
            : 0.0f;
        float bitrateMbps = (float) dst->currentBitrateKbps / 1000000.0f;

        float hostMs = 0.0f;
        float submitMs = 0.0f;
        float decOnlyMs = 0.0f;
        bool have_render = false;
        bool have_decode = false;
        bool have_encode = false;
        if (dst->submittedFrames) {
            submitMs = (float) dst->totalSubmitTime / (float) dst->submittedFrames;
            have_render = true;
            if (vdec_stream_info.has_host_latency) {
                hostMs = (float) dst->totalCaptureLatency / (float) dst->submittedFrames / 10.0f;
                have_encode = true;
            }
            if (vdec_stream_info.has_decoder_latency) {
                decOnlyMs = dst->avgDecoderLatency;
                have_decode = true;
            }
        }
#ifdef _WIN32
        /* On Windows the decode runs inside the submit call (#32), so it is already in submitMs --
         * adding it again would turn the quality dot yellow on a healthy stream. */
        float totalMs = (float) dst->rtt + hostMs + submitMs;
#else
        float totalMs = (float) dst->rtt + hostMs + submitMs + decOnlyMs;
#endif

        int len = snprintf(stats_line, sizeof(stats_line),
                           "%dx%d %s%s FPS %.1f Rx \xb7 %.1f De \xb7 %.1f Rd "
                           "N %u \xb1 %ums FD %.2f%% BW %.2f Mbps",
                           w, h, codec, hdr_suffix,
                           dst->receivedFps, dst->decodedFps, renderFps,
                           (unsigned) dst->rtt, (unsigned) dst->rttVariance,
                           lossPct, bitrateMbps);
        if (len > 0 && (size_t) len < sizeof(stats_line) && (have_render || have_decode || have_encode)) {
            len += snprintf(stats_line + len, sizeof(stats_line) - (size_t) len, " |");
            bool first = true;
            if (have_render && len > 0 && (size_t) len < sizeof(stats_line)) {
                len += snprintf(stats_line + len, sizeof(stats_line) - (size_t) len,
                                "%sS %.2fms", first ? "" : " \xb7 ", submitMs);
                first = false;
            }
            if (have_decode && len > 0 && (size_t) len < sizeof(stats_line)) {
#ifdef _WIN32
                /* Part of S, not a further term (#32): "S 4.10ms (D 3.20ms)". */
                if (have_render) {
                    len += snprintf(stats_line + len, sizeof(stats_line) - (size_t) len, " (D %.2fms)", decOnlyMs);
                } else {
                    len += snprintf(stats_line + len, sizeof(stats_line) - (size_t) len,
                                    "%sD %.2fms", first ? "" : " \xb7 ", decOnlyMs);
                }
#else
                len += snprintf(stats_line + len, sizeof(stats_line) - (size_t) len,
                                "%sD %.2fms", first ? "" : " \xb7 ", decOnlyMs);
#endif
                first = false;
            }
            if (have_encode && len > 0 && (size_t) len < sizeof(stats_line)) {
                snprintf(stats_line + len, sizeof(stats_line) - (size_t) len,
                         "%sEn %.1fms", first ? "" : " \xb7 ", hostMs);
            }
        }
        /* CO-OP IDENTITY SEGMENT -- passover 07 §3.4's TV-local overlay. Appended to the
         * compact line rather than given its own widget so there is exactly one thing to
         * point a camera at, and so it inherits the overlay's existing show/pin controls.
         *   P    the primary frame number just fed
         *   x    the peer frame number last ACCEPTED into a composite
         *   /    the peer frame number last RECEIVED by the coordinator
         *   t    CLOCK_MONOTONIC ms -- THE SAME CLOCK the per-picture trace stamps with, so a
         *        photograph of this line can be aligned to a trace row exactly.
         * A widening gap between accepted and received is the reading that decides passover
         * 07 §2B; a frozen accepted identity under a climbing t is the reading that decides
         * whether a held pane is being mistaken for latency. */
        {
            uint32_t idp = 0, ida = 0, idr = 0;
            unsigned long idt = 0;
            if (coop_trace_identity_get(&idp, &ida, &idr, &idt)) {
                size_t at = strlen(stats_line);
                if (at < sizeof(stats_line)) {
                    snprintf(stats_line + at, sizeof(stats_line) - at,
                             " | P%u x%u/%u t%lu", idp, ida, idr, idt);
                }
            }
        }
        lv_label_set_text(controller->stats_compact_label, stats_line);
        /* Quality dot: green ≤25ms, yellow ≤30ms, red >30ms */
        if (controller->stats_quality_indicator) {
            lv_color_t qc = totalMs <= 25.0f ? lv_palette_main(LV_PALETTE_GREEN)
                          : totalMs <= 30.0f ? lv_palette_main(LV_PALETTE_YELLOW)
                          : lv_palette_main(LV_PALETTE_RED);
            lv_obj_set_style_text_color(controller->stats_quality_indicator, qc, 0);
        }
        return true;
    }

    const char *hdr_str = app_configuration->hdr ? "HDR" : "SDR";
    const char *codec = streaming_codec_display(vdec_stream_info.format);
    if (info->width > 0 && info->height > 0) {
        lv_label_set_text_fmt(controller->stats_items.decoder, "%d\u00d7%d %s %s (%s)", info->width, info->height, hdr_str,
                              codec, SS4S_ModuleInfoGetId(app->ss4s.selection.video_module));
    } else {
        lv_label_set_text_fmt(controller->stats_items.decoder, "%s %s (%s)", codec, hdr_str,
                              SS4S_ModuleInfoGetId(app->ss4s.selection.video_module));
    }
    lv_label_set_text_fmt(controller->stats_items.audio, "%s, %s (%s)", audio_stream_info.format,
                          audio_stream_info.channels, SS4S_ModuleInfoGetId(app->ss4s.selection.audio_module));
    lv_label_set_text_fmt(controller->stats_items.rtt, "%d ms (var. %d ms)", dst->rtt, dst->rttVariance);
    lv_label_set_text_fmt(controller->stats_items.net_fps, "%.2f FPS", dst->receivedFps);
    float renderFps = streaming_render_fps(dst->decodedFps);
    lv_label_set_text_fmt(controller->stats_items.render_fps, "%.2f FPS", renderFps);
    lv_label_set_text_fmt(controller->stats_items.bitrate, "%u Mbps", dst->currentBitrateKbps / 1000000);

        if (dst->submittedFrames) {
            lv_label_set_text_fmt(controller->stats_items.drop_rate, "%.2f%%",
                                  (float) dst->networkDroppedFrames / (float) dst->totalFrames * 100);
            if (vdec_stream_info.has_host_latency) {
                float avgCapLatency = (float) dst->totalCaptureLatency / (float) dst->submittedFrames / 10.0f;
                lv_label_set_text_fmt(controller->stats_items.host_latency, "avg %.2f ms", avgCapLatency);
            } else {
                lv_label_set_text_fmt(controller->stats_items.host_latency, "not available");
            }
            if (vdec_stream_info.has_decoder_latency) {
                float avgSubmitTime = (float) dst->totalSubmitTime / (float) dst->submittedFrames;
#ifdef _WIN32
                /* The Windows decoder runs INSIDE the submit call (ss4s ffmpegvid.c Feed decodes,
                 * reads back and converts before returning), so "submit" already contains
                 * "decode": show it as a part, not a sum (#32). */
                lv_label_set_text_fmt(controller->stats_items.vdec_latency, "submit %.2f ms (decode %.2f ms)",
                                      avgSubmitTime, dst->avgDecoderLatency);
#else
                lv_label_set_text_fmt(controller->stats_items.vdec_latency, "submit %.2f ms + decode %.2f ms",
                                      avgSubmitTime, dst->avgDecoderLatency);
#endif
            } else {
                lv_label_set_text_fmt(controller->stats_items.vdec_latency, "not available");
            }
    } else {
        lv_label_set_text(controller->stats_items.drop_rate, "-");
        lv_label_set_text_fmt(controller->stats_items.host_latency, "-");
        lv_label_set_text_fmt(controller->stats_items.vdec_latency, "-");
    }
    return true;
}

void streaming_notice_show(const char *message) {
    streaming_controller_t *controller = current_controller;
    if (!controller) { return; }
    lv_label_set_text(controller->notice_label, message);
    if (message && message[0]) {
        lv_obj_clear_flag(controller->notice, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(controller->notice, LV_OBJ_FLAG_HIDDEN);
    }
}

static void constructor(lv_fragment_t *self, void *args) {
    streaming_controller_t *controller = (streaming_controller_t *) self;
    current_controller = controller;

    overlay_showing = false;

    streaming_styles_init(controller);

    const streaming_scene_arg_t *arg = (streaming_scene_arg_t *) args;
    controller->global = arg->global;
    controller->uuid = arg->uuid;
    controller->network_test = arg->network_test;
    controller->network_test_duration = arg->network_test_duration ? arg->network_test_duration : 10;
    controller->network_test_timer = NULL;
    if (app_session_begin(arg->global, &arg->uuid, &arg->app) != 0) {
        commons_log_error("Streaming", "Failed to start session");
        lv_async_call((lv_async_cb_t) lv_fragment_del, controller);
    }
}

static void controller_dtor(lv_fragment_t *self) {
    streaming_controller_t *fragment = (streaming_controller_t *) self;
    if (fragment->network_test_timer) {
        lv_timer_del(fragment->network_test_timer);
        fragment->network_test_timer = NULL;
    }
    /* The co-op first-picture poll holds a pointer to this fragment; a session torn down
       before the composite ever emitted (a failed launch, or the user backing out) would
       otherwise leave it firing into freed memory. */
    if (fragment->coop_wait_timer) {
        lv_timer_del(fragment->coop_wait_timer);
        fragment->coop_wait_timer = NULL;
    }
    streaming_styles_reset(fragment);
    if (current_controller == fragment) {
        current_controller = NULL;
    }
    fragment->soft_kbd = NULL; /* Will be deleted with parent */
}

static bool on_event(lv_fragment_t *self, int code, void *userdata) {
    LV_UNUSED(userdata);
    streaming_controller_t *controller = (streaming_controller_t *) self;
    switch (code) {
        case USER_STREAM_CONNECTING: {
            controller->progress = progress_dialog_create(locstr("Connecting..."));
            if (lv_obj_check_type(controller->progress->parent, &lv_msgbox_backdrop_class)) {
                lv_obj_set_style_bg_opa(controller->progress->parent, LV_OPA_TRANSP, 0);
            }
            lv_obj_set_width(controller->progress, LV_DPX(300));
            lv_obj_set_height(controller->progress, LV_DPX(100));
            lv_obj_add_flag(controller->overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(controller->hint, LV_OBJ_FLAG_HIDDEN);
            if (app_launch_local_pane()) {
                /* THIS PC'S HALF IS ITS OWN WINDOW ABOVE OURS (from the stream's open): the loader and the hint go
                 * in the stream's half, not across the divider (user 2026-11-13: the loading circle looked forced
                 * into the left half and broken). */
                const lv_coord_t hw = lv_disp_get_hor_res(NULL) / 4, hh = lv_disp_get_ver_res(NULL) / 4;
                const int side = app_launch_local_pane_first() ? 1 : -1;
                const bool vertical = app_launch_local_pane_vertical();
                lv_obj_align(controller->progress, LV_ALIGN_CENTER, vertical ? side * hw : 0, vertical ? 0 : side * hh);
                lv_label_set_long_mode(controller->hint, LV_LABEL_LONG_WRAP);
                lv_obj_set_width(controller->hint, vertical ? LV_PCT(45) : LV_PCT(90));
                lv_obj_align(controller->hint, side > 0 ? LV_ALIGN_BOTTOM_RIGHT
                                                        : (vertical ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_TOP_RIGHT), 0, 0);
            }
            return true;
        }
        case USER_STREAM_OPEN: {
            exit_hint_show(controller);
            /* HOLD THE LOADING DIALOG UNTIL A PICTURE EXISTS, in co-op only.
             *
             * USER_STREAM_OPEN means the STREAM opened, not that anything is on screen. The
             * horizontal composite suppresses its own pictures until it can splice ("[COOP-HH]
             * suppressing picture #N before the FIRST composite"), so closing the dialog here
             * hands the user a blank black panel with nothing to say it is still working --
             * user-reported as an awkward pause, then a correction. Vertical hides it by
             * bootstrap-mirroring the primary into both tiles (mirrored=291 live) where
             * horizontal has mirrored=0, which is exactly why the oddity is horizontal-only.
             *
             * Keeping the loader up is better than porting the mirror: the mirror briefly
             * shows one host's desktop in BOTH halves, which is its own kind of wrong, and the
             * dialog is honest about what is happening. The poll is cheap and self-limiting --
             * COOP_FIRST_PICTURE_TIMEOUT_MS is a backstop so a composite that never starts
             * cannot pin the dialog forever; the stream is up either way and the user must be
             * able to reach the overlay to quit. */
            if (app_launch_coop_requested() && controller->progress != NULL &&
                !coop_composite_started() && controller->coop_wait_timer == NULL) {
                controller->coop_wait_ms = 0;
                controller->slow_answered = false;
                controller->coop_wait_timer =
                        lv_timer_create(coop_first_picture_cb, COOP_FIRST_PICTURE_POLL_MS,
                                        controller);
                lv_obj_add_flag(controller->overlay, LV_OBJ_FLAG_HIDDEN);
                /* The hint stays up with the loader (user, 2026-11-11: hints on the co-op loading screen);
                 * coop_first_picture_cb hides it when the dialog goes. */
                apps_controller_hdr_relaunch_succeeded();
                pin_autoentry_on_stream_open(controller->global, &controller->uuid);
                if (app_configuration->show_stats_on_start || controller->network_test) {
                    streaming_set_stats_pinned(controller, true);
                }
                break;
            }
            if (controller->progress) {
                lv_msgbox_close(controller->progress);
                controller->progress = NULL;
            }
            lv_obj_add_flag(controller->overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(controller->hint, LV_OBJ_FLAG_HIDDEN);

            /* A stream opened successfully — clear any pending HDR reconnect/retry. */
            apps_controller_hdr_relaunch_succeeded();

            /* If the user pressed Wake for this host, auto-type the login PIN. */
            pin_autoentry_on_stream_open(controller->global, &controller->uuid);

            if (app_configuration->show_stats_on_start || controller->network_test) {
                streaming_set_stats_pinned(controller, true);
            }

            if (controller->network_test && controller->network_test_timer == NULL) {
                uint32_t period_ms = (uint32_t) controller->network_test_duration * 1000U;
                if (period_ms == 0) {
                    period_ms = 10000U;
                }
                controller->network_test_timer = lv_timer_create(network_test_timer_cb, period_ms, controller);
            }
            break;
        }
        case USER_STREAM_CLOSE: {
            exit_hint_close(controller);
            slow_prompt_close(controller, false);
            /* Stop waiting for a first picture that is no longer coming, and drop the
               loading dialog so "Disconnecting..." replaces it rather than stacking. */
            if (controller->coop_wait_timer) {
                lv_timer_del(controller->coop_wait_timer);
                controller->coop_wait_timer = NULL;
            }
            if (controller->progress) {
                lv_msgbox_close(controller->progress);
                controller->progress = NULL;
            }
            /* Say what is happening: Quit ends the game on the host, Disconnect leaves it to resume (user
             * 2026-11-04: Quit from the taskbar menu said "Disconnecting"; so did the stream menu's Quit). */
            controller->progress = progress_dialog_create(session_is_quitting(controller->global->session)
                                                          ? locstr("Quitting game...")
                                                          : locstr("Disconnecting..."));
            lv_obj_add_flag(controller->overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(controller->stats, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(controller->hint, LV_OBJ_FLAG_HIDDEN);
            break;
        }
        case USER_STREAM_FINISHED: {
            if (controller->progress) {
                lv_msgbox_close(controller->progress);
                controller->progress = NULL;
            }
            if (streaming_errno != 0) {
                lv_timer_create(stream_fragment_del_timer_cb, 50, controller);
            } else {
                lv_async_call((lv_async_cb_t) lv_fragment_del, controller);
            }
            break;
        }
        case USER_OPEN_OVERLAY: {
            show_overlay(controller);
            return true;
        }
        case USER_START_WHEEL: {
            start_wheel_view_update(&controller->wheel, (int) (intptr_t) ((ui_userevent_t *) userdata)->data1);
            local_view_sync(controller);
            return true;
        }
        case USER_CLOSE_SOFT_KEYBOARD: {
            if (streaming_soft_keyboard_shown()) {
                soft_keyboard_close_cb(controller);
            }
            return true;
        }
        case USER_TOGGLE_STATS_PIN: {
            streaming_toggle_stats_pin();
            return true;
        }
        case USER_OPEN_SOFT_KEYBOARD: {
            if (controller->soft_kbd) {
                return true;
            }
            close_start_wheel(controller);
            hide_overlay_impl(controller);
            session_screen_keyboard_opened(controller->global->session);
            controller->soft_kbd = soft_keyboard_create(
                controller->detached_root,
                controller->global->session,
                soft_keyboard_close_cb,
                controller);
            lv_group_t *kbd_group = soft_keyboard_get_group(controller->soft_kbd);
            if (kbd_group) {
                app_input_set_group(&controller->global->ui.input, kbd_group);
            }
            app_set_mouse_grab(&controller->global->input, false);
            local_view_sync(controller);
            return true;
        }
        case USER_SIZE_CHANGED: {
            update_buttons_layout(controller);
            streaming_overlay_resized(controller);
            if (app_launch_local_pane()) {
                local_pane_session_ui_layout(controller->global);
            }
            return false;
        }
        default: {
            break;
        }
    }
    return false;
}

static void on_view_created(lv_fragment_t *self, lv_obj_t *view) {
    streaming_controller_t *controller = (streaming_controller_t *) self;
    app_input_set_group(&controller->global->ui.input, controller->group);
    lv_obj_add_event_cb(controller->quit_btn, exit_streaming, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(controller->suspend_btn, suspend_streaming, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(controller->kbd_btn, open_keyboard, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(controller->vmouse_btn, toggle_vmouse, LV_EVENT_CLICKED, self);
#if TARGET_ANDROID
    lv_obj_add_event_cb(controller->guide_btn, send_guide, LV_EVENT_CLICKED, self);
#endif
    lv_obj_add_event_cb(controller->base.obj, hide_overlay, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(controller->overlay, overlay_key_cb, LV_EVENT_KEY, controller);
    lv_obj_add_event_cb(controller->base.obj, on_cancel_key, LV_EVENT_CANCEL, controller);

    lv_obj_t *notice = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(notice, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(notice, LV_ALIGN_TOP_RIGHT, -LV_DPX(20), LV_DPX(20));
    lv_obj_set_style_radius(notice, LV_DPX(5), 0);
    lv_obj_set_style_pad_hor(notice, LV_DPX(5), 0);
    lv_obj_set_style_pad_ver(notice, LV_DPX(3), 0);
    lv_obj_set_style_border_opa(notice, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(notice, LV_OPA_40, 0);
    lv_obj_set_style_bg_color(notice, lv_color_black(), 0);
    lv_obj_t *notice_label = lv_label_create(notice);
    lv_obj_set_size(notice_label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(notice_label, lv_theme_get_font_small(view), 0);
    lv_obj_add_flag(notice, LV_OBJ_FLAG_HIDDEN);

    controller->notice = notice;
    controller->notice_label = notice_label;

    lv_obj_add_event_cb(controller->stats_pin, pin_toggle, LV_EVENT_VALUE_CHANGED, controller->stats);

#if !defined(TARGET_WEBOS)
    const app_settings_t *settings = &controller->global->settings;
    if (settings->syskey_capture) {
        SDL_SetWindowGrab(controller->global->ui.window, SDL_TRUE);
    }
#endif
}

static void on_delete_obj(lv_fragment_t *self, lv_obj_t *view) {
    LV_UNUSED(view);
    streaming_controller_t *controller = (streaming_controller_t *) self;
    if (controller->notice) {
        lv_obj_del(controller->notice);
    }
    start_wheel_view_destroy(&controller->wheel);
    if (controller->stats->parent != controller->overlay) {
        lv_obj_del(controller->stats);
    }
    slow_prompt_close(controller, false);   /* before its group swap: controller->group goes next */
    app_input_set_group(&controller->global->ui.input, NULL);
    lv_group_del(controller->group);

#if !defined(TARGET_WEBOS)
    SDL_SetWindowGrab(controller->global->ui.window, SDL_FALSE);
#endif
}

static void on_obj_deleted(lv_fragment_t *self, lv_obj_t *view) {
    LV_UNUSED(view);
    streaming_controller_t *controller = (streaming_controller_t *) self;
    exit_hint_close(controller);   /* its timer must not outlive the label deleted with the root */
    lv_obj_del(controller->detached_root);
}

static void exit_streaming(lv_event_t *event) {
    streaming_controller_t *self = lv_event_get_user_data(event);
    session_interrupt(self->global->session, true, STREAMING_INTERRUPT_USER);
}

static void suspend_streaming(lv_event_t *event) {
    streaming_controller_t *self = lv_event_get_user_data(event);
    session_interrupt(self->global->session, false, STREAMING_INTERRUPT_USER);
}

static void soft_keyboard_close_cb(void *userdata) {
    streaming_controller_t *controller = userdata;
    if (!controller || !controller->soft_kbd) {
        return;
    }
    lv_obj_t *kbd_obj = controller->soft_kbd;
    controller->soft_kbd = NULL;
    app_input_set_group(&controller->global->ui.input, controller->group);
    app_set_mouse_grab(&controller->global->input, true);
    if (controller->global->session) {
        session_screen_keyboard_closed(controller->global->session);
    }
    lv_obj_del(kbd_obj);
    local_view_sync(controller);
}

static void open_keyboard(lv_event_t *event) {
    streaming_controller_t *controller = lv_event_get_user_data(event);
    hide_overlay(event);
    if (controller->soft_kbd) {
        return; /* Already showing */
    }
    session_screen_keyboard_opened(controller->global->session);
    controller->soft_kbd = soft_keyboard_create(
        lv_layer_top(),
        controller->global->session,
        soft_keyboard_close_cb,
        controller);
    lv_group_t *kbd_group = soft_keyboard_get_group(controller->soft_kbd);
    if (kbd_group) {
        app_input_set_group(&controller->global->ui.input, kbd_group);
    }
    /* Mostrar cursor e liberar mouse para Magic Remote / ponteiro funcionarem */
    app_set_mouse_grab(&controller->global->input, false);
    local_view_sync(controller);
}

static void toggle_vmouse(lv_event_t *event) {
    streaming_controller_t *controller = lv_event_get_user_data(event);
    hide_overlay(event);
    app_t *app = controller->global;
    session_toggle_vmouse(app->session);
}

#if TARGET_ANDROID
static void send_guide(lv_event_t *event) {
    streaming_controller_t *controller = lv_event_get_user_data(event);
    hide_overlay(event);
    app_t *app = controller->global;
    if (app->session != NULL) {
        session_send_guide(app->session);
    }
}
#endif

/* MOVED OUT OF #if TARGET_WEBOS: this deletes a fragment on a timer and has nothing
 * platform-specific in it, but it was defined inside the guard while its only caller --
 * the streaming_errno path below -- is outside one. Fine on webOS, an undefined reference
 * at link time anywhere else. */
static void stream_fragment_del_timer_cb(lv_timer_t *timer) {
    lv_fragment_t *fragment = timer->user_data;
    lv_timer_del(timer);
    lv_fragment_del(fragment);
}

/* The stream menu or the keyboard has the controllers now: a Start wheel still open goes without acting. */
static void close_start_wheel(streaming_controller_t *controller) {
    start_wheel_view_update(&controller->wheel, 0);
    stream_input_cancel_start_wheel();
    local_view_sync(controller);
}

bool show_overlay(streaming_controller_t *controller) {
    if (overlay_showing) {
        return false;
    }
    close_start_wheel(controller);
    overlay_showing = true;
    local_view_sync(controller);
    lv_obj_clear_flag(controller->base.obj, LV_OBJ_FLAG_HIDDEN);

    lv_area_t coords = controller->video->coords;
    streaming_enter_overlay(controller->global->session, coords.x1, coords.y1, lv_area_get_width(&coords),
                            lv_area_get_height(&coords));
    streaming_refresh_stats();

    app_stop_text_input(&controller->global->ui.input);

    update_buttons_layout(controller);

    return true;
}

/* B/Back: close keyboard if shown, else hide overlay */
static void on_cancel_key(lv_event_t *event) {
    streaming_controller_t *controller = lv_event_get_user_data(event);
    if (streaming_soft_keyboard_shown()) {
        soft_keyboard_close_cb(controller);
    } else {
        hide_overlay(event);
    }
}

static void hide_overlay_impl(streaming_controller_t *controller) {
    app_input_set_button_points(&controller->global->ui.input, NULL);
    lv_obj_add_flag(controller->base.obj, LV_OBJ_FLAG_HIDDEN);
    if (!overlay_showing) {
        return;
    }
    overlay_showing = false;
    local_view_sync(controller);
    app_set_mouse_grab(&controller->global->input, true);
    streaming_enter_fullscreen(controller->global->session);
}

static void hide_overlay(lv_event_t *event) {
    hide_overlay_impl((streaming_controller_t *) lv_event_get_user_data(event));
}

static void overlay_key_cb(lv_event_t *e) {
    streaming_controller_t *controller = lv_event_get_user_data(e);
    lv_group_t *group = controller->group;
    switch (lv_event_get_key(e)) {
        case LV_KEY_LEFT:
            lv_group_focus_prev(group);
            break;
        case LV_KEY_RIGHT:
            lv_group_focus_next(group);
            break;
        default:
            break;
    }
}

static void update_buttons_layout(streaming_controller_t *controller) {
    lv_area_t coords;
    lv_obj_get_coords(controller->quit_btn, &coords);
    lv_area_center(&coords, &controller->button_points[1]);
    lv_obj_get_coords(controller->suspend_btn, &coords);
    lv_area_center(&coords, &controller->button_points[3]);
    lv_obj_get_coords(controller->kbd_btn, &coords);
    lv_area_center(&coords, &controller->button_points[4]);
    app_input_set_button_points(&controller->global->ui.input, controller->button_points);
}

void streaming_toggle_stats_pin(void) {
    if (!current_controller) {
        return;
    }
    streaming_set_stats_pinned(current_controller, !overlay_pinned);
}

static void streaming_set_stats_pinned(streaming_controller_t *controller, bool pinned) {
    if (!controller || !controller->stats) {
        return;
    }
    lv_obj_t *stats = controller->stats;
    bool currently_pinned = stats->parent == lv_layer_top();
    if (pinned == currently_pinned) {
        overlay_pinned = pinned;
        return;
    }
    overlay_pinned = pinned;
    if (pinned) {
        lv_obj_clear_flag(stats, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_parent(stats, lv_layer_top());
        if (app_configuration->show_stats_compact) {
            lv_obj_align(stats, LV_ALIGN_TOP_LEFT, 0, 0);
        } else {
            lv_obj_align(stats, LV_ALIGN_TOP_RIGHT, -LV_DPX(20), LV_DPX(20));
        }
        lv_obj_add_state(stats, LV_STATE_USER_1);
        lv_obj_add_state(controller->stats_pin, LV_STATE_CHECKED);
        streaming_refresh_stats();
    } else {
        lv_obj_set_parent(stats, controller->base.obj);
        lv_obj_clear_state(stats, LV_STATE_USER_1);
        lv_obj_clear_state(controller->stats_pin, LV_STATE_CHECKED);
    }
}

static void pin_toggle(lv_event_t *e) {
    lv_obj_t *toggle_view = lv_event_get_user_data(e);
    lv_fragment_t *fragment = lv_obj_get_user_data(toggle_view);
    bool checked = lv_obj_has_state(lv_event_get_current_target(e), LV_STATE_CHECKED);
    streaming_set_stats_pinned((streaming_controller_t *) fragment, checked);
}

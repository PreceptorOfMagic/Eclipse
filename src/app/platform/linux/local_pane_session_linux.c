/* HOST-AND-CLIENT CO-OP ON LINUX: the session's side (who calls what, and when, is in platform/local_pane_session.h).
 * This PC's half is a virtual output (local_pane_linux.c) whose picture the client draws itself, between the video and
 * the UI (local_pane_session_draw, called from lv_app_redraw_now), so menus and dialogs cover it like anything else. */
#include "platform/local_pane_session.h"

#include <stdarg.h>
#include <stdio.h>

#include <SDL.h>

#include "app.h"
#include "app_launch.h"
#include "local_pane_linux.h"
#include "logging.h"
#include "platform/local_input.h"
#include "stream/input/coop_pointer.h"
#include "stream/session.h"

static volatile bool display_added = false;
static volatile bool ui_active = false;
static bool view_covered = false;
static SDL_Rect local_px;          /* this PC's half, in renderer output pixels */
static bool area_ok;
static int area_retries;
static bool lost_reported;

static void pane_log(const char *line) {
    commons_log_info("COOP-LP", "%s", line);
}

static void pane_logf(const char *fmt, ...) {
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    pane_log(line);
}

static void halves(int w, int h, SDL_Rect *local, SDL_Rect *stream) {
    SDL_Rect first = {0, 0, w, h}, second = {0, 0, w, h};
    if (app_launch_local_pane_vertical()) {
        first.w = w / 2;
        second.x = first.w;
        second.w = w - first.w;
    } else {
        first.h = h / 2;
        second.y = first.h;
        second.h = h - first.h;
    }
    const bool local_first = app_launch_local_pane_first();
    *local = local_first ? first : second;
    *stream = local_first ? second : first;
}

static bool place(app_t *app) {
    SDL_Renderer *renderer = app != NULL && app->ui.window != NULL ? SDL_GetRenderer(app->ui.window) : NULL;
    int out_w = 0, out_h = 0;
    if (renderer == NULL || SDL_GetRendererOutputSize(renderer, &out_w, &out_h) != 0 || out_w < 2 || out_h < 2) {
        return false;
    }
    SDL_Rect stream;
    halves(out_w, out_h, &local_px, &stream);
    area_ok = streaming_video_area(app->session, stream.x, stream.y, stream.w, stream.h);
    return true;
}

bool local_pane_session_supported(void) {
    return local_pane_linux_available();
}

bool local_pane_session_prepare(int width, int height, int hz) {
    /* The virtual output goes to the side of the layout the STREAM's half is on: the pointer leaves it toward the
     * stream by an outer edge, where the input capture's barrier can sit (docs/design-host-and-client-linux.md). */
    const bool vertical = app_launch_local_pane_vertical(), local_first = app_launch_local_pane_first();
    const local_pane_side_t side = vertical ? (local_first ? LOCAL_PANE_SIDE_RIGHT : LOCAL_PANE_SIDE_LEFT)
                                            : (local_first ? LOCAL_PANE_SIDE_BOTTOM : LOCAL_PANE_SIDE_TOP);
    if (!local_pane_linux_start(width, height, hz, side, pane_log)) {
        pane_logf("[pane] session: this PC's display (%dx%d at %d Hz) could not be made; the session does not start",
                  width, height, hz);
        return false;
    }
    display_added = true;
    return true;
}

void local_pane_session_ui_open(app_t *app) {
    if (!display_added || ui_active) {
        return;
    }
    const bool shown = place(app);
    pane_logf("[pane] session: %s half is this PC, the stream has the other; view %s",
              app_launch_local_pane_vertical() ? (app_launch_local_pane_first() ? "LEFT" : "RIGHT")
                                               : (app_launch_local_pane_first() ? "TOP" : "BOTTOM"),
              shown ? "started" : "DID NOT START (that half stays empty)");
    ui_active = true;
    lost_reported = false;
    view_covered = false;
    area_retries = 0;
    coop_pointer_start_local(app_launch_local_pane_vertical(), app_launch_local_pane_first());
}

void local_pane_session_ui_layout(app_t *app) {
    if (ui_active) {
        place(app);
    }
}

void local_pane_session_ui_tick(app_t *app) {
    if (ui_active && !lost_reported && app != NULL && app->session != NULL && local_pane_linux_lost()) {
        lost_reported = true;
        pane_log("[pane] session: this PC's display was stopped from outside Eclipse (GNOME's sharing indicator); "
                 "ending the session");
        session_interrupt(app->session, false, STREAMING_INTERRUPT_ERROR);
        streaming_error(app->session, STREAMING_INTERRUPT_ERROR,
                        "This PC's half was stopped from the system's screen-sharing indicator, so the co-op session "
                        "ended. Start it again to carry on.");
        return;
    }
    if (!ui_active || area_ok) {
        return;
    }
    place(app);
    area_retries++;
    if (area_ok || area_retries == 1 || area_retries % 20 == 0) {
        pane_logf("[pane] stream's half %s (retry %d)", area_ok ? "taken by the video module" : "NOT taken yet",
                  area_retries);
    }
}

void local_pane_session_ui_overlay(app_t *app, bool covered) {
    (void) app;
    if (!ui_active || covered == view_covered) {
        return;
    }
    view_covered = covered;
    local_input_set_ui_overlay(covered);
}

void local_pane_session_draw(void *renderer) {
    if (!ui_active || view_covered || renderer == NULL) {
        return;
    }
    local_pane_linux_draw((SDL_Renderer *) renderer, &local_px);
    /* every 10 s: how many pictures this PC's display delivered, and how often the client drew -- the view's real rate */
    static Uint32 since;
    static uint64_t frames_at, draws;
    draws++;
    const Uint32 now = SDL_GetTicks();
    if (since == 0) {
        since = now;
        frames_at = local_pane_linux_frames();
        draws = 0;
    } else if (now - since >= 10000) {
        const uint64_t f = local_pane_linux_frames();
        pane_logf("[pane] view: %.1f pictures/s from this PC's display, %.1f draws/s",
                  (double) (f - frames_at) * 1000.0 / (now - since), (double) draws * 1000.0 / (now - since));
        since = now;
        frames_at = f;
        draws = 0;
    }
}

void local_pane_session_ui_close(void) {
    if (!ui_active) {
        return;
    }
    coop_pointer_stop();
    local_pane_linux_drop_texture();
    view_covered = false;
    ui_active = false;
    pane_logf("[pane] session: view closed (%llu pictures received)", (unsigned long long) local_pane_linux_frames());
}

void local_pane_session_worker_end(void) {
    if (!display_added) {
        return;
    }
    for (int waited = 0; waited < 150 && ui_active; waited++) {
        SDL_Delay(20);
    }
    local_pane_linux_stop();
    display_added = false;
}

bool local_pane_session_frame_pending(void) {
    return ui_active && !view_covered && local_pane_linux_frame_pending();
}

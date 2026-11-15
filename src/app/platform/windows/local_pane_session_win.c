/*
 * CO-OP WITH THIS PC'S SIDE RUN LOCALLY -- the session's side of it. What it is for and who calls what, when:
 * platform/local_pane_session.h. The display and the view themselves: local_pane_win.h.
 *
 * TWO THREADS. The display is added and removed on the stream thread (adding blocks for seconds, and the display
 * must exist before the other PC is asked to start so a failure can still stop the launch). The view is a child
 * of the client window, so it is started and stopped on the UI thread, which owns that window. The stream thread
 * therefore cannot stop the view itself, and it must not wait on the UI thread without a limit either: the UI
 * thread joins the stream thread when a session is destroyed (session_destroy), and two threads waiting on each
 * other is a hang. So the UI thread stops the view when the stream closes (USER_STREAM_CLOSE), and the stream
 * thread, on its way out, waits a BOUNDED time for that before it removes the display regardless.
 */
#include "platform/local_pane_session.h"

#include <winsock2.h>
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>

#include <SDL.h>
#include <SDL_syswm.h>

#include "app.h"
#include "app_launch.h"
#include "local_host_win.h"
#include "local_pane_win.h"
#include "logging.h"
#include "platform/local_input.h"
#include "stream/input/coop_pointer.h"
#include "stream/session.h"

/* Written by the stream thread, read by both. */
static volatile bool display_added = false;
/* Written by the UI thread, read by both. */
static volatile bool ui_active = false;
/* UI thread only: the view is hidden because the client is drawing over the picture. */
static bool view_covered = false;

/* Every line of local_pane_win.c (they start "[pane] ") and of this file, into both logs: the client's own, and
 * the one beside the other "this PC is a host" lines. Called from the display's and the view's threads too. */
static void pane_log(const char *line) {
    local_host_log("%s", line);
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

static HWND client_window(app_t *app) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (app == NULL || app->ui.window == NULL || !SDL_GetWindowWMInfo(app->ui.window, &info) ||
        info.subsystem != SDL_SYSWM_WINDOWS) {
        return NULL;
    }
    return info.info.win.window;
}

/* A w x h surface cut in two along the session's divider: this PC's half and the stream's. */
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

/* Both halves where the window now is. The view is placed in the window's client pixels; the stream's picture in
 * the renderer's output pixels, which is what the video module draws in. They are the same numbers unless Windows
 * is scaling the window, and each is asked for in its own units so that case stays right too. */
/* THE STREAM'S HALF MUST BE TAKEN, NOT JUST ASKED FOR. ss4s ignores a display area set before its video instance
 * exists, and its ffmpeg module one set before that instance is the renderer's current one (ss4s src/video.c,
 * modules/ffmpeg/ffmpegvid.c SetDisplayArea). User 2026-11-12/13: at every stream start a line showed in the middle
 * of the stream's half for a while -- x = 3/4 of the window, the right edge of a 1280-wide picture centred in the
 * WHOLE 2560-wide window, i.e. the area had not been taken. So the result is checked and the tick retries it. */
static bool area_ok;
static int area_retries;

static bool place(app_t *app, bool start_view) {
    HWND hwnd = client_window(app);
    RECT rc;
    if (hwnd == NULL || !GetClientRect(hwnd, &rc) || rc.right < 2 || rc.bottom < 2) {
        return false;
    }
    SDL_Rect local, stream;
    halves((int) rc.right, (int) rc.bottom, &local, &stream);
    bool ok = true;
    if (start_view) {
        ok = local_pane_view_start(hwnd, NULL, local.x, local.y, local.w, local.h);
    } else {
        local_pane_view_set_area(local.x, local.y, local.w, local.h);
    }
    int out_w = 0, out_h = 0;
    SDL_Renderer *renderer = SDL_GetRenderer(app->ui.window);
    if (renderer == NULL || SDL_GetRendererOutputSize(renderer, &out_w, &out_h) != 0 || out_w < 2 || out_h < 2) {
        out_w = (int) rc.right;
        out_h = (int) rc.bottom;
    }
    halves(out_w, out_h, &local, &stream);
    area_ok = streaming_video_area(app->session, stream.x, stream.y, stream.w, stream.h);
    return ok;
}

bool local_pane_session_supported(void) {
    return local_pane_available();
}

bool local_pane_session_prepare(int width, int height, int hz) {
    local_pane_set_log(pane_log);
    /* FIRST the session file: adding a display changes the layout, and the guard that keeps the client window on
     * its monitor through that (local_host_window_tick) only acts while the file names this process. */
    local_host_pane_session_begin();
    if (!local_pane_display_add(width, height, hz)) {
        pane_logf("[pane] session: this PC's display (%dx%d at %d Hz) could not be made; the session does not start",
                  width, height, hz);
        local_pane_display_remove();
        return false;
    }
    display_added = true;
    return true;
}

void local_pane_session_ui_open(app_t *app) {
    if (!display_added || ui_active) {
        return;
    }
    char name[64] = "";
    const bool named = local_pane_display_name(name, sizeof(name));
    const bool shown = place(app, true);
    pane_logf("[pane] session: %s half is this PC (%s), the stream has the other; view %s",
              app_launch_local_pane_vertical() ? (app_launch_local_pane_first() ? "LEFT" : "RIGHT")
                                               : (app_launch_local_pane_first() ? "TOP" : "BOTTOM"),
              named ? name : "display not named", shown ? "started" : "DID NOT START (that half stays empty)");
    ui_active = true;
    view_covered = false;
    area_retries = 0;
    if (!area_ok) {
        pane_logf("[pane] stream's half not taken at open: retried on the window timer");
    }
    /* The pointer and the keyboard go to this PC's display by name: the client made it, so it is not searched
     * for, and another virtual display on this PC cannot be taken for it. */
    local_input_pin_display(named ? name : NULL);
    coop_pointer_start_local(app_launch_local_pane_vertical(), app_launch_local_pane_first());
}

void local_pane_session_ui_layout(app_t *app) {
    if (!ui_active) {
        return;
    }
    place(app, false);
}

void local_pane_session_ui_tick(app_t *app) {
    if (!ui_active || area_ok) {
        return;
    }
    place(app, false);
    area_retries++;
    if (area_ok || area_retries == 1 || area_retries % 20 == 0) {
        pane_logf("[pane] stream's half %s (retry %d)", area_ok ? "taken by the video module" : "NOT taken yet",
                  area_retries);
    }
}

void local_pane_session_ui_overlay(app_t *app, bool covered) {
    if (!ui_active || covered == view_covered) {
        return;
    }
    view_covered = covered;
    local_input_set_ui_overlay(covered);
    local_pane_view_show(!covered);
    if (!covered) {
        place(app, false);
    }
}

void local_pane_session_ui_close(void) {
    if (!ui_active) {
        return;
    }
    local_pane_stats_t st;
    local_pane_view_stats(&st);
    coop_pointer_stop();
    local_input_pin_display(NULL);
    local_pane_view_stop();
    view_covered = false;
    ui_active = false;
    pane_logf("[pane] session: view closed (%dx%d, %lu frames taken, %lu shown, %lu superseded)", st.cap_w, st.cap_h,
              st.frames, st.presents, st.dropped);
}

void local_pane_session_worker_end(void) {
    if (!display_added) {
        return;
    }
    /* The UI thread stops the view when it sees the stream close. Give it a moment -- a display removed under a
     * running capture ends the capture with an error instead of a stop -- but never wait without a limit: see the
     * top of this file. */
    int waited = 0;
    for (; waited < 150; waited++) {
        local_pane_stats_t st;
        local_pane_view_stats(&st);
        if (!st.running && !ui_active) {
            break;
        }
        SDL_Delay(20);
    }
    if (waited >= 150) {
        pane_log("[pane] session: the view was still up after 3 s; removing this PC's display anyway");
    }
    local_pane_display_remove();
    display_added = false;
}

void local_pane_session_draw(void *renderer) {
    (void) renderer;   /* the view is a window of its own here */
}

bool local_pane_session_frame_pending(void) {
    return false;
}

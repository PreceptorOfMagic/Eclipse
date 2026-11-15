#pragma once
/*
 * CO-OP WITH THIS PC'S SIDE RUN LOCALLY -- the session's side of it (user 2026-11-10: "a pc acting as client and
 * host isn't streaming its own plane to itself instead we are running the pc acting as client and host fully
 * locally and composing in the other pc's stream").
 *
 * Such a session (app_launch_local_pane) is ONE ordinary stream, to the other PC, drawn in one half of the client
 * window. The other half is this PC's own virtual display, shown directly: nothing of this PC is encoded, sent or
 * decoded, and no host program on this PC is asked for anything. The display and its view are
 * platform/windows/local_pane_win.h; this is who calls them and when:
 *
 *   stream thread, before the other PC is asked to start   local_pane_session_prepare      (adds the display)
 *   UI thread, the stream is up                             local_pane_session_ui_open      (view, pointer split)
 *   UI thread, window resized / a menu over the picture     local_pane_session_ui_layout / _ui_overlay
 *   UI thread, the stream is going down                     local_pane_session_ui_close     (view, pointer split)
 *   stream thread, on its way out                           local_pane_session_worker_end   (removes the display)
 *
 * The view is a window of its own ABOVE everything the client draws: whatever the client shows in this PC's half
 * is covered by it. That is why _ui_overlay exists, and why notes for such a session go in the stream's half.
 *
 * Windows and desktop Linux (GNOME, platform/linux/local_pane_session_linux.c: the view is drawn by the client
 * itself, local_pane_session_draw). Elsewhere local_pane_session_supported() is false and the rest does nothing.
 */

#include <stdbool.h>

typedef struct app_t app_t;

#if defined(_WIN32) || (defined(OS_LINUX) && !defined(TARGET_WEBOS) && !defined(TARGET_ANDROID) && !defined(ECLIPSE_NO_LOCAL_PANE))
#define LOCAL_PANE_SESSION_BUILT 1

/** This PC can run its own side: the virtual display driver is installed and opens for this user. Cheap. */
bool local_pane_session_supported(void);

/**
 * Add this PC's display (@p width x @p height at @p hz, the size of its half) and make it the primary one, so
 * this PC's games open on it. Blocks for up to a few seconds: call on the stream thread, never the UI thread.
 * @return false if the display could not be made; nothing is left behind then.
 */
bool local_pane_session_prepare(int width, int height, int hz);

/** The stream is up: show the display in this PC's half, the stream in the other, and split the pointer. */
void local_pane_session_ui_open(app_t *app);

/** The window changed size: both halves follow. */
void local_pane_session_ui_layout(app_t *app);

/** Called on the UI's window timer: re-applies the stream's half until the video module has taken it. */
void local_pane_session_ui_tick(app_t *app);

/** The client is drawing something of its own over the picture (@p covered) or has stopped: hide / show the view. */
void local_pane_session_ui_overlay(app_t *app, bool covered);

/** The stream is going down: undo local_pane_session_ui_open. Safe to call again, and when it never opened. */
void local_pane_session_ui_close(void);

/** The stream thread is leaving: remove the display, after giving the UI thread a moment to stop the view. */
void local_pane_session_worker_end(void);

/** UI thread, every present, between the video and the UI: draw this PC's half where the client draws it itself
 *  (Linux). @p renderer is the window's SDL_Renderer. No-op on Windows (its view is a window of its own). */
void local_pane_session_draw(void *renderer);

/** A picture of this PC's display newer than the one drawn is waiting (Linux): the main loop presents for it as it
 *  does for a stream frame. Always false on Windows. */
bool local_pane_session_frame_pending(void);

#else

static inline bool local_pane_session_supported(void) {
    return false;
}

static inline bool local_pane_session_prepare(int width, int height, int hz) {
    (void) width;
    (void) height;
    (void) hz;
    return false;
}

static inline void local_pane_session_ui_open(app_t *app) {
    (void) app;
}

static inline void local_pane_session_ui_tick(app_t *app) {
    (void) app;
}

static inline void local_pane_session_ui_layout(app_t *app) {
    (void) app;
}

static inline void local_pane_session_ui_overlay(app_t *app, bool covered) {
    (void) app;
    (void) covered;
}

static inline void local_pane_session_ui_close(void) {}

static inline void local_pane_session_worker_end(void) {}

static inline void local_pane_session_draw(void *renderer) {
    (void) renderer;
}

static inline bool local_pane_session_frame_pending(void) {
    return false;
}

#endif

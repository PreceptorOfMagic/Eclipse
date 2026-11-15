#pragma once
/*
 * HOST-AND-CLIENT CO-OP ON LINUX: this PC's own display (GNOME/Mutter). The Linux side of what
 * platform/windows/local_pane_win.h does on Windows, built from what was measured on VEGA on 2026-11-14
 * (docs/design-host-and-client-linux.md, sections D and E):
 *
 *   - a virtual output the size of this PC's half, made through Mutter's ScreenCast RecordVirtual inside a
 *     RemoteDesktop session (no root, no driver; Mutter removes it when the session ends or Eclipse dies);
 *   - placed beyond the layout on the side that faces the stream's half, at scale 1, as the primary output (so
 *     this PC's games and GNOME's own UI open there), the previous layout put back at the end;
 *   - its picture read from PipeWire (shared memory) and handed to the UI thread to draw in this PC's half;
 *   - the same RemoteDesktop session places the real pointer on it (absolute motion) for the hand-over.
 */
#include <stdbool.h>
#include <stdint.h>

#include <SDL.h>

/** Mutter's ScreenCast + RemoteDesktop + DisplayConfig are on the session bus. Cheap enough to call at Start. */
bool local_pane_linux_available(void);

/** Which edge of the layout the virtual output goes to: the one facing the stream's half. */
typedef enum {
    LOCAL_PANE_SIDE_LEFT,
    LOCAL_PANE_SIDE_RIGHT,
    LOCAL_PANE_SIDE_TOP,
    LOCAL_PANE_SIDE_BOTTOM,
} local_pane_side_t;

/**
 * Make the virtual output (@p width x @p height, @p hz) and start reading it. Blocks up to ~5 s (stream thread).
 * @return false when any step failed; nothing is left behind then.
 */
bool local_pane_linux_start(int width, int height, int hz, local_pane_side_t side, void (*log)(const char *line));

/** Undo local_pane_linux_start (session stopped, layout put back). Safe to call again. */
void local_pane_linux_stop(void);

bool local_pane_linux_running(void);

/** The virtual output's picture stream stopped for good after it had run (GNOME's sharing indicator was used to stop
 *  it): the session cannot go on with this PC's half. GNOME only; false on KDE. */
bool local_pane_linux_lost(void);

/** UI thread: draw the newest picture into @p dst of @p renderer (keeps its own texture). @return true if drawn. */
bool local_pane_linux_draw(SDL_Renderer *renderer, const SDL_Rect *dst);

/** UI thread: drop the texture (renderer going away or reset). */
void local_pane_linux_drop_texture(void);

/** The virtual output and the output Eclipse's window is on, in the compositor's logical coordinates. */
bool local_pane_linux_geometry(SDL_Rect *virtual_output, SDL_Rect *client_output);

/** Put the real pointer at (@p x, @p y) of the virtual output (its own logical coordinates). */
bool local_pane_linux_pointer_to_pane(double x, double y);

/** Put the real pointer at (@p x, @p y) of the output Eclipse's window is on (that output's pixels). */
bool local_pane_linux_pointer_to_client_output(double x, double y);

/** A picture newer than the one last drawn has arrived. */
bool local_pane_linux_frame_pending(void);

/** Frames received so far (diagnostics). */
uint64_t local_pane_linux_frames(void);

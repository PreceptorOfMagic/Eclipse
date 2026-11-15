#pragma once
/*
 * HOST-AND-CLIENT CO-OP ON LINUX, KDE PLASMA: this PC's half as a KWin virtual output (local_pane_linux.c draws its
 * picture; this file makes, places and removes the output and moves the real pointer). Measured on VEGA 2026-11-15,
 * Ubuntu 26.04, KDE Plasma 6.6.6 (docs/design-host-and-client-linux.md, section F):
 *
 *   - zkde_screencast_unstable_v1.stream_virtual_output makes the output and gives its PipeWire node (as Umbra for
 *     Linux does); KWin removes it when the stream is closed or this connection ends;
 *   - kde_output_management_v2 places it beyond the layout on the stream's side, at scale 1, priority 1;
 *   - org_kde_kwin_fake_input.pointer_motion_absolute puts the real pointer on it (KWin then reports it as the
 *     active output, so windows opened from there open on it);
 *   - all three are KWin "restricted" protocols: KWin offers them only to a client whose desktop file lists them in
 *     X-KDE-Wayland-Interfaces (io.github.PreceptorOfMagic.Eclipse.desktop; eclipse.sh keeps a copy for Flatpak
 *     1.18.4+, which stops exporting that key).
 */
#include <stdbool.h>
#include <stdint.h>

#include <SDL.h>

#include "local_pane_linux.h"

/** KWin offers this client its virtual outputs, output management and fake input. */
bool local_pane_kde_available(void);

/** Make the output (@p width x @p height) and place it on @p side. On success @p node is its PipeWire node and the
 *  rectangles are the compositor's logical coordinates of the virtual output and of this window's output. */
bool local_pane_kde_start(int width, int height, local_pane_side_t side, uint32_t *node, SDL_Rect *virtual_output,
                          SDL_Rect *client_output, double *client_scale, void (*log)(const char *fmt, ...));

/** Put the real pointer at (@p x, @p y) in the compositor's logical coordinates. */
bool local_pane_kde_pointer(double x, double y);

/** Pointer back on this window's output, then the output removed and the connection closed. Safe to call again. */
void local_pane_kde_stop(void);

#pragma once

#include <stdbool.h>

#include <SDL.h>

/** How often local_host_window_tick() runs, from a UI-thread timer. */
#define LOCAL_HOST_WINDOW_POLL_MS 500

/**
 * A stream is about to start from @p address. If that host is THIS machine: block all input to
 * it (LiSetLocalInputBlocked) and announce it in %ProgramData%\Eclipse\local-host-session (this
 * process's id), which the host's display hook reads when the client connects. Call it before
 * the launch request, and local_host_session_end() when the stream is over.
 * @return true if the host is this machine (even if the file could not be written).
 */
bool local_host_session_begin(const char *address);

/** The stream ended: unblock input and remove the file, if this process wrote it. Always safe. */
void local_host_session_end(void);

/** Append one line to %ProgramData%\Eclipse\local-host.log (timestamp and pid added). */
void local_host_log(const char *fmt, ...);

/**
 * KEEP THE CLIENT WINDOW ON ITS OWN MONITOR through a local-host session. When this PC is one
 * of the hosts, Apollo's virtual display appears and the display layout changes under the
 * window; Windows can move the window onto the virtual display, where the user cannot see it
 * and the host captures it. Call every LOCAL_HOST_WINDOW_POLL_MS on the UI thread, always: the
 * window's home monitor is learned from calm periods, and it is only moved back while a
 * local-host session file exists.
 */
void local_host_window_tick(SDL_Window *window);

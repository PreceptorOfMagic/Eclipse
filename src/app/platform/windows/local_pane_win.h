#pragma once
/*
 * THIS PC'S OWN CO-OP PANE, WITHOUT STREAMING IT (user 2026-11-10: "a pc acting as client and host isn't
 * streaming its own pane to itself instead we are running the pc acting as client and host fully locally and
 * composing in the other pc's stream").
 *
 * Two independent halves, both Windows-only:
 *
 *   DISPLAY  a virtual display of the pane's size, made through the SudoVDA driver's control device, kept alive
 *            by its watchdog ping, made the primary display so this PC's games open on it, and removed when the
 *            session ends (Windows then returns to the layout it remembers for the real monitors).
 *   VIEW     that display shown inside the client window: Windows Graphics Capture of the monitor, presented by
 *            its own swap chain in a child window over this PC's half. It runs on its own thread at its own
 *            rate, independent of the client's UI loop and of the other PC's stream.
 *
 * Nothing here needs a host program (Umbra) on this PC, nor administrator rights: the driver's control device
 * is open to ordinary users (measured on SKYNET 2026-11-10). It does need the driver installed.
 *
 * No windows.h in this header on purpose: it is included from portable code. HWNDs travel as void *.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Every line this module logs goes here (one line, no newline). NULL = OutputDebugString only. */
typedef void (*local_pane_log_fn)(const char *line);
void local_pane_set_log(local_pane_log_fn fn);

/** The virtual display driver is installed and its control device opens for this user. Cheap; no side effects. */
bool local_pane_available(void);

/**
 * Create the pane's display (@p width x @p height at @p hz), wait for Windows to activate it, give it that mode
 * and make it the primary display with the real monitors beside it. A display left behind by an earlier run of
 * this program is removed first. Blocks for up to a few seconds.
 * @return true once the display is active (even if the layout could only partly be applied; that is logged).
 */
bool local_pane_display_add(int width, int height, int hz);

/** Remove the display made by local_pane_display_add(). Safe to call when there is none. */
void local_pane_display_remove(void);

/** The GDI name of the pane's display ("\\\\.\\DISPLAY7"), or false when there is none. */
bool local_pane_display_name(char *out, size_t len);

/**
 * Show a display inside @p parent_hwnd (the client window), aspect-fitted into the rectangle given in the
 * parent's client pixels. @p gdi_name NULL = the display made by local_pane_display_add().
 * Call on the thread that owns @p parent_hwnd (the child window is created there and needs its message loop).
 */
bool local_pane_view_start(void *parent_hwnd, const char *gdi_name, int x, int y, int w, int h);

/** Move/resize the view (same thread as start). */
void local_pane_view_set_area(int x, int y, int w, int h);

/** Hide or show the view without stopping the capture, e.g. while the client draws a menu over it. */
void local_pane_view_show(bool show);

/** Stop the capture and destroy the child window (same thread as start). Safe when not started. */
void local_pane_view_stop(void);

typedef struct local_pane_stats_t {
    bool running;            /* the capture thread is alive and has not failed */
    int cap_w, cap_h;        /* size of the captured display, 0 before the first frame */
    unsigned long frames;    /* frames taken from Windows */
    unsigned long presents;  /* frames shown */
    unsigned long dropped;   /* frames superseded by a newer one before they were shown */
} local_pane_stats_t;

void local_pane_view_stats(local_pane_stats_t *out);

#ifdef __cplusplus
}
#endif

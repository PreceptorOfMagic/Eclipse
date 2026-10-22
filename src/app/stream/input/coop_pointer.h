/*
 * Co-op split-screen: POINTER ROUTING (the LG magic remote's cursor).
 *
 * THE PROBLEM. The magic remote is an ABSOLUTE pointer -- it reports where on the
 * panel you are aiming, and Eclipse forwards that straight to the host with
 * LiSendMousePositionEvent(x, y, window_w, window_h). The host divides by the
 * reference size, so the whole TV screen maps onto the whole of that host's desktop.
 *
 * In co-op the TV screen is not one desktop, it is TWO PANES, and each host is
 * streaming HALF a picture (session.c halves the width for vertical, the height for
 * horizontal). Sending the unmapped position therefore did two wrong things at once:
 * every event went to the primary no matter which pane you were aiming at, and the
 * full TV height (or width) was squeezed into a half-sized desktop, so the cursor sat
 * nowhere near the thing you were pointing at.
 *
 * WHAT THIS DOES. One choke point for the absolute pointer:
 *   1. decide which PANE the position is in, from the divider's real position;
 *   2. re-express the position as PANE-LOCAL, against the pane's own reference size;
 *   3. send it on that pane's connection -- ours for the primary, over the co-op
 *      input socket for the peer.
 *
 * CROSSING THE DIVIDER NEEDS NO HANDSHAKE, and that is the whole reason absolute
 * positioning is the right primitive here. The first event on the far side already
 * carries where it is, so the newly-entered PC's cursor appears at the matching edge
 * of its own screen -- come in from above and you arrive at its top, come in from the
 * left and you arrive at its left. There is no warp, no re-centre, no settling frame:
 * the switch costs exactly one packet, the same one packet an ordinary move costs.
 *
 * THE DIVIDER'S AXIS IS READ FROM THE MODE SENTINEL, not from the settings. The
 * composition path latches its geometry off `/tmp/coop-vmirror` (session_video.c:2516),
 * and a pointer mapped against a different orientation than the pixels are composed in
 * is worse than no mapping at all. One source of truth, read at the same moment.
 *
 * A HELD BUTTON PINS THE PANE. While anything is pressed the pointer stays on the PC
 * the press landed on and is clamped to that pane, so a drag near the divider cannot
 * hand a half-finished drag to the other machine or leave a button stuck down on a
 * host we have stopped talking to. It is released the moment the button is.
 *
 * WHAT IS DELIBERATELY NOT ROUTED: the RELATIVE paths (a USB mouse through evmouse,
 * the analog-stick virtual mouse, SDL relative mode). A delta carries no position, so
 * there is nothing to decide a pane from; they keep going to the primary exactly as
 * before. The magic remote is absolute, which is what this is for. One exception: the
 * virtual mouse keeps a position of its own in co-op and comes through here like the
 * magic remote (session_virt_mouse.c).
 */
#pragma once

#include <stdbool.h>

/**
 * Latch the split geometry and clear the routing state. Called when the co-op input
 * coordinator comes up, so the pointer and the composite agree about the mode.
 */
void coop_pointer_start(void);

/** Forget the split; the pointer goes back to the plain single-host path. */
void coop_pointer_stop(void);

/** True when positions are being split between two panes. */
bool coop_pointer_active(void);

/**
 * Absolute pointer position in TV window coordinates, @p ref_w x @p ref_h being the
 * coordinate space it was reported in (the session's display size).
 *
 * Outside co-op this is exactly LiSendMousePositionEvent(x, y, ref_w, ref_h) and
 * nothing about the single-host path changes.
 */
void coop_pointer_position(int x, int y, int ref_w, int ref_h);

/** Mouse button, routed to the pane the pointer is currently in. */
void coop_pointer_button(int action, int button);

/** Wheel, routed to the pane the pointer is currently in. Zero axes are not sent,
 *  which is the guard the single-host path already applied. */
void coop_pointer_scroll(int vertical, int horizontal);

/** Diagnostics: how many times the pointer has crossed the divider this session. */
unsigned coop_pointer_crossings(void);

/*
 * A POINTER WITH A POSITION OF ITS OWN. The virtual mouse (session_virt_mouse.c) is a stick,
 * so it has only deltas; in co-op it keeps its own position across the whole split and comes
 * through coop_pointer_position like the magic remote, which is what lets it cross the divider.
 * These are for that.
 */

/** True when the divider is a vertical line (panes left | right); valid while the split is armed. */
bool coop_pointer_split_vertical(void);

/** Where such a pointer starts: the centre of the pane the pointer is routed to now, in a
 *  @p ref_w x @p ref_h window. */
void coop_pointer_pane_centre(int ref_w, int ref_h, int *x, int *y);

/** A high-resolution wheel step (120 = one notch), routed like coop_pointer_scroll. The peer's
 *  link carries whole notches, so the peer is sent one each time 120 has built up. */
void coop_pointer_scroll_high_res(int amount);

/** True while the split is armed with THIS machine as one of its hosts (coop_pointer_set_local_pane). */
bool coop_pointer_has_local_pane(void);

/**
 * Which pane's host is THIS machine -- the Windows client can be one of its own co-op hosts:
 * 0 = the primary's pane, 1 = the peer's, anything else = neither. Nothing is ever SENT to that
 * pane (it would loop back through this desktop); the pointer entering it hands the real
 * cursor over to the host's virtual display instead (platform/local_input.h). Set before the
 * session starts; coop_pointer_start latches it.
 */
void coop_pointer_set_local_pane(int pane);

/**
 * THE MOUSE AND KEYBOARD ASSIGNED LIKE CONTROLLERS (issue #37, co-op setting "assign mouse and
 * keyboard like controllers"): @p mouse_pane / @p keys_pane = 0 primary, 1 peer, anything else =
 * follows the pointer (the default). An assigned mouse never crosses the divider -- its position
 * is clamped into its pane; assigned keys go to their pane wherever the pointer is. Set before
 * the session starts; coop_pointer_start latches it.
 */
void coop_pointer_set_pins(int mouse_pane, int keys_pane);

/** Keys go where the pointer is (user requirement 2026-09-26: "active on whichever pc the mouse is
 *  hovered over"): true while the split is armed, the peer is attached and the pointer's pane is
 *  the PEER's. The pane only changes on pointer motion, so typing never moves it. */
bool coop_pointer_keys_to_peer(void);

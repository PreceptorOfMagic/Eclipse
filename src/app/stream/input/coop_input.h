/*
 * Co-op split-screen: CONTROLLER ROUTING.
 *
 * The two hosts are two moonlight sessions in two processes (moonlight-common-c
 * keeps connection state in file-scope globals, so they cannot share one). Only
 * the PRIMARY process owns SDL and therefore sees the controllers. Routing a pad
 * to the peer means shipping its events to the secondary process, which calls
 * LiSend*() on the connection it holds.
 *
 * A SEPARATE SOCKET FROM VIDEO AND AUDIO, and that is not tidiness. The video
 * link carries megabyte access units; a button press queued behind one would
 * arrive a frame late, which is the one thing this project refuses to trade
 * ([[latency-minimization]]). Audio got its own socket for the same reason.
 *
 * RULES THIS PATH OBEYS
 * - Input NEVER blocks. Every send is non-blocking. Backpressure is absorbed by
 *   a bounded queue: pointer/sensor and controller axes/trigger samples may
 *   coalesce, while controller-button, lifecycle, touch, mouse-button, and
 *   scroll edges retain FIFO order. A dead peer cannot stall the SDL thread.
 * - The PRIMARY decides routing, not the secondary. One place reads
 *   coop_pad_pane[] so the two processes cannot disagree about who owns a pad.
 * - Pads are RENUMBERED PER HOST. TV pad 2 routed to the peer arrives there as
 *   that host's controller 1. A game handed "controller 2" with no controller 1
 *   mis-assigns split-screen slots and prompts for a pad that will never appear.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <Limelight.h>

/** Opaque, same forward declaration input_gamepad.h uses. Needed because rumble
 *  coming back from the peer has to be actuated on the real pad. */
typedef struct app_input_t app_input_t;

/** Coordinator's listening socket. Distinct from the video and audio links.
 *  PER-VARIANT: CMake passes -DCOOP_INPUT_SOCKET with an app-id suffix so the live and beta
 *  installs never share a /tmp socket (sticky-bit /tmp blocks cross-uid unlink). This literal
 *  is the fallback for builds/tests without the -D. */
#ifndef COOP_INPUT_SOCKET
#define COOP_INPUT_SOCKET "/tmp/aurora-coop-input.sock"
#endif

#define COOP_INPUT_MAGIC 0x43504931u   /* "CPI1" */

enum {
    COOP_INPUT_MSG_CONTROLLER = 1,  /**< buttons + triggers + sticks */
    COOP_INPUT_MSG_ARRIVE = 2,      /**< pad appeared: type/capabilities */
    COOP_INPUT_MSG_REMOVE = 3,      /**< pad gone */
    COOP_INPUT_MSG_SENSOR = 4,      /**< motion sample */
    COOP_INPUT_MSG_TOUCHPAD = 5,    /**< touch sample */
    COOP_INPUT_MSG_RUMBLE = 6,      /**< SECONDARY -> PRIMARY: low/high motors */
    COOP_INPUT_MSG_RUMBLE_TRIGGERS = 7, /**< SECONDARY -> PRIMARY: trigger motors */
    COOP_INPUT_MSG_MOUSE_POS = 8,   /**< absolute position, ALREADY PANE-LOCAL */
    COOP_INPUT_MSG_MOUSE_BUTTON = 9,/**< mouse button press/release */
    COOP_INPUT_MSG_MOUSE_SCROLL = 10, /**< wheel, both axes in one message */
    COOP_INPUT_MSG_KEYBOARD = 11,   /**< key press/release: lx = key code, sub = action,
                                     *   pad_type = modifiers (the LiSendKeyboardEvent args) */
};

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  type;        /**< enum above */
    uint8_t  slot;        /**< controller index AS THE RECEIVING HOST SHOULD SEE IT */
    uint16_t mask;        /**< active gamepad mask for that host */
    /* CONTROLLER */
    uint16_t buttons_lo;
    uint16_t buttons_hi;
    uint8_t  left_trigger, right_trigger;
    int16_t  lx, ly, rx, ry;
    /* ARRIVE */
    uint8_t  pad_type;
    uint16_t capabilities;
    uint32_t supported_buttons;
    /* SENSOR / TOUCHPAD / RUMBLE reuse the floats below */
    uint8_t  sub;         /**< sensor type or touch event type */
    uint32_t timestamp;
    float    f0, f1, f2;  /**< sensor xyz, or touch x/y/pressure */
    uint8_t  pointer_id;
    uint16_t motor_lo, motor_hi;  /**< RUMBLE */
} coop_input_msg_t;
#pragma pack(pop)

/**
 * Coordinator: start listening for the secondary. No-op outside co-op.
 * Failure is non-fatal -- pads assigned to the peer simply do not reach it.
 * @p input is kept so rumble returned by the peer can be actuated on the pad.
 */
void coop_input_coord_start(app_input_t *input);

/** Coordinator: stop listening and release the socket. Safe if never started. */
void coop_input_coord_stop(void);

/**
 * Service the coordinator's non-blocking socket from the primary UI loop.
 *
 * This accepts a waiting secondary, drains returned rumble packets, and makes
 * forward progress on buffered input whenever the socket becomes writable.
 * It never waits and is safe to call when co-op is inactive.
 */
void coop_input_coord_pump(void);

/** True when this process should route (co-op coordinator with the socket up). */
bool coop_input_coord_active(void);

/** Is a secondary ACTUALLY attached to the coordinator right now?
 *
 * Distinct from coop_input_coord_active(), which only says the socket is listening.
 * Anything that would send input to the peer must check this first, because coord_send()
 * drops silently when nobody is attached -- correct mid-session (a lost button beats a
 * stalled input thread) and disastrous when there is no peer at all, because then input
 * does not go anywhere and the user simply has no control. */
bool coop_input_peer_attached(void);

/**
 * Where should controller @p gs_id go? Returns COOP_PAD_PRIMARY / _SECONDARY /
 * _BOTH from app_configuration. Outside co-op this is always _PRIMARY, so the
 * ordinary single-host path is untouched.
 */
int coop_input_route(int gs_id);

/** True if this pad's events should still be sent on OUR connection. */
bool coop_input_wants_primary(int gs_id);

/**
 * The compact slot each host should see for source controller @p gs_id.
 * Returns -1 when that controller is not routed to the requested host.
 */
int coop_input_primary_slot(int gs_id);
int coop_input_peer_slot(int gs_id);

/** Map feedback from the primary host's compact id back to the physical pad. */
int coop_input_primary_source_slot(int primary_slot);

/** Filter the physical active-pad mask and pack it into each host's slot space. */
uint16_t coop_input_primary_mask(uint16_t active_mask);
uint16_t coop_input_peer_mask(uint16_t active_mask);

/* Coordinator -> secondary. All are no-ops unless the pad is routed to the peer
 * and the link is up. @p active_mask is the physical/source mask at this event;
 * it is filtered so the peer never sees configured-but-disconnected pads.
 * None of these calls block. */
void coop_input_send_controller(int gs_id, uint16_t active_mask,
                                short buttons, unsigned char left_trigger,
                                unsigned char right_trigger, short lx, short ly,
                                short rx, short ry);
void coop_input_send_arrive(int gs_id, uint16_t active_mask,
                            uint8_t type, uint16_t capabilities,
                            uint32_t supported_buttons);
void coop_input_send_remove(int gs_id, uint16_t active_mask);
void coop_input_send_sensor(int gs_id, uint16_t active_mask,
                            uint8_t sensor_type, uint32_t timestamp,
                            float x, float y, float z);
void coop_input_send_touchpad(int gs_id, uint16_t active_mask,
                              uint8_t event_type, uint32_t pointer_id,
                              float x, float y, float pressure);

/* THE POINTER, and unlike a pad it is not owned by anybody -- whichever pane it is
 * over is the one that gets it, decided in coop_pointer.c. The coordinates here are
 * ALREADY PANE-LOCAL and @p ref_w x @p ref_h is the PANE's size, not the TV's: the
 * peer must not have to know how the screen is split to put its cursor in the right
 * place. Same non-blocking contract as everything else on this socket. */
void coop_input_send_mouse_position(int x, int y, int ref_w, int ref_h);
void coop_input_send_mouse_button(int action, int button);
void coop_input_send_mouse_scroll(int vertical, int horizontal);
/** A key edge for the PEER's host -- keys follow the pane under the pointer (coop_pointer). */
void coop_input_send_keyboard(short key_code, char action, char modifiers);

/**
 * Secondary: connect to the coordinator and start applying received events to
 * this process's moonlight connection. Returns false if the socket is absent.
 */
bool coop_input_secondary_start(void);

/** Secondary: disconnect. Safe if never started. */
void coop_input_secondary_stop(void);

/**
 * Secondary: hand a rumble event back to the coordinator, which owns the pad.
 * Called from the secondary's connection listener.
 */
void coop_input_secondary_rumble(uint16_t controller, uint16_t low, uint16_t high);
void coop_input_secondary_rumble_triggers(uint16_t controller, uint16_t left, uint16_t right);

/** Diagnostics: events forwarded, dropped, and rumbles returned. */
unsigned coop_input_sent(void);
unsigned coop_input_dropped(void);
unsigned coop_input_rumbles(void);

#pragma once

/*
 * HOLD START FOR THE SHORTCUT WHEEL -- the Start gesture of Moonlight V+ (qiin2333/moonlight-vplus 4726655,
 * app/src/main/java/com/limelight/binding/input/StartGestureReducer.kt), as a pure state machine.
 *
 * WHY: the button combos need buttons not every controller has (View, stick clicks), and they failed on a Switch Pro
 * Controller on the SHIELD (user, 2026-10-21: "this needs to work with any controller a user connects"). Every
 * controller has Start. Hold it START_WHEEL_HOLD_MS and a wheel appears: push a stick or the d-pad towards Menu (up),
 * Mouse (right), Keyboard (down) or Performance (left), and let go of Start to choose what is highlighted.
 *
 * WHAT THE HOST SEES, as in V+: Start is passed on at once, so a short press is an ordinary Start. Until a direction is
 * pushed (or B) the wheel only shows and the controller still plays; the first push takes the controller (the host is
 * sent it at rest, once) and it stays taken until everything is let go. Letting go of Start without pushing anything
 * just closes the wheel.
 *
 * CHANGES FROM V+ (user, 2026-10-22, after trying it). The chosen action runs once the controller is let go, not at the
 * moment of choosing -- the menu and the keyboard take the controller's buttons the moment they open, so a Start or
 * d-pad still held would land in them; Eclipse's button combos act on release for the same reason. A stick let back to
 * the middle keeps the option it chose highlighted until it is pushed towards another (in V+ the middle meant
 * Continue). The d-pad moves the highlight a step per press instead of choosing when let go ("exiting instantly after
 * clicking any direction"): from the centre to that option, from an option back to the centre with the opposite
 * direction. A stick steps the same way, once per new direction it points ("it skips over [the centre] on a push of
 * the stick all the way to the other side"). B takes the highlight back to Continue, so a wheel opened by accident
 * can be left. Only Start's release chooses.
 */

#include <stdbool.h>
#include <stdint.h>

/** V+: ControllerHandler.START_DOWN_TIME_MOUSE_MODE_MS, also Moonlight's hold-Start-for-mouse time. */
#define START_WHEEL_HOLD_MS 750u

typedef enum start_wheel_action_t {
    START_WHEEL_CONTINUE = 0,   /* the centre: back to the game, nothing done */
    START_WHEEL_MENU,           /* up: the stream menu */
    START_WHEEL_MOUSE,          /* right: the virtual mouse on or off */
    START_WHEEL_KEYBOARD,       /* down: the on-screen keyboard */
    START_WHEEL_PERFORMANCE,    /* left: the performance panel pinned or unpinned */
} start_wheel_action_t;

typedef enum start_wheel_state_t {
    START_WHEEL_IDLE = 0,
    START_WHEEL_HELD,           /* Start down, not yet for START_WHEEL_HOLD_MS */
    START_WHEEL_VISIBLE,
    START_WHEEL_WAIT_RELEASE,   /* chosen; nothing reaches the host until the controller is let go */
} start_wheel_state_t;

typedef struct start_wheel_t {
    start_wheel_state_t state;
    uint32_t start_down_ms;
    start_wheel_action_t selected;
    int source;                     /* which control is choosing (start_wheel.c) */
    start_wheel_action_t candidate; /* a stick's new direction, until it has held for two events */
    int candidate_events;
    bool captured;                  /* a direction was pushed: the controller is the wheel's */
    start_wheel_action_t pending;   /* chosen, to run once the controller is let go */
    int held;                       /* buttons down at the last input: a d-pad or B press is a new one */
    start_wheel_action_t stick_dir; /* where the choosing stick points now; CONTINUE = back in the middle */
} start_wheel_t;

/** The controller as the wheel reads it: Limelight's *_FLAG buttons, and the sticks as sent to the host
 *  (-32767..32767, Y positive UP). */
typedef struct start_wheel_pad_t {
    int buttons;
    short lx, ly, rx, ry;
} start_wheel_pad_t;

/** What to do after an input. */
typedef struct start_wheel_update_t {
    bool show, hide, changed;   /* the wheel's picture: appear, go, or highlight `selected` */
    bool neutral;               /* send the host this controller at rest, once: the wheel just took it */
    bool consume;               /* this input does not reach the host */
    start_wheel_action_t run;   /* START_WHEEL_CONTINUE, or the action to run now */
} start_wheel_update_t;

void start_wheel_reset(start_wheel_t *wheel);

/** Start went down. The caller arranges a start_wheel_hold_elapsed() START_WHEEL_HOLD_MS later. */
start_wheel_update_t start_wheel_start_down(start_wheel_t *wheel, uint32_t now_ms);

/** The hold time is up: shows the wheel if Start is still held. */
start_wheel_update_t start_wheel_hold_elapsed(start_wheel_t *wheel, uint32_t now_ms);

/** Any change of the owning controller's buttons or sticks, Start's release included. */
start_wheel_update_t start_wheel_input(start_wheel_t *wheel, const start_wheel_pad_t *pad);

/** Abandon the gesture (a button combo formed, the stream menu opened, the controller went away). */
start_wheel_update_t start_wheel_cancel(start_wheel_t *wheel);

/** Whether the controller's input is the wheel's now, not the host's. */
bool start_wheel_consuming(const start_wheel_t *wheel);

const char *start_wheel_action_name(start_wheel_action_t action);

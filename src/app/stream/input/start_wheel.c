#include "start_wheel.h"

#include <Limelight.h>
#include <stddef.h>

/* V+ StartGestureReducer: a stick takes over past ENTER and lets go below EXIT; the hysteresis keeps a direction from
 * flickering at its edge. */
#define WHEEL_ENTER 0.65f
#define WHEEL_EXIT 0.35f
#define DPAD_FLAGS (UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG)

enum {
    SOURCE_NONE = 0,
    SOURCE_LEFT_STICK,
    SOURCE_RIGHT_STICK,
};

static float axis(short value) {
    return (float) value / 32767.0f;
}

static float absf(float value) {
    return value < 0.0f ? -value : value;
}

static float magnitude(float x, float y) {
    return absf(x) > absf(y) ? absf(x) : absf(y);
}

static bool active(float x, float y, float threshold) {
    return magnitude(x, y) >= threshold;
}

/** Anything still held: a button, or a stick off centre. */
static bool pressed(const start_wheel_pad_t *pad) {
    return pad->buttons != 0 || active(axis(pad->lx), axis(pad->ly), WHEEL_EXIT) ||
           active(axis(pad->rx), axis(pad->ry), WHEEL_EXIT);
}

static void clear_gesture(start_wheel_t *wheel) {
    wheel->start_down_ms = 0;
    wheel->selected = START_WHEEL_CONTINUE;
    wheel->source = SOURCE_NONE;
    wheel->candidate = START_WHEEL_CONTINUE;
    wheel->candidate_events = 0;
    wheel->captured = false;
}

void start_wheel_reset(start_wheel_t *wheel) {
    clear_gesture(wheel);
    wheel->state = START_WHEEL_IDLE;
    wheel->pending = START_WHEEL_CONTINUE;
    wheel->held = 0;
}

bool start_wheel_consuming(const start_wheel_t *wheel) {
    return (wheel->state == START_WHEEL_VISIBLE && wheel->captured) || wheel->state == START_WHEEL_WAIT_RELEASE;
}

start_wheel_update_t start_wheel_start_down(start_wheel_t *wheel, uint32_t now_ms) {
    start_wheel_update_t update = {0};
    if (wheel->state != START_WHEEL_IDLE) {
        return update;
    }
    clear_gesture(wheel);
    wheel->start_down_ms = now_ms;
    wheel->state = START_WHEEL_HELD;
    return update;
}

start_wheel_update_t start_wheel_hold_elapsed(start_wheel_t *wheel, uint32_t now_ms) {
    start_wheel_update_t update = {0};
    /* The timer and the event clock are different ticks; a few ms early is still the same hold. */
    if (wheel->state != START_WHEEL_HELD || now_ms - wheel->start_down_ms + 10 < START_WHEEL_HOLD_MS) {
        return update;
    }
    wheel->state = START_WHEEL_VISIBLE;
    wheel->source = SOURCE_NONE;
    update.show = true;
    return update;
}

start_wheel_update_t start_wheel_cancel(start_wheel_t *wheel) {
    start_wheel_update_t update = {0};
    update.hide = wheel->state == START_WHEEL_VISIBLE;
    start_wheel_reset(wheel);
    return update;
}

/** The chosen action is the controller's to run once it is let go -- now, if it already is. */
static void commit(start_wheel_t *wheel, const start_wheel_pad_t *pad, start_wheel_update_t *update) {
    const start_wheel_action_t chosen = wheel->selected;
    clear_gesture(wheel);
    update->hide = true;
    if (pressed(pad)) {
        wheel->pending = chosen;
        wheel->state = START_WHEEL_WAIT_RELEASE;
    } else {
        wheel->pending = START_WHEEL_CONTINUE;
        wheel->state = START_WHEEL_IDLE;
        update->run = chosen;
    }
}

/** Which stick chooses: the one already choosing while it stays out, else the one pushed further. */
static int resolve_source(const start_wheel_t *wheel, float lx, float ly, float rx, float ry) {
    switch (wheel->source) {
        case SOURCE_RIGHT_STICK:
            if (active(rx, ry, WHEEL_EXIT)) {
                return SOURCE_RIGHT_STICK;
            }
            break;
        case SOURCE_LEFT_STICK:
            if (active(lx, ly, WHEEL_EXIT)) {
                return SOURCE_LEFT_STICK;
            }
            break;
        default:
            break;
    }
    const float right = magnitude(rx, ry), left = magnitude(lx, ly);
    if (right < WHEEL_ENTER && left < WHEEL_ENTER) {
        return SOURCE_NONE;
    }
    return right >= left ? SOURCE_RIGHT_STICK : SOURCE_LEFT_STICK;
}

static bool direction_held(float x, float y, start_wheel_action_t action) {
    switch (action) {
        case START_WHEEL_MOUSE:
            return x >= WHEEL_EXIT && absf(x) >= absf(y);
        case START_WHEEL_PERFORMANCE:
            return x <= -WHEEL_EXIT && absf(x) >= absf(y);
        case START_WHEEL_MENU:
            return y <= -WHEEL_EXIT && absf(y) > absf(x);
        case START_WHEEL_KEYBOARD:
            return y >= WHEEL_EXIT && absf(y) > absf(x);
        default:
            return false;
    }
}

/** x right, y DOWN. */
static start_wheel_action_t action_for_axis(const start_wheel_t *wheel, float x, float y) {
    if (wheel->selected != START_WHEEL_CONTINUE && direction_held(x, y, wheel->selected)) {
        return wheel->selected;
    }
    if (x * x + y * y < WHEEL_EXIT * WHEEL_EXIT || magnitude(x, y) < WHEEL_ENTER) {
        return START_WHEEL_CONTINUE;
    }
    if (absf(x) >= absf(y)) {
        return x >= WHEEL_ENTER ? START_WHEEL_MOUSE : x <= -WHEEL_ENTER ? START_WHEEL_PERFORMANCE : START_WHEEL_CONTINUE;
    }
    return y <= -WHEEL_ENTER ? START_WHEEL_MENU : y >= WHEEL_ENTER ? START_WHEEL_KEYBOARD : START_WHEEL_CONTINUE;
}

/** A stick's new direction counts once it has held for two events (V+: two frames). */
static start_wheel_action_t stabilize(start_wheel_t *wheel, start_wheel_action_t candidate) {
    if (candidate == wheel->selected) {
        wheel->candidate = START_WHEEL_CONTINUE;
        wheel->candidate_events = 0;
        return candidate;
    }
    if (wheel->candidate != candidate) {
        wheel->candidate = candidate;
        wheel->candidate_events = 1;
        return wheel->selected;
    }
    if (++wheel->candidate_events >= 2) {
        wheel->candidate = START_WHEEL_CONTINUE;
        wheel->candidate_events = 0;
        return candidate;
    }
    return wheel->selected;
}

static start_wheel_action_t opposite(start_wheel_action_t action) {
    switch (action) {
        case START_WHEEL_MENU:
            return START_WHEEL_KEYBOARD;
        case START_WHEEL_KEYBOARD:
            return START_WHEEL_MENU;
        case START_WHEEL_MOUSE:
            return START_WHEEL_PERFORMANCE;
        case START_WHEEL_PERFORMANCE:
            return START_WHEEL_MOUSE;
        default:
            return START_WHEEL_CONTINUE;
    }
}

/** One d-pad press moves the highlight a step: from the centre to that option, from an option back to the centre
 *  when it points the other way, else to the option it points at. */
static start_wheel_action_t dpad_step(start_wheel_action_t from, int press) {
    const start_wheel_action_t to = (press & UP_FLAG) ? START_WHEEL_MENU
                                    : (press & RIGHT_FLAG) ? START_WHEEL_MOUSE
                                    : (press & DOWN_FLAG) ? START_WHEEL_KEYBOARD
                                    : START_WHEEL_PERFORMANCE;
    return from != START_WHEEL_CONTINUE && to == opposite(from) ? START_WHEEL_CONTINUE : to;
}

static void choose(start_wheel_t *wheel, const start_wheel_pad_t *pad, start_wheel_update_t *update) {
    /* Wheel axes are y DOWN, as on screen; the host's sticks are y UP. */
    const float lx = axis(pad->lx), ly = -axis(pad->ly), rx = axis(pad->rx), ry = -axis(pad->ry);
    const int presses = pad->buttons & ~wheel->held;
    wheel->held = pad->buttons;
    const int dpad = presses & DPAD_FLAGS;
    const bool back = (presses & B_FLAG) != 0;
    if (!wheel->captured && (dpad || back || active(lx, ly, WHEEL_ENTER) || active(rx, ry, WHEEL_ENTER))) {
        wheel->captured = true;
        update->neutral = true;
    }
    start_wheel_action_t next = wheel->selected;
    if (back) {
        next = START_WHEEL_CONTINUE;
    } else if (dpad) {
        next = dpad_step(wheel->selected, dpad);
    } else {
        wheel->source = resolve_source(wheel, lx, ly, rx, ry);
        if (wheel->source == SOURCE_NONE) {
            /* A stick let back to the middle keeps its choice while Start is held (user, 2026-10-22). */
            wheel->candidate = START_WHEEL_CONTINUE;
            wheel->candidate_events = 0;
            return;
        }
        const bool right = wheel->source == SOURCE_RIGHT_STICK;
        next = stabilize(wheel, action_for_axis(wheel, right ? rx : lx, right ? ry : ly));
    }
    if (back || dpad) {
        wheel->candidate = START_WHEEL_CONTINUE;
        wheel->candidate_events = 0;
    }
    if (next != wheel->selected) {
        wheel->selected = next;
        update->changed = true;
    }
}

start_wheel_update_t start_wheel_input(start_wheel_t *wheel, const start_wheel_pad_t *pad) {
    start_wheel_update_t update = {0};
    const bool was_consuming = start_wheel_consuming(wheel);
    const bool start = (pad->buttons & PLAY_FLAG) != 0;
    switch (wheel->state) {
        case START_WHEEL_HELD:
            if (!start) {
                /* A tap: Start was the host's, as any button. */
                start_wheel_reset(wheel);
            } else {
                wheel->held = pad->buttons;   /* what is down before the wheel shows is not a press on it */
            }
            break;
        case START_WHEEL_VISIBLE:
            if (start) {
                choose(wheel, pad, &update);
            } else if (wheel->captured) {
                commit(wheel, pad, &update);
            } else {
                /* Only looked: the wheel closes and Start's release is the host's. */
                start_wheel_reset(wheel);
                update.hide = true;
            }
            break;
        case START_WHEEL_WAIT_RELEASE:
            if (!pressed(pad)) {
                update.run = wheel->pending;
                start_wheel_reset(wheel);
            }
            break;
        default:
            break;
    }
    update.consume = was_consuming || update.neutral || start_wheel_consuming(wheel) ||
                     update.run != START_WHEEL_CONTINUE;
    return update;
}

const char *start_wheel_action_name(start_wheel_action_t action) {
    switch (action) {
        case START_WHEEL_MENU:
            return "menu";
        case START_WHEEL_MOUSE:
            return "mouse";
        case START_WHEEL_KEYBOARD:
            return "keyboard";
        case START_WHEEL_PERFORMANCE:
            return "performance";
        default:
            return "continue";
    }
}

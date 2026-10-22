#include "session_virt_mouse.h"
#include "session_input.h"

#include <SDL_stdinc.h>

#include "app.h"
#include "coop_input.h"
#include "coop_pointer.h"
#include "stream/session_priv.h"
#include "util/bus.h"

static short calc_mouse_movement(short axis);

static Uint32 vmouse_timer_callback(Uint32 interval, void *param);

/*
 * IN CO-OP THE VIRTUAL MOUSE POINTS LIKE THE MAGIC REMOTE.
 *
 * Outside co-op the stick sends deltas, and a delta carries no position, so in co-op it always moved
 * the first PC's pointer (coop_pointer.h). While the co-op split is up, the stick keeps a position of
 * its own across the whole TV and sends it through coop_pointer_position, the magic remote's path:
 * past the divider it is the other PC's pointer, landing at the matching edge, and clicks, the wheel
 * and the keys go to whichever PC it is over. The position is in host pixels of the composite (two
 * panes of the stream size), so a stick move covers the same distance on a PC as before. It starts
 * at the centre of the pane the pointer is routed to (the first move puts the PC's cursor there),
 * and is kept when the virtual mouse is turned off and on again.
 *
 * The co-op pointer's state belongs to the UI thread (coop_input_coord_pump) and this timer runs on
 * SDL's, so the timer only adds its deltas up and posts one apply to the UI thread at a time.
 *
 * Not when THIS PC is one of the co-op hosts (Windows, coop_pointer_set_local_pane): a position in
 * its pane hands the real cursor over to its virtual display, after which coop_pointer_position
 * drops every further one, so there the stick keeps sending deltas to the first PC as before.
 */
static void vmouse_coop_apply(void *data);

/** UI thread: whether the stick's steps go through the co-op pointer (see above). */
static bool vmouse_coop_wanted(const session_input_vmouse_t *vmouse) {
    return vmouse->state.active && coop_pointer_active() && !coop_pointer_has_local_pane();
}

/** Timer thread: true when the step was queued for the co-op pointer instead of sent. */
static bool vmouse_coop_queue(session_input_vmouse_t *vmouse, short dx, short dy, short scroll) {
    if (!SDL_AtomicGet(&vmouse->coop)) {
        return false;
    }
    SDL_AtomicAdd(&vmouse->pending_x, dx);
    SDL_AtomicAdd(&vmouse->pending_y, dy);
    SDL_AtomicAdd(&vmouse->pending_scroll, scroll);
    if (SDL_AtomicCAS(&vmouse->posted, 0, 1)) {
        app_t *app = vmouse->input->session->app;
        if (!app_bus_post(app, vmouse_coop_apply, app)) {
            SDL_AtomicSet(&vmouse->posted, 0);
        }
    }
    return true;
}

static int vmouse_clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/** UI thread: send what the timer has added up since the last apply. */
static void vmouse_coop_flush(session_input_vmouse_t *vmouse) {
    SDL_AtomicSet(&vmouse->posted, 0);
    int dx = SDL_AtomicSet(&vmouse->pending_x, 0);
    int dy = SDL_AtomicSet(&vmouse->pending_y, 0);
    int scroll = SDL_AtomicSet(&vmouse->pending_scroll, 0);
    if (!coop_pointer_active() || !coop_input_peer_attached()) {
        /* No split, or no second PC yet: the deltas go to the first PC as they always did. */
        vmouse->placed = false;
        if (dx != 0 || dy != 0) {
            LiSendMouseMoveEvent((short) dx, (short) dy);
        }
        if (scroll != 0) {
            LiSendHighResScrollEvent((short) scroll);
        }
        return;
    }
    if (scroll != 0) {
        coop_pointer_scroll_high_res(scroll);
    }
    if (dx == 0 && dy == 0) {
        return;
    }
    const STREAM_CONFIGURATION *stream = &vmouse->input->session->config.stream;
    const bool vertical = coop_pointer_split_vertical();
    const int ref_w = vertical ? stream->width * 2 : stream->width;
    const int ref_h = vertical ? stream->height : stream->height * 2;
    if (ref_w <= 1 || ref_h <= 1) {
        return;
    }
    if (!vmouse->placed) {
        coop_pointer_pane_centre(ref_w, ref_h, &vmouse->x, &vmouse->y);
        vmouse->placed = true;
    }
    int lo_x = 0, hi_x = ref_w - 1, lo_y = 0, hi_y = ref_h - 1;
    if (vmouse->state.l || vmouse->state.r) {
        /* A held trigger pins the pane, as coop_pointer does for every pointer: keep the position
         * inside it too, or the pointer would sit past the divider and jump there on release. */
        if (vertical) {
            if (vmouse->x < ref_w / 2) { hi_x = ref_w / 2 - 1; } else { lo_x = ref_w / 2; }
        } else {
            if (vmouse->y < ref_h / 2) { hi_y = ref_h / 2 - 1; } else { lo_y = ref_h / 2; }
        }
    }
    vmouse->x = vmouse_clamp(vmouse->x + dx, lo_x, hi_x);
    vmouse->y = vmouse_clamp(vmouse->y + dy, lo_y, hi_y);
    coop_pointer_position(vmouse->x, vmouse->y, ref_w, ref_h);
}

static void vmouse_coop_apply(void *data) {
    app_t *app = data;
    /* The session the steps were queued in may have ended since; a new one has nothing pending. */
    if (app->session != NULL) {
        vmouse_coop_flush(&app->session->input.vmouse);
    }
}

void session_input_set_vmouse_active(session_input_vmouse_t *vmouse, bool active) {
    vmouse->state.active = active;
    /* Latched here, on the UI thread that owns the co-op pointer, for the timer thread to read. */
    SDL_AtomicSet(&vmouse->coop, vmouse_coop_wanted(vmouse));
    if (!active) {
        vmouse_set_vector(vmouse, 0, 0);
        vmouse_set_trigger(vmouse, 0, 0);
    }
}

bool session_input_is_vmouse_active(session_input_vmouse_t *vmouse) {
    return vmouse->state.active;
}

void vmouse_set_vector(session_input_vmouse_t *vmouse, short x, short y) {
    /* Again on every stick move (UI thread too), so a split that comes up, or goes, after the virtual
     * mouse was turned on is followed without turning it off and on. */
    SDL_AtomicSet(&vmouse->coop, vmouse_coop_wanted(vmouse));
    vmouse->state.x = calc_mouse_movement(x);
    vmouse->state.y = calc_mouse_movement((short) -SDL_max(y, -32767));
    if (vmouse->state.x || vmouse->state.y) {
        if (!vmouse->timer_id) {
            vmouse->timer_id = SDL_AddTimer(0, vmouse_timer_callback, vmouse);
        }
    } else if (vmouse->timer_id) {
        SDL_RemoveTimer(vmouse->timer_id);
        vmouse->timer_id = 0;
    }
}

static void vmouse_send_button(bool down, int button) {
    const int action = down ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE;
    if (coop_pointer_has_local_pane()) {
        LiSendMouseButtonEvent((char) action, button);
    } else {
        coop_pointer_button(action, button);
    }
}

void vmouse_set_trigger(session_input_vmouse_t *vmouse, char l, char r) {
    /* As unsigned: a full pull is 255, and where char is signed (x86: Windows, Linux) that read as -1,
     * i.e. not pressed, so a trigger pulled all the way let go of the button. */
    const unsigned char trigger_threshold = 64;
    bool ldown = (unsigned char) l > trigger_threshold, rdown = (unsigned char) r > trigger_threshold;
    if (!coop_pointer_has_local_pane() && (vmouse->state.l != ldown || vmouse->state.r != rdown)) {
        /* Click where the pointer is now, on the PC it is over (coop_pointer_button routes it). */
        vmouse_coop_flush(vmouse);
    }
    if (vmouse->state.l != ldown) {
        vmouse_send_button(ldown, BUTTON_LEFT);
    }
    if (vmouse->state.r != rdown) {
        vmouse_send_button(rdown, BUTTON_RIGHT);
    }
    vmouse->state.l = ldown;
    vmouse->state.r = rdown;
}

void vmouse_set_modifier(session_input_vmouse_t *vmouse, bool v) {
    vmouse->state.modifier = v;
}

static Uint32 vmouse_timer_callback(Uint32 interval, void *param) {
    (void) interval;
    session_input_vmouse_t *vmouse = param;
    if (!vmouse->state.active || (!vmouse->state.x && !vmouse->state.y)) {
        return 0;
    }
    short speed = 4;
    double speed_divider = 32 - SDL_max(0, SDL_min(speed, 16));
    double x = vmouse->state.x / speed_divider;
    double y = vmouse->state.y / speed_divider;
    double abs_x = SDL_fabs(x), abs_y = SDL_fabs(y);
    if (vmouse->state.modifier) {
        abs_y /= 20.0;
        const short amount = (short) (abs_y > 1 ? -y : -y / abs_y);
        if (!vmouse_coop_queue(vmouse, 0, 0, amount)) {
            LiSendHighResScrollEvent(amount);
        }
    } else {
        const short dx = (short) (abs_x > 1 ? x : x / abs_x), dy = (short) (abs_y > 1 ? y : y / abs_y);
        if (!vmouse_coop_queue(vmouse, dx, dy, 0)) {
            LiSendMouseMoveEvent(dx, dy);
        }
    }
    return SDL_max(5, SDL_min(5 / SDL_max(abs_x, abs_y), 20));
}

static short calc_mouse_movement(short axis) {
    short abs_axis = (short) (axis > 0 ? axis : -axis);
    short threshold = 4096;
    if (abs_axis < threshold) { return 0; }
    return (short) (SDL_sqrt(abs_axis - threshold) * (axis > 0 ? 1 : -1));
}
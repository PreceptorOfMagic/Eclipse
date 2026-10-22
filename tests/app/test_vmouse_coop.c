/*
 * The virtual mouse in co-op (stream/input/session_virt_mouse.c, together with the real stream/input/coop_pointer.c).
 *
 * Outside co-op the stick's deltas move the first PC's pointer. While the co-op split is up, the stick keeps a
 * position of its own and points like the magic remote: past the divider it is the second PC's pointer, and
 * clicks and the wheel go to whichever PC it is over. The timer thread only adds steps up; the UI thread applies them
 * (here: the test runs the posted action itself). What reaches each PC is recorded by fake Limelight and co-op link calls.
 */
#include SESSION_VIRT_MOUSE_SOURCE

#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { TO_NONE = 0, TO_PRIMARY, TO_PEER };

static struct { int target, x, y, w, h, count; } pos;
static struct { int dx, dy, count; } rel;
static struct { int target, action, button, count; } btn;
static struct { int target, amount, count; } scr;
static bus_actionfunc posted_action;
static void *posted_data;
static int posts;
static bool peer_attached = true;

int LiSendMousePositionEvent(short x, short y, short w, short h) {
    pos.target = TO_PRIMARY; pos.x = x; pos.y = y; pos.w = w; pos.h = h; pos.count++;
    return 0;
}
int LiSendMouseMoveEvent(short dx, short dy) { rel.dx += dx; rel.dy += dy; rel.count++; return 0; }
int LiSendMouseButtonEvent(char action, int button) {
    btn.target = TO_PRIMARY; btn.action = action; btn.button = button; btn.count++;
    return 0;
}
int LiSendHighResScrollEvent(short amount) { scr.target = TO_PRIMARY; scr.amount += amount; scr.count++; return 0; }
int LiSendScrollEvent(signed char clicks) { scr.target = TO_PRIMARY; scr.amount += clicks * 120; scr.count++; return 0; }
int LiSendHScrollEvent(signed char clicks) { (void) clicks; return 0; }

void coop_input_send_mouse_position(int x, int y, int w, int h) {
    pos.target = TO_PEER; pos.x = x; pos.y = y; pos.w = w; pos.h = h; pos.count++;
}
void coop_input_send_mouse_button(int action, int button) {
    btn.target = TO_PEER; btn.action = action; btn.button = button; btn.count++;
}
void coop_input_send_mouse_scroll(int vertical, int horizontal) {
    (void) horizontal; scr.target = TO_PEER; scr.amount += vertical * 120; scr.count++;
}
bool coop_input_peer_attached(void) { return peer_attached; }

bool app_bus_post(app_t *app, bus_actionfunc action, void *data) {
    (void) app; posted_action = action; posted_data = data; posts++;
    return true;
}

static int fails;
static void ck(const char *what, int ok) {
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) { fails++; }
}

static app_t app;
static session_t session;
static session_input_vmouse_t *vm;

static void reset_capture(void) {
    memset(&pos, 0, sizeof(pos)); memset(&rel, 0, sizeof(rel));
    memset(&btn, 0, sizeof(btn)); memset(&scr, 0, sizeof(scr));
    posts = 0;
}

/** The UI thread's turn: run what the timer posted, if anything. */
static void ui_pass(void) {
    if (posted_action != NULL) {
        bus_actionfunc action = posted_action;
        posted_action = NULL;
        action(posted_data);
    }
}

/** One timer step of (dx, dy), then the UI pass. */
static void step(short dx, short dy) {
    if (!vmouse_coop_queue(vm, dx, dy, 0)) {
        LiSendMouseMoveEvent(dx, dy);
    }
    ui_pass();
}

static void arm_split_only(bool vertical) {
    coop_pointer_stop();
    unlink(COOP_PTR_VMIRROR_SENTINEL);
    if (vertical) {
        FILE *f = fopen(COOP_PTR_VMIRROR_SENTINEL, "w");
        if (f != NULL) { fclose(f); }
    }
    coop_pointer_start();
}

static void arm(bool vertical, int pane_w, int pane_h) {
    arm_split_only(vertical);
    memset(&session, 0, sizeof(session));
    session.app = &app;
    session.config.stream.width = pane_w;
    session.config.stream.height = pane_h;
    session.input.session = &session;
    session.input.vmouse.input = &session.input;     /* as session_input_init does */
    app.session = &session;
    vm = &session.input.vmouse;
    session_input_set_vmouse_active(vm, true);
    reset_capture();
}

int main(void) {
    printf("1. left | right panes of 1920x1080: the stick starts in the middle of the first PC's pane\n");
    arm(true, 1920, 1080);
    ck("latched for co-op when turned on", SDL_AtomicGet(&vm->coop) == 1);
    step(6, 0);
    ck("the first PC gets (966, 540) of its 1920x1080", pos.target == TO_PRIMARY && pos.x == 966 && pos.y == 540 &&
       pos.w == 1920 && pos.h == 1080);
    ck("no relative move was sent", rel.count == 0);

    printf("2. pushed right past the divider: it is the second PC's pointer, at its left edge\n");
    for (int i = 0; i < 159; ++i) { step(6, 0); }      /* 966 + 954 = 1920 */
    ck("the second PC gets (0, 540) of its 1920x1080", pos.target == TO_PEER && pos.x == 0 && pos.y == 540 &&
       pos.w == 1920 && pos.h == 1080);
    for (int i = 0; i < 400; ++i) { step(6, 6); }
    ck("held at the right and bottom edges", pos.target == TO_PEER && pos.x == 1919 && pos.y == 1079);

    printf("3. a click goes to the PC the pointer is over; while held it cannot cross back\n");
    for (int i = 0; i < 100; ++i) { step(-6, -6); }    /* x 3839 -> 3239 in the composite, y 1079 -> 479 */
    reset_capture();
    vmouse_set_trigger(vm, 0, 127);
    ck("right trigger: press to the second PC", btn.target == TO_PEER && btn.button == BUTTON_RIGHT &&
       btn.action == BUTTON_ACTION_PRESS);
    for (int i = 0; i < 250; ++i) { step(-6, 0); }     /* 1500 left would be past the divider at 1920 */
    ck("dragged left, it stops at the second PC's left edge", pos.target == TO_PEER && pos.x == 0);
    vmouse_set_trigger(vm, 0, 0);
    ck("release to the second PC", btn.target == TO_PEER && btn.action == BUTTON_ACTION_RELEASE);
    step(-6, 0);
    ck("let go, the next move crosses to the first PC's right edge", pos.target == TO_PRIMARY && pos.x == 1914);

    printf("4. the wheel goes where the pointer is\n");
    reset_capture();
    for (int i = 0; i < 3; ++i) { vmouse_coop_queue(vm, 0, 0, -20); ui_pass(); }
    ck("first PC: high-resolution steps as before", scr.target == TO_PRIMARY && scr.amount == -60 && scr.count == 3);
    for (int i = 0; i < 2; ++i) { step(6, 0); }        /* back over to the second PC */
    reset_capture();
    for (int i = 0; i < 7; ++i) { vmouse_coop_queue(vm, 0, 0, 20); ui_pass(); }
    ck("second PC: one whole notch once 120 has built up", scr.target == TO_PEER && scr.amount == 120 && scr.count == 1);

    printf("5. steps the UI has not got to yet are posted once and applied together\n");
    reset_capture();
    vmouse_coop_queue(vm, 6, 0, 0);
    vmouse_coop_queue(vm, 6, 0, 0);
    vmouse_coop_queue(vm, 6, 0, 0);
    ck("one post for three steps", posts == 1);
    const int before = vm->x;
    ui_pass();
    ck("one position, 18 further on", pos.count == 1 && vm->x == before + 18);
    vmouse_coop_queue(vm, 6, 0, 0);
    ck("posted again after the UI pass", posts == 2);
    ui_pass();

    printf("6. turned off and on again, it carries on from where it was\n");
    const int kept_x = vm->x, kept_y = vm->y;
    session_input_set_vmouse_active(vm, false);
    session_input_set_vmouse_active(vm, true);
    reset_capture();
    step(0, 6);
    ck("same place, 6 lower", vm->x == kept_x && vm->y == kept_y + 6);

    printf("7. through the timer itself: a full push right and down\n");
    reset_capture();
    vm->state.x = 169;
    vm->state.y = 169;
    vmouse_timer_callback(0, vm);
    ui_pass();
    ck("queued and applied as a position, not a delta", pos.count == 1 && rel.count == 0);
    vm->state.x = vm->state.y = 0;

    printf("8. no second PC attached yet: deltas to the first PC, as before\n");
    peer_attached = false;
    reset_capture();
    step(6, -6);
    ck("a relative move, no position", rel.count == 1 && rel.dx == 6 && rel.dy == -6 && pos.count == 0);
    peer_attached = true;

    printf("9. top / bottom panes of 3840x1088: starts in the middle of the top pane, crosses down\n");
    arm(false, 3840, 1088);
    step(0, 6);
    ck("the first PC gets (1920, 550) of its 3840x1088", pos.target == TO_PRIMARY && pos.x == 1920 &&
       pos.y == 550 && pos.w == 3840 && pos.h == 1088);
    for (int i = 0; i < 90; ++i) { step(0, 6); }       /* 550 + 540 = 1090 */
    ck("the second PC gets (1920, 2) of its 3840x1088", pos.target == TO_PEER && pos.x == 1920 && pos.y == 2 &&
       pos.h == 1088);

    printf("10. turned on before the split was up: deltas, as elsewhere\n");
    coop_pointer_stop();
    memset(&session, 0, sizeof(session));
    session.app = &app;
    session.input.session = &session;
    session.input.vmouse.input = &session.input;
    vm = &session.input.vmouse;
    session_input_set_vmouse_active(vm, true);
    reset_capture();
    vm->state.x = 169;
    vm->state.y = 169;
    vmouse_timer_callback(0, vm);
    ck("not latched, sent straight from the timer", SDL_AtomicGet(&vm->coop) == 0 && rel.count == 1 && posts == 0);
    vm->state.x = vm->state.y = 0;
    vmouse_set_trigger(vm, 127, 0);
    ck("a click goes to the first PC", btn.target == TO_PRIMARY && btn.button == BUTTON_LEFT);
    vmouse_set_trigger(vm, 0, 0);
    arm_split_only(true);
    vmouse_set_vector(vm, 0, 0);
    ck("the split comes up later: the next stick move latches it", SDL_AtomicGet(&vm->coop) == 1);

    printf("11. a trigger pulled all the way (255) is a press, whatever char's sign\n");
    arm(true, 1920, 1080);
    vmouse_set_trigger(vm, (char) 255, 0);
    ck("full left pull: press", btn.button == BUTTON_LEFT && btn.action == BUTTON_ACTION_PRESS && btn.count == 1);
    vmouse_set_trigger(vm, 0, 0);
    ck("let go: release", btn.button == BUTTON_LEFT && btn.action == BUTTON_ACTION_RELEASE && btn.count == 2);

    printf("12. THIS PC is one of the co-op hosts (Windows): deltas and clicks to the first PC, as before\n");
    coop_pointer_set_local_pane(0);
    arm(true, 1920, 1080);
    ck("not latched", SDL_AtomicGet(&vm->coop) == 0);
    vm->state.x = 169;
    vm->state.y = 169;
    vmouse_timer_callback(0, vm);
    ck("a relative move, no position, nothing posted", rel.count == 1 && pos.count == 0 && posts == 0);
    vm->state.x = vm->state.y = 0;
    vmouse_set_trigger(vm, 127, 0);
    ck("a click goes to the first PC", btn.target == TO_PRIMARY && btn.button == BUTTON_LEFT &&
       btn.action == BUTTON_ACTION_PRESS);
    vmouse_set_trigger(vm, 0, 0);
    coop_pointer_set_local_pane(-1);

    unlink(COOP_PTR_VMIRROR_SENTINEL);
    printf(fails == 0 ? "\nALL PASS\n" : "\n%d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}

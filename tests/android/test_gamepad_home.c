/*
 * View + Start shows the TV's home screen on Android (stream/input/session_gamepad.c, compiled here with TARGET_ANDROID).
 *
 * The SHIELD TV turns a controller's Guide button into Home before any app sees it (2026-10-16). With that rewrite
 * turned off, so that Guide reaches the host as Guide, a controller needs another way home during a stream: View +
 * Start, acted on when both are let go. Guide itself still goes to the host, and View + Start + LB + RB still opens
 * the stream menu. What reaches the host is recorded by a fake LiSendMultiControllerEvent.
 *
 * Where the system takes Guide (the SHIELD TV), holding View is the Guide button, as Sunshine's back_button_timeout
 * does it: after GUIDE_HOLD_MS the host sees View let go, then Guide pressed and let go GUIDE_TAP_MS later, and View
 * stays let go until it is. The stream menu's Guide button taps Guide on the first controller. The LVGL timers are
 * fired by hand here, which is what LVGL does when their time is up.
 *
 * And the virtual mouse is told when its stick is let go and its trigger comes up, however fast -- from the controller
 * that was driving it only (recorded by fake vmouse_set_vector / vmouse_set_trigger).
 */
#include SESSION_GAMEPAD_SOURCE

#include <stdio.h>

static short sent[64];
static int sent_n;
static int overlay_opened;
static int went_home;

void android_go_home(void) { went_home++; }

static bool guide_is_home;
bool android_guide_is_home(void) { return guide_is_home; }
app_t *global = NULL;
static app_settings_t settings;
PCONFIGURATION app_configuration = &settings;

static intptr_t wheel_picture_code = -1;   /* the last USER_START_WHEEL: 0 hidden, else 1 + the highlighted action */
static int vmouse_toggles;

int LiSendMultiControllerEvent(short controllerNumber, short activeGamepadMask, int buttonFlags,
                               unsigned char leftTrigger, unsigned char rightTrigger, short leftStickX,
                               short leftStickY, short rightStickX, short rightStickY) {
    (void) controllerNumber; (void) activeGamepadMask; (void) leftTrigger; (void) rightTrigger;
    (void) leftStickX; (void) leftStickY; (void) rightStickX; (void) rightStickY;
    if (sent_n < (int) (sizeof(sent) / sizeof(sent[0]))) { sent[sent_n++] = (short) buttonFlags; }
    return 0;
}
int LiSendControllerArrivalEvent(uint8_t n, uint16_t m, uint8_t t, uint32_t b, uint16_t c) {
    (void) n; (void) m; (void) t; (void) b; (void) c; return 0;
}
int LiSendControllerMotionEvent(uint8_t n, uint8_t t, float x, float y, float z) {
    (void) n; (void) t; (void) x; (void) y; (void) z; return 0;
}
int LiSendControllerTouchEvent(uint8_t n, uint8_t t, uint32_t p, float x, float y, float pr) {
    (void) n; (void) t; (void) p; (void) x; (void) y; (void) pr; return 0;
}

static app_gamepad_state_t pad, pad2;   /* pad2: a second controller, instance 2 */
app_gamepad_state_t *app_input_gamepad_state_by_instance_id(app_input_t *input, SDL_JoystickID id) {
    (void) input; return id == 2 ? &pad2 : &pad;
}
app_gamepad_state_t *app_input_gamepad_state_by_index(app_input_t *input, int index) {
    (void) input; (void) index; return &pad;
}
bool app_input_gamepad_swaps_abxy(SDL_GameController *controller) { (void) controller; return false; }
short app_input_get_max_gamepads(app_input_t *input) { (void) input; return 4; }

bool bus_pushevent(int which, void *data1, void *data2) {
    (void) data2;
    if (which == USER_OPEN_OVERLAY) { overlay_opened++; }
    if (which == USER_START_WHEEL) { wheel_picture_code = (intptr_t) data1; }
    return true;
}
void commons_log_printf(commons_log_level level, const char *tag, const char *fmt, ...) {
    (void) level; (void) tag; (void) fmt;
}

int coop_input_primary_slot(int gs_id) { return gs_id; }
uint16_t coop_input_primary_mask(uint16_t mask) { return mask; }
void coop_input_send_arrive(int gs_id, uint16_t mask, uint8_t type, uint16_t caps, uint32_t buttons) {
    (void) gs_id; (void) mask; (void) type; (void) caps; (void) buttons;
}
void coop_input_send_remove(int gs_id, uint16_t mask) { (void) gs_id; (void) mask; }
void coop_input_send_controller(int gs_id, uint16_t mask, short buttons, unsigned char lt, unsigned char rt,
                                short lx, short ly, short rx, short ry) {
    (void) gs_id; (void) mask; (void) buttons; (void) lt; (void) rt; (void) lx; (void) ly; (void) rx; (void) ry;
}
void coop_input_send_sensor(int gs_id, uint16_t mask, uint8_t type, uint32_t ts, float x, float y, float z) {
    (void) gs_id; (void) mask; (void) type; (void) ts; (void) x; (void) y; (void) z;
}
void coop_input_send_touchpad(int gs_id, uint16_t mask, uint8_t type, uint32_t id, float x, float y, float p) {
    (void) gs_id; (void) mask; (void) type; (void) id; (void) x; (void) y; (void) p;
}
/* The virtual mouse as the stick and triggers drive it: what it was last told, and how often. */
static bool vmouse_active;
static short vm_x, vm_y;
static int vm_vec_calls, vm_trig_calls;
static char vm_l, vm_r;
bool session_input_is_vmouse_active(session_input_vmouse_t *vmouse) { (void) vmouse; return vmouse_active; }
void session_toggle_vmouse(session_t *session) { (void) session; vmouse_toggles++; }
void vmouse_set_modifier(session_input_vmouse_t *vmouse, bool pressed) { (void) vmouse; (void) pressed; }
void vmouse_set_trigger(session_input_vmouse_t *vmouse, char l, char r) {
    (void) vmouse; vm_l = l; vm_r = r; vm_trig_calls++;
}
void vmouse_set_vector(session_input_vmouse_t *vmouse, short x, short y) {
    (void) vmouse; vm_x = x; vm_y = y; vm_vec_calls++;
}

static stream_input_t input;
static app_input_t app_input;

static void button(SDL_GameControllerButton b, bool down) {
    SDL_ControllerButtonEvent e;
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    e.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.button = (Uint8) b;
    stream_input_handle_cbutton(&input, &e);
}

static void stick(short value) {
    SDL_ControllerAxisEvent e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_CONTROLLERAXISMOTION;
    e.axis = SDL_CONTROLLER_AXIS_LEFTX;
    e.value = value;
    stream_input_handle_caxis(&input, &e);
}

/** An axis of the controller with instance @p which: the right stick across, or a trigger. */
static void axis(SDL_JoystickID which, SDL_GameControllerAxis a, short value) {
    SDL_ControllerAxisEvent e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_CONTROLLERAXISMOTION;
    e.which = which;
    e.axis = (Uint8) a;
    e.value = value;
    stream_input_handle_caxis(&input, &e);
}

static int failures;
static void expect_sent(const char *what, const short *want, int n) {
    int ok = sent_n == n;
    for (int i = 0; ok && i < n; ++i) { ok = sent[i] == want[i]; }
    if (!ok) {
        fprintf(stderr, "FAIL: %s: sent", what);
        for (int i = 0; i < sent_n; ++i) { fprintf(stderr, " 0x%04x", (unsigned) (unsigned short) sent[i]); }
        fprintf(stderr, ", want");
        for (int i = 0; i < n; ++i) { fprintf(stderr, " 0x%04x", (unsigned) (unsigned short) want[i]); }
        fprintf(stderr, "\n");
        failures++;
    } else {
        printf("ok: %s\n", what);
    }
    sent_n = 0;
}

static void check(const char *what, bool ok) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    } else {
        printf("ok: %s\n", what);
    }
}

/** What LVGL does when a run-once timer's time is up: run it, then delete it. */
static void fire(void **slot) {
    lv_timer_t *timer = *slot;
    if (timer == NULL) {
        fprintf(stderr, "FAIL: no timer to fire\n");
        failures++;
        return;
    }
    timer->timer_cb(timer);
    lv_timer_del(timer);
}

int main(void) {
    input.input = &app_input;
    app_input.activeGamepadMask = 1;
    pad.gs_id = 0;
    /* The button combos alone until the Start wheel's own cases at the end: its hold timer needs LVGL running. */
    settings.controller_shortcuts = CONTROLLER_SHORTCUTS_COMBOS;

    /* View, then Start, then both let go: the host gets View only, and the home screen opens on release. */
    button(SDL_CONTROLLER_BUTTON_BACK, true);
    button(SDL_CONTROLLER_BUTTON_START, true);
    stick(12000);
    button(SDL_CONTROLLER_BUTTON_START, false);
    button(SDL_CONTROLLER_BUTTON_BACK, false);
    /* View reaches the host alone; from the moment Start joins it, the host sees neither -- through stick
     * movement and through letting go of either -- and never sees Start or Guide. */
    expect_sent("View + Start is taken back and never reaches the host as such", (const short[]) {BACK_FLAG, 0, 0, 0, 0}, 5);
    if (went_home != 1) {
        fprintf(stderr, "FAIL: View + Start went home %d times\n", went_home);
        failures++;
    } else {
        printf("ok: View + Start shows the home screen\n");
    }
    stick(0);
    sent_n = 0;
    went_home = 0;

    /* Start alone and View alone are themselves. */
    button(SDL_CONTROLLER_BUTTON_START, true);
    button(SDL_CONTROLLER_BUTTON_START, false);
    button(SDL_CONTROLLER_BUTTON_BACK, true);
    button(SDL_CONTROLLER_BUTTON_BACK, false);
    expect_sent("Start and View alone are unchanged", (const short[]) {PLAY_FLAG, 0, BACK_FLAG, 0}, 4);

    /* A real Guide button (where the platform delivers it) is still Guide. */
    button(SDL_CONTROLLER_BUTTON_GUIDE, true);
    button(SDL_CONTROLLER_BUTTON_GUIDE, false);
    expect_sent("Guide button itself", (const short[]) {SPECIAL_FLAG, 0}, 2);

    /* The stream menu combo (View + Start + LB + RB) still opens the menu. */
    button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, true);
    button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, true);
    button(SDL_CONTROLLER_BUTTON_BACK, true);
    button(SDL_CONTROLLER_BUTTON_START, true);
    button(SDL_CONTROLLER_BUTTON_START, false);
    button(SDL_CONTROLLER_BUTTON_BACK, false);
    button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, false);
    button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, false);
    if (went_home != 0) {
        fprintf(stderr, "FAIL: the stream menu combo also went home\n");
        failures++;
    }
    if (overlay_opened != 1) {
        fprintf(stderr, "FAIL: the stream menu combo opened the menu %d times\n", overlay_opened);
        failures++;
    } else {
        printf("ok: View + Start + LB + RB still opens the stream menu\n");
    }

    /* ── Where the system takes Guide: View held = Guide, as Sunshine does it ── */
    lv_init();
    static app_t app;
    static session_t session;
    session.input.input = &app_input;
    app.session = &session;
    global = &app;
    sent_n = 0;
    overlay_opened = 0;

    /* Not where Guide reaches the app: View is only View. */
    button(SDL_CONTROLLER_BUTTON_BACK, true);
    check("no hold timer where the system leaves Guide alone", pad.guide_hold_timer == NULL);
    button(SDL_CONTROLLER_BUTTON_BACK, false);
    expect_sent("View alone, as before", (const short[]) {BACK_FLAG, 0}, 2);

    guide_is_home = true;

    /* Held: View, then View let go, Guide, Guide let go; letting go of View afterwards changes nothing. */
    button(SDL_CONTROLLER_BUTTON_BACK, true);
    check("a hold timer for View", pad.guide_hold_timer != NULL);
    fire(&pad.guide_hold_timer);
    check("a tap timer for Guide", pad.guide_tap_timer != NULL);
    fire(&pad.guide_tap_timer);
    button(SDL_CONTROLLER_BUTTON_BACK, false);
    expect_sent("View held: View, View up, Guide, Guide up, then nothing new",
                (const short[]) {BACK_FLAG, 0, SPECIAL_FLAG, 0, 0}, 5);

    /* Let go before the time is up: only View, and the timer is gone. */
    button(SDL_CONTROLLER_BUTTON_BACK, true);
    button(SDL_CONTROLLER_BUTTON_BACK, false);
    check("a short press cancels the hold", pad.guide_hold_timer == NULL);
    expect_sent("View tapped is View", (const short[]) {BACK_FLAG, 0}, 2);

    /* Other buttons held with View are still sent; Guide joins them and View stays up while they change. */
    button(SDL_CONTROLLER_BUTTON_BACK, true);
    button(SDL_CONTROLLER_BUTTON_A, true);
    fire(&pad.guide_hold_timer);
    button(SDL_CONTROLLER_BUTTON_A, false);
    fire(&pad.guide_tap_timer);
    button(SDL_CONTROLLER_BUTTON_BACK, false);
    expect_sent("View + A held: A stays, View goes up, Guide taps",
                (const short[]) {BACK_FLAG, BACK_FLAG | A_FLAG, A_FLAG, A_FLAG | SPECIAL_FLAG, SPECIAL_FLAG, 0, 0}, 7);

    /* View let go during the Guide tap: Guide still lets go on time. */
    button(SDL_CONTROLLER_BUTTON_BACK, true);
    fire(&pad.guide_hold_timer);
    button(SDL_CONTROLLER_BUTTON_BACK, false);
    fire(&pad.guide_tap_timer);
    expect_sent("View let go during the tap", (const short[]) {BACK_FLAG, 0, SPECIAL_FLAG, SPECIAL_FLAG, 0}, 5);

    /* View + Start (the way home) held past the time: no Guide, still home. */
    went_home = 0;
    button(SDL_CONTROLLER_BUTTON_BACK, true);
    button(SDL_CONTROLLER_BUTTON_START, true);
    fire(&pad.guide_hold_timer);
    check("no Guide during View + Start", pad.guide_tap_timer == NULL);
    button(SDL_CONTROLLER_BUTTON_START, false);
    button(SDL_CONTROLLER_BUTTON_BACK, false);
    check("View + Start still goes home", went_home == 1);
    sent_n = 0;

    /* The stream menu's Guide button. */
    stream_input_send_guide(&session.input);
    fire(&pad.guide_tap_timer);
    expect_sent("stream menu Guide taps Guide", (const short[]) {SPECIAL_FLAG, 0}, 2);

    /* The session gone before the hold is up: nothing is sent. */
    button(SDL_CONTROLLER_BUTTON_BACK, true);
    app.session = NULL;
    fire(&pad.guide_hold_timer);
    check("no Guide after the session ended", pad.guide_tap_timer == NULL);
    app.session = &session;
    button(SDL_CONTROLLER_BUTTON_BACK, false);
    sent_n = 0;

    /* THE VIRTUAL MOUSE STOPS WHEN THE STICK IS LET GO, even when the stick springs back to the middle in one event
     * (user, SHIELD, 2026-10-21: the pointer kept moving after the stick was let go). */
    input.stick_deadzone = 7;
    pad2.instance_id = 2;
    vmouse_active = true;
    vm_vec_calls = vm_trig_calls = 0;
    axis(0, SDL_CONTROLLER_AXIS_RIGHTX, 20000);
    check("a pushed stick moves the virtual mouse", vm_vec_calls == 1 && vm_x == 20000);
    axis(0, SDL_CONTROLLER_AXIS_RIGHTX, 0);
    check("let go in one event, the virtual mouse is told to stop", vm_vec_calls == 2 && vm_x == 0 && vm_y == 0);

    /* Another controller's idle stick (jitter inside the deadzone) does not stop the pointer the first one moves. */
    axis(0, SDL_CONTROLLER_AXIS_RIGHTX, 20000);
    const int calls = vm_vec_calls;
    axis(2, SDL_CONTROLLER_AXIS_RIGHTX, 300);
    check("another controller's idle stick leaves it moving", vm_vec_calls == calls && vm_x == 20000);
    axis(0, SDL_CONTROLLER_AXIS_RIGHTX, 0);
    check("and the first one letting go still stops it", vm_x == 0);

    /* A trigger that snaps up lets the mouse button go. */
    axis(0, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
    const int trig_calls = vm_trig_calls;
    check("a pulled trigger reaches the virtual mouse", vm_r != 0);
    axis(0, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 0);
    check("a trigger snapping up lets the button go", vm_trig_calls == trig_calls + 1 && vm_r == 0 && vm_l == 0);

    /* With the virtual mouse off, nothing reaches it. */
    vmouse_active = false;
    vm_vec_calls = 0;
    axis(0, SDL_CONTROLLER_AXIS_RIGHTX, 20000);
    axis(0, SDL_CONTROLLER_AXIS_RIGHTX, 0);
    check("virtual mouse off: the stick is the controller's own", vm_vec_calls == 0);

    /* ── THE START WHEEL (start_wheel.h): hold Start, push a direction, let go ── */
    settings.controller_shortcuts = CONTROLLER_SHORTCUTS_ALL;
    input.session = &session;
    pad.buttons = 0;
    pad.leftStickX = pad.leftStickY = pad.rightStickX = pad.rightStickY = 0;
    sent_n = 0;

    /* A tap is Start, as before: no wheel. */
    button(SDL_CONTROLLER_BUTTON_START, true);
    check("Start arms the wheel's hold timer", wheel.hold_timer != NULL);
    button(SDL_CONTROLLER_BUTTON_START, false);
    check("a tap disarms it, no wheel", wheel.hold_timer == NULL && wheel_picture_code == -1);
    expect_sent("a tapped Start reaches the host", (const short[]) {PLAY_FLAG, 0}, 2);

    /* Held: the wheel shows. The stick right takes the controller (the host sees it at rest, once) and highlights
     * Mouse; Start let go chooses; the stick back runs it. */
    vmouse_toggles = 0;
    button(SDL_CONTROLLER_BUTTON_START, true);
    fire((void **) &wheel.hold_timer);
    check("held, the wheel shows", wheel_picture_code == 1 + START_WHEEL_CONTINUE);
    axis(0, SDL_CONTROLLER_AXIS_RIGHTX, 32767);
    axis(0, SDL_CONTROLLER_AXIS_RIGHTX, 32767);
    check("the stick right highlights Mouse", wheel_picture_code == 1 + START_WHEEL_MOUSE);
    button(SDL_CONTROLLER_BUTTON_START, false);
    check("Start let go closes the wheel", wheel_picture_code == 0);
    check("the choice waits for the stick to come back", vmouse_toggles == 0);
    axis(0, SDL_CONTROLLER_AXIS_RIGHTX, 0);
    check("the stick back: the virtual mouse toggles", vmouse_toggles == 1);
    expect_sent("the host saw Start, then the controller at rest, and nothing of the wheel",
                (const short[]) {PLAY_FLAG, 0}, 2);

    /* A combo wins: View + Start forming cancels the hold, and the stream menu combo still opens the menu. */
    overlay_opened = 0;
    button(SDL_CONTROLLER_BUTTON_START, true);
    button(SDL_CONTROLLER_BUTTON_BACK, true);
    check("View + Start cancels the wheel's hold", wheel.hold_timer == NULL);
    button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, true);
    button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, true);
    button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, false);
    button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, false);
    button(SDL_CONTROLLER_BUTTON_BACK, false);
    button(SDL_CONTROLLER_BUTTON_START, false);
    check("with both on, the stream menu combo still opens the menu", overlay_opened == 1);

    /* The setting: "Start wheel only" turns the combos off, "Button combos only" and "Off" the wheel. */
    settings.controller_shortcuts = CONTROLLER_SHORTCUTS_WHEEL;
    overlay_opened = 0;
    button(SDL_CONTROLLER_BUTTON_START, true);
    button(SDL_CONTROLLER_BUTTON_BACK, true);
    button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, true);
    button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, true);
    button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, false);
    button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, false);
    button(SDL_CONTROLLER_BUTTON_BACK, false);
    button(SDL_CONTROLLER_BUTTON_START, false);
    check("Start wheel only: the stream menu combo does nothing", overlay_opened == 0);
    settings.controller_shortcuts = CONTROLLER_SHORTCUTS_COMBOS;
    button(SDL_CONTROLLER_BUTTON_START, true);
    check("Button combos only: Start arms no wheel", wheel.hold_timer == NULL);
    button(SDL_CONTROLLER_BUTTON_START, false);
    settings.controller_shortcuts = CONTROLLER_SHORTCUTS_NONE;
    button(SDL_CONTROLLER_BUTTON_START, true);
    check("Off: Start arms no wheel", wheel.hold_timer == NULL);
    button(SDL_CONTROLLER_BUTTON_START, false);
    sent_n = 0;

    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

/*
 * The Android TV remote during a stream, as webOS's remote behaves (stream/input/session_keyboard.c and
 * platform/android/keyboard_android.c, compiled here with TARGET_ANDROID): Back tapped is Escape on the host and
 * held opens the stream menu; OK tapped is Enter and held a right click; the colour, channel, info/guide and menu
 * keys mean what they mean on webOS. What leaves the input code -- keys to the host, pointer buttons, UI events --
 * is recorded by fakes.
 */
#include KEYBOARD_SOURCE
#include SESSION_KEYBOARD_SOURCE

#include <stdio.h>

/* --- recorded outputs ---------------------------------------------------------------------------------------- */
static char log_buf[2048];

static void note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const size_t used = strlen(log_buf);
    vsnprintf(log_buf + used, sizeof(log_buf) - used, fmt, ap);
    va_end(ap);
}

int LiSendKeyboardEvent(short keyCode, char keyAction, char modifiers) {
    (void) modifiers;
    note("key %s %d;", keyAction == KEY_ACTION_DOWN ? "down" : "up", keyCode & 0x7fff);
    return 0;
}
int LiSendKeyboardEvent2(short keyCode, char keyAction, char modifiers, char flags) {
    (void) flags;
    return LiSendKeyboardEvent(keyCode, keyAction, modifiers);
}
int LiSendUtf8TextEvent(const char *text, unsigned int length) {
    (void) text;
    (void) length;
    return 0;
}
void coop_input_send_keyboard(short key_code, char action, char modifiers) {
    (void) modifiers;
    note("peer-key %s %d;", action == KEY_ACTION_DOWN ? "down" : "up", key_code & 0x7fff);
}
void coop_pointer_button(int action, int button) {
    note("button %s %d;", action == BUTTON_ACTION_PRESS ? "press" : "release", button);
}
bool coop_pointer_keys_to_peer(void) { return false; }
static bool soft_keyboard_shown;
bool bus_pushevent(int which, void *data1, void *data2) {
    (void) data1;
    (void) data2;
    if (which == USER_CLOSE_SOFT_KEYBOARD) {
        soft_keyboard_shown = false;   /* as the streaming screen does */
    }
    note("ui %s;", which == USER_OPEN_OVERLAY ? "overlay" : which == USER_OPEN_SOFT_KEYBOARD ? "soft-keyboard" :
                   which == USER_CLOSE_SOFT_KEYBOARD ? "close-soft-keyboard" : "other");
    return true;
}
bool streaming_soft_keyboard_shown(void) { return soft_keyboard_shown; }
bool ui_should_block_input(void) { return false; }

PCONFIGURATION app_configuration;
app_t *global;

/* Desktop key combos (Ctrl+Alt+Shift+...): reachable from the code under test, not exercised by it. */
void app_input_set_capture(app_input_t *input, bool captured) { (void) input; (void) captured; }
bool app_input_toggle_mouse_mode(app_input_t *input) { (void) input; return false; }
bool app_input_toggle_local_cursor(app_input_t *input) { (void) input; return false; }
bool app_input_toggle_pointer_lock(app_input_t *input) { (void) input; return false; }
bool app_set_fullscreen(app_t *app, bool fullscreen) { (void) app; (void) fullscreen; return false; }
bool coop_pointer_active(void) { return false; }
void session_interrupt(session_t *session, bool quitapp, streaming_interrupt_reason_t reason) {
    (void) session;
    (void) quitapp;
    (void) reason;
    note("interrupt;");
}

/* --- the tests ---------------------------------------------------------------------------------------------- */
static int failures;

static void key(stream_input_t *input, SDL_Scancode scancode, bool pressed, uint32_t at) {
    SDL_KeyboardEvent e = {.type = pressed ? SDL_KEYDOWN : SDL_KEYUP, .timestamp = at,
            .state = pressed ? SDL_PRESSED : SDL_RELEASED};
    e.keysym.scancode = scancode;
    e.keysym.sym = SDL_GetKeyFromScancode(scancode);
    stream_input_handle_key(input, &e);
}

static void press(stream_input_t *input, SDL_Scancode scancode, uint32_t held_ms) {
    log_buf[0] = '\0';
    key(input, scancode, true, 1000);
    key(input, scancode, false, 1000 + held_ms);
}

static void expect(const char *what, const char *wanted) {
    if (strcmp(log_buf, wanted) != 0) {
        printf("FAIL: %s\n    wanted: %s\n    got:    %s\n", what, wanted, log_buf);
        failures++;
    }
}

int main(void) {
    static CONFIGURATION config;
    app_configuration = &config;
    static session_t session;
    stream_input_t input = {.session = &session};

    press(&input, SDL_SCANCODE_AC_BACK, 120);
    expect("Back tapped: Escape on the host", "key down 27;key up 27;");
    press(&input, SDL_SCANCODE_AC_BACK, 700);
    expect("Back held: the stream menu", "ui overlay;");
    soft_keyboard_shown = true;
    press(&input, SDL_SCANCODE_AC_BACK, 120);
    expect("Back with the on-screen keyboard up: closes it, and nothing else", "ui close-soft-keyboard;");
    soft_keyboard_shown = false;

    press(&input, SDL_SCANCODE_SELECT, 120);
    expect("OK tapped: Enter on the host", "key down 13;key up 13;");
    press(&input, SDL_SCANCODE_SELECT, 700);
    expect("OK held: a right click", "button press 3;button release 3;");
    press(&input, SDL_SCANCODE_RETURN, 700);
    expect("a keyboard's Enter, even held: plain Enter", "key down 13;key up 13;");

    press(&input, (SDL_Scancode) ANDROID_SCANCODE_RED, 50);
    expect("red: the stream menu", "ui overlay;");
    press(&input, (SDL_Scancode) ANDROID_SCANCODE_OVERLAY, 50);
    expect("info/guide: the stream menu", "ui overlay;");
    press(&input, (SDL_Scancode) ANDROID_SCANCODE_MENU, 50);
    expect("the remote's menu key: the stream menu", "ui overlay;");
    press(&input, (SDL_Scancode) ANDROID_SCANCODE_BLUE, 50);
    expect("blue: the on-screen keyboard", "ui soft-keyboard;");
    press(&input, (SDL_Scancode) ANDROID_SCANCODE_GREEN, 50);
    expect("green: nothing", "");
    press(&input, (SDL_Scancode) ANDROID_SCANCODE_YELLOW, 50);
    expect("yellow: nothing", "");
    press(&input, (SDL_Scancode) ANDROID_SCANCODE_CH_UP, 50);
    expect("channel up: Page Up on the host", "key down 33;key up 33;");
    press(&input, (SDL_Scancode) ANDROID_SCANCODE_CH_DOWN, 50);
    expect("channel down: Page Down on the host", "key down 34;key up 34;");

    input.view_only = true;
    press(&input, SDL_SCANCODE_SELECT, 120);
    expect("view only: OK sends nothing", "");
    press(&input, SDL_SCANCODE_AC_BACK, 120);
    expect("view only: Back sends nothing", "");
    press(&input, SDL_SCANCODE_AC_BACK, 700);
    expect("view only: Back held still opens the stream menu", "ui overlay;");
    press(&input, (SDL_Scancode) ANDROID_SCANCODE_BLUE, 50);
    expect("view only: no on-screen keyboard", "");

    if (failures == 0) printf("Android remote keys: OK\n");
    return failures == 0 ? 0 : 1;
}

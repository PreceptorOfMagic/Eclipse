#include "app.h"
#include "logging.h"
#include "stream/input/coop_input.h"
#include "stream/input/coop_pointer.h"
#include "stream/input/session_input.h"
#include "stream/session.h"

#include <Limelight.h>
#include <SDL.h>

#include "util/bus.h"
#include "util/user_event.h"

#include "vk.h"

enum KeyCombo {
    KeyComboQuit,
    KeyComboUngrabInput,
    KeyComboToggleFullScreen,
    KeyComboToggleStatsOverlay,
    KeyComboToggleMouseMode,
    KeyComboToggleCursorHide,
    KeyComboToggleMinimize,
    KeyComboToggleMouseLock,
    KeyComboPasteText,
    KeyComboMax
};

#if defined(TARGET_WEBOS)
#define DESKTOP_COMBO false
#else
#define DESKTOP_COMBO true
#endif

struct SpecialKeyCombo {
    enum KeyCombo keyCombo;
    SDL_Keycode keyCode;
    SDL_Scancode scanCode;
    bool enabled;
};

static struct SpecialKeyCombo m_SpecialKeyCombos[KeyComboMax] = {
        {KeyComboQuit,               SDLK_q, SDL_SCANCODE_Q, true},
        {KeyComboUngrabInput,        SDLK_z, SDL_SCANCODE_Z, true},
        {KeyComboToggleFullScreen,   SDLK_x, SDL_SCANCODE_X, true},
        {KeyComboToggleStatsOverlay, SDLK_s, SDL_SCANCODE_S, true},
        {KeyComboToggleMouseMode,    SDLK_m, SDL_SCANCODE_M, true},
        {KeyComboToggleCursorHide,   SDLK_c, SDL_SCANCODE_C, true},
        {KeyComboToggleMinimize,     SDLK_d, SDL_SCANCODE_D, true},
        /* The rest of moonlight-qt's set. Desktop only: on the TV these keys still reach the host. */
        {KeyComboToggleMouseLock,    SDLK_l, SDL_SCANCODE_L, DESKTOP_COMBO},
        {KeyComboPasteText,          SDLK_v, SDL_SCANCODE_V, DESKTOP_COMBO},
};

enum KeyCombo _pending_key_combo = KeyComboMax;

struct KeysDown {
    short keyCode;
    /** Went to the co-op PEER's host: its release must follow, wherever the pointer is by then. */
    bool peer;
    struct KeysDown *prev;
    struct KeysDown *next;
};

static struct KeysDown *_pressed_keys;

#define LINKEDLIST_IMPL

// Linked list functions for KeysDown
#define LINKEDLIST_TYPE struct KeysDown
#define LINKEDLIST_PREFIX keys
#define LINKEDLIST_DOUBLE 1

#include "linked_list.h"

/* KEYS FOLLOW THE POINTER'S PANE in co-op, as the mouse already does (coop_pointer.c): the user
 * wants keyboard and mouse "active on whichever pc the mouse is hovered over". A press goes to
 * the pane under the pointer; its release goes where the press went. Outside co-op, or with no
 * peer attached, every key goes to this process's host exactly as before. A key for THIS PC as
 * a local host is still dropped by LiSetLocalInputBlocked -- that needs the focus hand-off. */
static void send_key(short key_code, char action, char modifiers, bool to_peer) {
    if (to_peer) {
        coop_input_send_keyboard(key_code, action, modifiers);
    } else {
        LiSendKeyboardEvent(key_code, action, modifiers);
    }
}

#undef LINKEDLIST_TYPE
#undef LINKEDLIST_PREFIX
#undef LINKEDLIST_DOUBLE

static int keydown_count = 0;

#define REMOTE_OK_RIGHT_CLICK_HOLD_MS 600

#if TARGET_WEBOS

bool stream_input_webos_intercept_remote_keys(stream_input_t *input, const SDL_KeyboardEvent *event, short *keyCode);

#endif

static int keys_code_comparator(struct KeysDown *p, const void *fv) {
    return p->keyCode - *((short *) fv);
}

static bool isSystemKeyCaptureActive() {
    return app_configuration->syskey_capture;
}

#if TARGET_WEBOS

static bool is_webos_remote_ok_key(const SDL_KeyboardEvent *event) {
    return event->keysym.sym == SDLK_RETURN || event->keysym.sym == SDLK_KP_ENTER ||
           event->keysym.scancode == SDL_SCANCODE_RETURN || event->keysym.scancode == SDL_SCANCODE_KP_ENTER;
}

static bool stream_input_handle_remote_ok_long_press(stream_input_t *input, const SDL_KeyboardEvent *event,
                                                     char modifiers) {
    if (!is_webos_remote_ok_key(event)) {
        return false;
    }
    if (event->repeat) {
        return true;
    }
    if (event->state == SDL_PRESSED) {
        input->remoteOkPressed = true;
        input->remoteOkPressedAt = event->timestamp != 0 ? event->timestamp : SDL_GetTicks();
        input->remoteOkModifiers = modifiers;
        return true;
    }
    if (event->state != SDL_RELEASED || !input->remoteOkPressed) {
        return false;
    }

    uint32_t releasedAt = event->timestamp != 0 ? event->timestamp : SDL_GetTicks();
    uint32_t heldMs = releasedAt - input->remoteOkPressedAt;
    char downModifiers = input->remoteOkModifiers;
    input->remoteOkPressed = false;
    input->remoteOkPressedAt = 0;
    input->remoteOkModifiers = 0;

    if (input->view_only) {
        return true;
    }
    if (heldMs >= REMOTE_OK_RIGHT_CLICK_HOLD_MS) {
        /* Fires wherever the cursor is, so in co-op it must land on the PC the pointer
         * is currently over -- not always the primary. */
        coop_pointer_button(BUTTON_ACTION_PRESS, BUTTON_RIGHT);
        coop_pointer_button(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
    } else {
        const bool to_peer = coop_pointer_keys_to_peer();
        send_key(0x8000 | VK_RETURN, KEY_ACTION_DOWN, downModifiers, to_peer);
        send_key(0x8000 | VK_RETURN, KEY_ACTION_UP, downModifiers, to_peer);
    }
    return true;
}

#endif

#if !defined(TARGET_WEBOS)
/* Longest clipboard paste typed on the host; the rest is dropped (and logged) rather than flooding it. */
#define PASTE_MAX_BYTES 8192

/* Ctrl+Alt+Shift+V (moonlight-qt): type the client's clipboard on the host. One character per text event --
 * LiSendUtf8TextEvent refuses more than UTF8_TEXT_EVENT_MAX_COUNT (32) bytes -- and line breaks and tabs as
 * the keys themselves. The co-op input channel carries no text, so while the keyboard is on the PEER's pane
 * the paste is refused rather than typed into the wrong PC. */
static void paste_clipboard(stream_input_t *input) {
    if (input->view_only) {
        return;
    }
    if (coop_pointer_active() && coop_pointer_keys_to_peer()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Paste ignored: the keyboard is on the co-op peer's pane, which has no text channel");
        return;
    }
    char *text = SDL_HasClipboardText() ? SDL_GetClipboardText() : NULL;
    if (text == NULL) {
        return;
    }
    size_t len = strlen(text), typed = 0;
    if (len > PASTE_MAX_BYTES) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Paste truncated from %u to %u bytes",
                    (unsigned) len, (unsigned) PASTE_MAX_BYTES);
        len = PASTE_MAX_BYTES;
    }
    for (size_t i = 0; i < len;) {
        const unsigned char c = (unsigned char) text[i];
        const size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (n == 0 || i + n > len) {
            i++;    /* a stray continuation byte, or a character cut off by the limit */
            continue;
        }
        if (c == '\r' || c == '\n' || c == '\t') {
            if (c == '\r' && i + 1 < len && text[i + 1] == '\n') {
                i++;    /* CRLF is one Enter */
            }
            const short vk = c == '\t' ? VK_TAB : VK_RETURN;
            send_key(0x8000 | vk, KEY_ACTION_DOWN, 0, false);
            send_key(0x8000 | vk, KEY_ACTION_UP, 0, false);
            typed++;
        } else if (c >= 0x20) {
            LiSendUtf8TextEvent(&text[i], (unsigned int) n);
            typed++;
        }
        i += n;
    }
    SDL_free(text);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Pasted %u characters", (unsigned) typed);
}
#endif

void performPendingSpecialKeyCombo(stream_input_t *input) {
    // The caller must ensure all keys are up
    SDL_assert_release(keys_len(_pressed_keys) == 0);

    switch (_pending_key_combo) {
        case KeyComboQuit:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Detected quit key combo");
            session_interrupt(input->session, false, STREAMING_INTERRUPT_USER);
            break;

        case KeyComboUngrabInput:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Detected mouse capture toggle combo");
#if !defined(TARGET_WEBOS)
            if (coop_pointer_active()) {
                /* Co-op has its own capture model: the pointer follows the pane it is over, and when this PC
                 * is a host the same combo is the global hotkey that brings the mouse back (local_input_win.c). */
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Mouse capture toggle ignored during co-op");
                break;
            }
            app_input_set_capture(input->input, input->input->capture_released);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Mouse %s",
                        input->input->capture_released ? "released to the desktop" : "captured by the stream");
#endif
            break;

        case KeyComboToggleFullScreen:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Detected full-screen toggle combo");
#if !defined(TARGET_WEBOS)
            /* This stream only: app_set_fullscreen() leaves the saved "Fullscreen" setting alone unless the
             * switch fails. The video letterboxes into the new size and the pointer follows it
             * (app_event_filter, SIZE_CHANGED -> streaming_display_size). */
            app_set_fullscreen(global, (SDL_GetWindowFlags(global->ui.window) &
                                        (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) == 0);
#endif
            break;

        case KeyComboToggleStatsOverlay:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Detected stats toggle combo");
            bus_pushevent(USER_OPEN_OVERLAY, NULL, NULL);
            break;

        case KeyComboToggleMouseMode:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Detected mouse mode toggle combo");
#if !defined(TARGET_WEBOS)
            if (coop_pointer_active()) {
                /* Relative motion goes to the primary host only (stream_input_handle_mmotion): in co-op the
                 * absolute position is what says which PC the pointer is over. */
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Mouse mode toggle ignored during co-op");
                break;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Mouse mode now %s",
                        app_input_toggle_mouse_mode(input->input) ? "relative (games)" : "absolute (remote desktop)");
#endif
            break;
        case KeyComboToggleCursorHide:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Detected show mouse combo");
#if !defined(TARGET_WEBOS)
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Local cursor %s",
                        app_input_toggle_local_cursor(input->input) ? "shown" : "hidden");
#endif
            break;
        case KeyComboToggleMinimize:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Detected minimize combo");
#if !defined(TARGET_WEBOS)
            /* The stream keeps running, as in moonlight-qt: losing focus only releases the mouse
             * (app_event_filter, FOCUS_LOST). */
            SDL_MinimizeWindow(global->ui.window);
#endif
            break;
        case KeyComboToggleMouseLock:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Detected mouse lock toggle combo");
#if !defined(TARGET_WEBOS)
            if (coop_pointer_active()) {
                /* When this PC is a co-op host the real cursor leaves the window for its pane
                 * (local_input_win.c); confining it to the window would strand the mouse. */
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Mouse lock toggle ignored during co-op");
                break;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Mouse %s",
                        app_input_toggle_pointer_lock(input->input) ? "locked to the window" : "free to leave the window");
#endif
            break;
        case KeyComboPasteText:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Detected paste text combo");
#if !defined(TARGET_WEBOS)
            paste_clipboard(input);
#endif
            break;
        default:
            break;
    }

    // Reset pending key combo
    _pending_key_combo = KeyComboMax;
}

void stream_input_handle_key(stream_input_t *input, const SDL_KeyboardEvent *event) {
    short keyCode = 0;
#if TARGET_WEBOS
    if (stream_input_webos_intercept_remote_keys(input, event, &keyCode)) {
        return;
    }
#endif
    char modifiers;

    // Check for our special key combos
    if ((event->state == SDL_PRESSED) &&
        (event->keysym.mod & KMOD_CTRL) &&
        (event->keysym.mod & KMOD_ALT) &&
        (event->keysym.mod & KMOD_SHIFT)) {
        // First we test the SDLK combos for matches,
        // that way we ensure that latin keyboard users
        // can match to the key they see on their keyboards.
        // If nothing matches that, we'll then go on to
        // checking scancodes so non-latin keyboard users
        // can have working hotkeys (though possibly in
        // odd positions). We must do all SDLK tests before
        // any scancode tests to avoid issues in cases
        // where the SDLK for one shortcut collides with
        // the scancode of another.

        if (_pending_key_combo == KeyComboMax) {
            for (int i = 0; i < KeyComboMax; i++) {
                if (m_SpecialKeyCombos[i].enabled && event->keysym.sym == m_SpecialKeyCombos[i].keyCode) {
                    _pending_key_combo = m_SpecialKeyCombos[i].keyCombo;
                    break;
                }
            }
        }

        if (_pending_key_combo == KeyComboMax) {
            for (int i = 0; i < KeyComboMax; i++) {
                if (m_SpecialKeyCombos[i].enabled && event->keysym.scancode == m_SpecialKeyCombos[i].scanCode) {
                    _pending_key_combo = m_SpecialKeyCombos[i].keyCombo;
                    break;
                }
            }
        }
    }

    if (event->state == SDL_PRESSED && _pending_key_combo != KeyComboMax) {
        // Ignore further key presses until the special combo is raised
        return;
    }

    if (event->repeat) {
        // Ignore repeat key down events
        SDL_assert_release(event->state == SDL_PRESSED);
        return;
    }

    // Set modifier flags
    modifiers = 0;
    if (event->keysym.mod & KMOD_CTRL) {
        modifiers |= MODIFIER_CTRL;
    }
    if (event->keysym.mod & KMOD_ALT) {
        modifiers |= MODIFIER_ALT;
    }
    if (event->keysym.mod & KMOD_SHIFT) {
        modifiers |= MODIFIER_SHIFT;
    }
    if (event->keysym.mod & KMOD_GUI) {
        if (isSystemKeyCaptureActive()) {
            modifiers |= MODIFIER_META;
        }
    }

#if TARGET_WEBOS
    if (stream_input_handle_remote_ok_long_press(input, event, modifiers)) {
        return;
    }
#endif

    // Set keycode. We explicitly use scancode here because GFE will try to correct
    // for AZERTY layouts on the host, but it depends on receiving VK_ values matching
    // a QWERTY layout to work.
    if (event->keysym.scancode >= SDL_SCANCODE_1 && event->keysym.scancode <= SDL_SCANCODE_9) {
        // SDL defines SDL_SCANCODE_0 > SDL_SCANCODE_9, so we need to handle that manually
        keyCode = (event->keysym.scancode - SDL_SCANCODE_1) + VK_0 + 1;
    } else if (event->keysym.scancode >= SDL_SCANCODE_A && event->keysym.scancode <= SDL_SCANCODE_Z) {
        keyCode = (event->keysym.scancode - SDL_SCANCODE_A) + VK_A;
    } else if (event->keysym.scancode >= SDL_SCANCODE_F1 && event->keysym.scancode <= SDL_SCANCODE_F12) {
        keyCode = (event->keysym.scancode - SDL_SCANCODE_F1) + VK_F1;
    } else if (event->keysym.scancode >= SDL_SCANCODE_F13 && event->keysym.scancode <= SDL_SCANCODE_F24) {
        keyCode = (event->keysym.scancode - SDL_SCANCODE_F13) + VK_F13;
    } else if (event->keysym.scancode >= SDL_SCANCODE_KP_1 && event->keysym.scancode <= SDL_SCANCODE_KP_9) {
        // SDL defines SDL_SCANCODE_KP_0 > SDL_SCANCODE_KP_9, so we need to handle that manually
        keyCode = (event->keysym.scancode - SDL_SCANCODE_KP_1) + VK_NUMPAD0 + 1;
    } else {
        switch (event->keysym.scancode) {
            case SDL_SCANCODE_BACKSPACE:
                keyCode = VK_BACK;
                break;
            case SDL_SCANCODE_TAB:
                keyCode = VK_TAB;
                break;
            case SDL_SCANCODE_CLEAR:
                keyCode = VK_CLEAR;
                break;
            case SDL_SCANCODE_KP_ENTER: // FIXME: Is this correct?
            case SDL_SCANCODE_RETURN:
                keyCode = VK_RETURN;
                break;
            case SDL_SCANCODE_PAUSE:
                keyCode = VK_PAUSE;
                break;
            case SDL_SCANCODE_CAPSLOCK:
                keyCode = VK_CAPITAL;
                break;
            case SDL_SCANCODE_ESCAPE:
                keyCode = VK_ESCAPE;
                break;
            case SDL_SCANCODE_SPACE:
                keyCode = VK_SPACE;
                break;
            case SDL_SCANCODE_PAGEUP:
                keyCode = VK_PRIOR;
                break;
            case SDL_SCANCODE_PAGEDOWN:
                keyCode = VK_NEXT;
                break;
            case SDL_SCANCODE_END:
                keyCode = VK_END;
                break;
            case SDL_SCANCODE_HOME:
                keyCode = VK_HOME;
                break;
            case SDL_SCANCODE_LEFT:
                keyCode = VK_LEFT;
                break;
            case SDL_SCANCODE_UP:
                keyCode = VK_UP;
                break;
            case SDL_SCANCODE_RIGHT:
                keyCode = VK_RIGHT;
                break;
            case SDL_SCANCODE_DOWN:
                keyCode = VK_DOWN;
                break;
            case SDL_SCANCODE_SELECT:
                keyCode = VK_SELECT;
                break;
            case SDL_SCANCODE_EXECUTE:
                keyCode = VK_EXECUTE;
                break;
            case SDL_SCANCODE_PRINTSCREEN:
                keyCode = VK_SNAPSHOT;
                break;
            case SDL_SCANCODE_INSERT:
                keyCode = VK_INSERT;
                break;
            case SDL_SCANCODE_DELETE:
                keyCode = VK_DELETE;
                break;
            case SDL_SCANCODE_HELP:
                keyCode = VK_HELP;
                break;
            case SDL_SCANCODE_KP_0:
                // See comment above about why we only handle SDL_SCANCODE_KP_0 here
                keyCode = VK_NUMPAD0;
                break;
            case SDL_SCANCODE_0:
                // See comment above about why we only handle SDL_SCANCODE_0 here
                keyCode = VK_0;
                break;
            case SDL_SCANCODE_KP_MULTIPLY:
                keyCode = VK_MULTIPLY;
                break;
            case SDL_SCANCODE_KP_PLUS:
                keyCode = VK_ADD;
                break;
            case SDL_SCANCODE_KP_COMMA:
                keyCode = VK_SEPARATOR;
                break;
            case SDL_SCANCODE_KP_MINUS:
                keyCode = VK_SUBTRACT;
                break;
            case SDL_SCANCODE_KP_PERIOD:
                keyCode = VK_DECIMAL;
                break;
            case SDL_SCANCODE_KP_DIVIDE:
                keyCode = VK_DIVIDE;
                break;
            case SDL_SCANCODE_NUMLOCKCLEAR:
                keyCode = VK_NUMLOCK;
                break;
            case SDL_SCANCODE_SCROLLLOCK:
                keyCode = VK_SCROLL;
                break;
            case SDL_SCANCODE_LSHIFT:
                keyCode = VK_LSHIFT;
                break;
            case SDL_SCANCODE_RSHIFT:
                keyCode = VK_RSHIFT;
                break;
            case SDL_SCANCODE_LCTRL:
                keyCode = VK_LCONTROL;
                break;
            case SDL_SCANCODE_RCTRL:
                keyCode = VK_RCONTROL;
                break;
            case SDL_SCANCODE_LALT:
                keyCode = VK_LMENU;
                break;
            case SDL_SCANCODE_RALT:
                keyCode = VK_RMENU;
                break;
            case SDL_SCANCODE_LGUI:
                if (!isSystemKeyCaptureActive()) {
                    return;
                }
                keyCode = VK_LWIN;
                break;
            case SDL_SCANCODE_RGUI:
                if (!isSystemKeyCaptureActive()) {
                    return;
                }
                keyCode = VK_RWIN;
                break;
            case SDL_SCANCODE_AC_BACK:
                keyCode = VK_BROWSER_BACK;
                break;
            case SDL_SCANCODE_AC_FORWARD:
                keyCode = VK_BROWSER_FORWARD;
                break;
            case SDL_SCANCODE_AC_REFRESH:
                keyCode = VK_BROWSER_REFRESH;
                break;
            case SDL_SCANCODE_AC_STOP:
                keyCode = VK_BROWSER_STOP;
                break;
            case SDL_SCANCODE_AC_SEARCH:
                keyCode = VK_BROWSER_SEARCH;
                break;
            case SDL_SCANCODE_AC_BOOKMARKS:
                keyCode = VK_BROWSER_FAVORITES;
                break;
            case SDL_SCANCODE_AC_HOME:
                keyCode = VK_BROWSER_HOME;
                break;
            case SDL_SCANCODE_SEMICOLON:
                keyCode = VK_OEM_1;
                break;
            case SDL_SCANCODE_EQUALS:
                keyCode = 0xBB;
                break;
            case SDL_SCANCODE_COMMA:
                keyCode = VK_OEM_COMMA;
                break;
            case SDL_SCANCODE_MINUS:
                keyCode = VK_OEM_MINUS;
                break;
            case SDL_SCANCODE_PERIOD:
                keyCode = VK_OEM_PERIOD;
                break;
            case SDL_SCANCODE_SLASH:
                keyCode = VK_OEM_2;
                break;
            case SDL_SCANCODE_GRAVE:
                keyCode = VK_OEM_3;
                break;
            case SDL_SCANCODE_LEFTBRACKET:
                keyCode = VK_OEM_4;
                break;
            case SDL_SCANCODE_BACKSLASH:
                keyCode = VK_OEM_5;
                break;
            case SDL_SCANCODE_RIGHTBRACKET:
                keyCode = VK_OEM_6;
                break;
            case SDL_SCANCODE_APOSTROPHE:
                keyCode = VK_OEM_7;
                break;
            case SDL_SCANCODE_NONUSBACKSLASH:
                keyCode = VK_OEM_102;
                break;
            default:
                if (!keyCode) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Unhandled button event: scancode: %d, keycode: %d",
                                event->keysym.scancode, event->keysym.sym);
                    return;
                }
        }
    }

    // Track the key state, so we always know which keys are down
    bool to_peer = false;
    if (event->state == SDL_PRESSED) {
        struct KeysDown *node = keys_new();
        node->keyCode = keyCode;
        node->peer = to_peer = coop_pointer_keys_to_peer();
        _pressed_keys = keys_append(_pressed_keys, node);
    } else {
        struct KeysDown *node = keys_find_by(_pressed_keys, &keyCode, &keys_code_comparator);
        if (node) {
            to_peer = node->peer;
            _pressed_keys = keys_remove(_pressed_keys, node);
            free(node);
        }
    }

    if (!input->view_only) {
        if (event->state == SDL_PRESSED) {
            keydown_count++;
        } else if (event->state == SDL_RELEASED) {
            keydown_count--;
        }
        send_key(0x8000 | keyCode, event->state == SDL_PRESSED ? KEY_ACTION_DOWN : KEY_ACTION_UP,
                 modifiers, to_peer);
    }

    if (_pending_key_combo != KeyComboMax && keys_len(_pressed_keys) == 0) {
        int keys;
        const Uint8 *keyState = SDL_GetKeyboardState(&keys);

        // Make sure all client keys are up before we process the special key combo
        for (int i = 0; i < keys; i++) {
            if (keyState[i] == SDL_PRESSED) {
                return;
            }
        }

        // If we made it this far, no keys are pressed
        performPendingSpecialKeyCombo(input);
    }
}

void stream_input_handle_text(stream_input_t *input, const SDL_TextInputEvent *event) {
    if (keydown_count) {
        commons_log_verbose("Input", "Ignoring duplicated text input %s. Pressed keys: %d", event->text, keydown_count);
        return;
    }
    size_t len = strlen(event->text);
    if (!len) {
        return;
    }
    LiSendUtf8TextEvent(event->text, len);
}

void stream_input_send_key_event(stream_input_t *input, short keyCode, bool keyDown, char modifiers) {
    if (input->view_only) {
        return;
    }
    bool to_peer = false;
    if (keyDown) {
        struct KeysDown *node = keys_new();
        node->keyCode = keyCode;
        node->peer = to_peer = coop_pointer_keys_to_peer();
        _pressed_keys = keys_append(_pressed_keys, node);
        keydown_count++;
    } else {
        struct KeysDown *node = keys_find_by(_pressed_keys, &keyCode, &keys_code_comparator);
        if (node) {
            to_peer = node->peer;
            _pressed_keys = keys_remove(_pressed_keys, node);
            free(node);
            keydown_count--;
        }
    }
    send_key(0x8000 | keyCode, keyDown ? KEY_ACTION_DOWN : KEY_ACTION_UP, modifiers, to_peer);
}

void stream_input_flush_pressed_keys(stream_input_t *input) {
    if (input->view_only) {
        return;
    }
    while (_pressed_keys) {
        struct KeysDown *n = _pressed_keys;
        short kc = n->keyCode;
        send_key(0x8000 | kc, KEY_ACTION_UP, 0, n->peer);
        _pressed_keys = keys_remove(_pressed_keys, n);
        free(n);
    }
    keydown_count = 0;
    _pending_key_combo = KeyComboMax;
}
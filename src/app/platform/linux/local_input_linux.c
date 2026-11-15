/*
 * HOST-AND-CLIENT CO-OP ON LINUX: the mouse and keyboard (the Windows side is platform/windows/local_input_win.c).
 * Measured on VEGA 2026-11-14 (docs/design-host-and-client-linux.md, D/E):
 *
 *   INTO THIS PC'S HALF   the pointer over that half of the client window -> Mutter RemoteDesktop puts the real pointer
 *                         on the same spot of the virtual output (local_pane_linux_pointer_to_pane). From there the real
 *                         mouse moves the real pointer and this PC's game gets everything natively.
 *   INTO THE STREAM'S     the pointer leaves the virtual output by its outer edge (the one facing the stream's half;
 *   HALF                  local_pane_linux.c put it there) -> an InputCapture barrier fires -> the compositor hands
 *                         EVERY mouse and keyboard event to the client over libei while this PC's game KEEPS the focus
 *                         (so its controllers keep working). Those events are pushed into SDL's queue as if they had
 *                         come from the window, so the existing co-op pointer split, the stream's input and the
 *                         client's shortcuts (Ctrl+Alt+Shift+Q/Z/S) all work unchanged. Crossing back over the divider
 *                         reaches local_input_hand_off, which releases the capture at the matching point.
 *   WALL                  the pointer leaving the virtual output by any other edge lands on the real output: it is put
 *                         straight back.
 *
 * GNOME asks the user once per session ("Capture Input -- Allow Remote Input Capturing"). Until that is answered,
 * or if it is refused, the pointer still reaches this PC's half; it then comes back only by Ctrl+Alt+Shift+Z.
 */
#include "platform/local_input.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>
#include <libei.h>

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "app.h"
#include "local_pane_linux.h"
#include "logging.h"
#include "platform/taskbar_menu.h"
#include "util/bus.h"
#include "util/user_event.h"


#define PORTAL "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define IC "org.freedesktop.portal.InputCapture"


static bool armed, vertical_split, local_first, mouse_here, overlay;
static volatile bool handed;           /* the real pointer is on the virtual output */
static volatile bool captured;         /* InputCapture is active: the client has the mouse and keyboard */
static guint32 activation_id;
static double cur_x, cur_y;            /* captured: the client's own pointer, in window coordinates */

static GThread *ic_thread;
static GMainContext *ic_ctx;
static GMainLoop *ic_loop;
static GDBusConnection *ic_bus;
static char *ic_session;
static guint32 ic_zone_set;
static struct ei *ei;
static guint ei_watch;
static guint sig_activated, sig_deactivated, sig_zones, sig_disabled;

static void ilog(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void ilog(const char *fmt, ...) {
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    commons_log_info("COOP-LP", "[input-linux] %s", line);
}

/* ---- the client window ----------------------------------------------------------------------------------------- */

static SDL_Window *client_window(void) {
    app_t *app = global;
    return app != NULL ? app->ui.window : NULL;
}

/** The divider, and which side of it is this PC's, in window coordinates. */
static void split_geometry(int *w, int *h, int *divider) {
    SDL_Window *win = client_window();
    *w = *h = 0;
    if (win != NULL) {
        SDL_GetWindowSize(win, w, h);
    }
    *divider = vertical_split ? *w / 2 : *h / 2;
}

/* ---- evdev key code -> SDL scancode (the keys a game and the client's shortcuts use) ------------------------- */

static SDL_Scancode evdev_to_scancode(uint32_t code) {
    static const SDL_Scancode table[] = {
            [1] = SDL_SCANCODE_ESCAPE, [2] = SDL_SCANCODE_1, [3] = SDL_SCANCODE_2, [4] = SDL_SCANCODE_3,
            [5] = SDL_SCANCODE_4, [6] = SDL_SCANCODE_5, [7] = SDL_SCANCODE_6, [8] = SDL_SCANCODE_7,
            [9] = SDL_SCANCODE_8, [10] = SDL_SCANCODE_9, [11] = SDL_SCANCODE_0, [12] = SDL_SCANCODE_MINUS,
            [13] = SDL_SCANCODE_EQUALS, [14] = SDL_SCANCODE_BACKSPACE, [15] = SDL_SCANCODE_TAB, [16] = SDL_SCANCODE_Q,
            [17] = SDL_SCANCODE_W, [18] = SDL_SCANCODE_E, [19] = SDL_SCANCODE_R, [20] = SDL_SCANCODE_T,
            [21] = SDL_SCANCODE_Y, [22] = SDL_SCANCODE_U, [23] = SDL_SCANCODE_I, [24] = SDL_SCANCODE_O,
            [25] = SDL_SCANCODE_P, [26] = SDL_SCANCODE_LEFTBRACKET, [27] = SDL_SCANCODE_RIGHTBRACKET,
            [28] = SDL_SCANCODE_RETURN, [29] = SDL_SCANCODE_LCTRL, [30] = SDL_SCANCODE_A, [31] = SDL_SCANCODE_S,
            [32] = SDL_SCANCODE_D, [33] = SDL_SCANCODE_F, [34] = SDL_SCANCODE_G, [35] = SDL_SCANCODE_H,
            [36] = SDL_SCANCODE_J, [37] = SDL_SCANCODE_K, [38] = SDL_SCANCODE_L, [39] = SDL_SCANCODE_SEMICOLON,
            [40] = SDL_SCANCODE_APOSTROPHE, [41] = SDL_SCANCODE_GRAVE, [42] = SDL_SCANCODE_LSHIFT,
            [43] = SDL_SCANCODE_BACKSLASH, [44] = SDL_SCANCODE_Z, [45] = SDL_SCANCODE_X, [46] = SDL_SCANCODE_C,
            [47] = SDL_SCANCODE_V, [48] = SDL_SCANCODE_B, [49] = SDL_SCANCODE_N, [50] = SDL_SCANCODE_M,
            [51] = SDL_SCANCODE_COMMA, [52] = SDL_SCANCODE_PERIOD, [53] = SDL_SCANCODE_SLASH,
            [54] = SDL_SCANCODE_RSHIFT, [55] = SDL_SCANCODE_KP_MULTIPLY, [56] = SDL_SCANCODE_LALT,
            [57] = SDL_SCANCODE_SPACE, [58] = SDL_SCANCODE_CAPSLOCK, [59] = SDL_SCANCODE_F1, [60] = SDL_SCANCODE_F2,
            [61] = SDL_SCANCODE_F3, [62] = SDL_SCANCODE_F4, [63] = SDL_SCANCODE_F5, [64] = SDL_SCANCODE_F6,
            [65] = SDL_SCANCODE_F7, [66] = SDL_SCANCODE_F8, [67] = SDL_SCANCODE_F9, [68] = SDL_SCANCODE_F10,
            [69] = SDL_SCANCODE_NUMLOCKCLEAR, [70] = SDL_SCANCODE_SCROLLLOCK, [71] = SDL_SCANCODE_KP_7,
            [72] = SDL_SCANCODE_KP_8, [73] = SDL_SCANCODE_KP_9, [74] = SDL_SCANCODE_KP_MINUS, [75] = SDL_SCANCODE_KP_4,
            [76] = SDL_SCANCODE_KP_5, [77] = SDL_SCANCODE_KP_6, [78] = SDL_SCANCODE_KP_PLUS, [79] = SDL_SCANCODE_KP_1,
            [80] = SDL_SCANCODE_KP_2, [81] = SDL_SCANCODE_KP_3, [82] = SDL_SCANCODE_KP_0,
            [83] = SDL_SCANCODE_KP_PERIOD, [86] = SDL_SCANCODE_NONUSBACKSLASH, [87] = SDL_SCANCODE_F11,
            [88] = SDL_SCANCODE_F12, [96] = SDL_SCANCODE_KP_ENTER, [97] = SDL_SCANCODE_RCTRL,
            [98] = SDL_SCANCODE_KP_DIVIDE, [99] = SDL_SCANCODE_PRINTSCREEN, [100] = SDL_SCANCODE_RALT,
            [102] = SDL_SCANCODE_HOME, [103] = SDL_SCANCODE_UP, [104] = SDL_SCANCODE_PAGEUP,
            [105] = SDL_SCANCODE_LEFT, [106] = SDL_SCANCODE_RIGHT, [107] = SDL_SCANCODE_END,
            [108] = SDL_SCANCODE_DOWN, [109] = SDL_SCANCODE_PAGEDOWN, [110] = SDL_SCANCODE_INSERT,
            [111] = SDL_SCANCODE_DELETE, [119] = SDL_SCANCODE_PAUSE, [125] = SDL_SCANCODE_LGUI,
            [126] = SDL_SCANCODE_RGUI, [127] = SDL_SCANCODE_APPLICATION,
    };
    return code < sizeof(table) / sizeof(table[0]) ? table[code] : SDL_SCANCODE_UNKNOWN;
}

static Uint16 mods;

static void push_key(uint32_t code, bool down) {
    const SDL_Scancode sc = evdev_to_scancode(code);
    if (sc == SDL_SCANCODE_UNKNOWN) {
        return;
    }
    Uint16 bit = 0;
    switch (sc) {
        case SDL_SCANCODE_LCTRL: bit = KMOD_LCTRL; break;
        case SDL_SCANCODE_RCTRL: bit = KMOD_RCTRL; break;
        case SDL_SCANCODE_LSHIFT: bit = KMOD_LSHIFT; break;
        case SDL_SCANCODE_RSHIFT: bit = KMOD_RSHIFT; break;
        case SDL_SCANCODE_LALT: bit = KMOD_LALT; break;
        case SDL_SCANCODE_RALT: bit = KMOD_RALT; break;
        case SDL_SCANCODE_LGUI: bit = KMOD_LGUI; break;
        case SDL_SCANCODE_RGUI: bit = KMOD_RGUI; break;
        default: break;
    }
    if (bit != 0) {
        mods = down ? (mods | bit) : (mods & ~bit);
    }
    SDL_Window *win = client_window();
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.windowID = win != NULL ? SDL_GetWindowID(win) : 0;
    ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    ev.key.keysym.scancode = sc;
    ev.key.keysym.sym = SDL_GetKeyFromScancode(sc);
    ev.key.keysym.mod = mods;
    SDL_PushEvent(&ev);
}

static unsigned pushed_motions;

static void push_motion(double dx, double dy) {
    int w, h, div;
    split_geometry(&w, &h, &div);
    if (++pushed_motions <= 3 || pushed_motions % 50 == 0) {
        ilog("captured motion #%u: %.0f,%.0f -> %.0f,%.0f in a %dx%d window", pushed_motions, dx, dy,
             SDL_clamp(cur_x + dx, 0, w - 1), SDL_clamp(cur_y + dy, 0, h - 1), w, h);
    }
    cur_x = SDL_clamp(cur_x + dx, 0, w - 1);
    cur_y = SDL_clamp(cur_y + dy, 0, h - 1);
    SDL_Window *win = client_window();
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_MOUSEMOTION;
    ev.motion.windowID = win != NULL ? SDL_GetWindowID(win) : 0;
    ev.motion.x = (Sint32) lround(cur_x);
    ev.motion.y = (Sint32) lround(cur_y);
    ev.motion.xrel = (Sint32) lround(dx);
    ev.motion.yrel = (Sint32) lround(dy);
    SDL_PushEvent(&ev);
}

static void push_button(uint32_t evbtn, bool down) {
    Uint8 b;
    switch (evbtn) {
        case 0x110: b = SDL_BUTTON_LEFT; break;
        case 0x111: b = SDL_BUTTON_RIGHT; break;
        case 0x112: b = SDL_BUTTON_MIDDLE; break;
        case 0x113: b = SDL_BUTTON_X1; break;
        case 0x114: b = SDL_BUTTON_X2; break;
        default: return;
    }
    SDL_Window *win = client_window();
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    ev.button.windowID = win != NULL ? SDL_GetWindowID(win) : 0;
    ev.button.button = b;
    ev.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    ev.button.clicks = 1;
    ev.button.x = (Sint32) lround(cur_x);
    ev.button.y = (Sint32) lround(cur_y);
    SDL_PushEvent(&ev);
}

static void push_wheel(double dx, double dy) {
    SDL_Window *win = client_window();
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_MOUSEWHEEL;
    ev.wheel.windowID = win != NULL ? SDL_GetWindowID(win) : 0;
    ev.wheel.preciseX = (float) dx;
    ev.wheel.preciseY = (float) -dy;
    ev.wheel.x = (Sint32) lround(dx);
    ev.wheel.y = (Sint32) lround(-dy);
    SDL_PushEvent(&ev);
}

/* ---- libei ------------------------------------------------------------------------------------------------------- */

static double scroll_acc_x, scroll_acc_y;

static gboolean ei_ready(gint fd, GIOCondition c, gpointer d) {
    (void) fd; (void) c; (void) d;
    ei_dispatch(ei);
    struct ei_event *e;
    while ((e = ei_get_event(ei)) != NULL) {
        switch (ei_event_get_type(e)) {
            case EI_EVENT_SEAT_ADDED:
                ei_seat_bind_capabilities(ei_event_get_seat(e), EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_KEYBOARD,
                                          EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL, NULL);
                break;
            case EI_EVENT_POINTER_MOTION:
                if (captured) {
                    const double dx = ei_event_pointer_get_dx(e), dy = ei_event_pointer_get_dy(e);
                    /* MEASURED 2026-11-14 (VEGA, GNOME 50.1): 2 ms after a capture began, while a test tool warped
                     * the real pointer through RemoteDesktop, one motion arrived as -8.6e9,9.7e19. A real mouse moves
                     * far less than a screen per event; such a delta is dropped, not clamped to an edge. */
                    if (!isfinite(dx) || !isfinite(dy) || fabs(dx) > 4096 || fabs(dy) > 4096) {
                        ilog("captured motion %.3g,%.3g dropped (not a real movement)", dx, dy);
                    } else {
                        push_motion(dx, dy);
                    }
                }
                break;
            case EI_EVENT_BUTTON_BUTTON:
                if (captured) {
                    push_button(ei_event_button_get_button(e), ei_event_button_get_is_press(e));
                }
                break;
            case EI_EVENT_SCROLL_DELTA:
                if (captured) {
                    scroll_acc_x += ei_event_scroll_get_dx(e);
                    scroll_acc_y += ei_event_scroll_get_dy(e);
                    if (fabs(scroll_acc_y) >= 15 || fabs(scroll_acc_x) >= 15) {
                        push_wheel(scroll_acc_x / 15.0, scroll_acc_y / 15.0);
                        scroll_acc_x = scroll_acc_y = 0;
                    }
                }
                break;
            case EI_EVENT_SCROLL_DISCRETE:
                if (captured) {
                    push_wheel(ei_event_scroll_get_discrete_dx(e) / 120.0, ei_event_scroll_get_discrete_dy(e) / 120.0);
                }
                break;
            case EI_EVENT_KEYBOARD_KEY:
                if (captured) {
                    push_key(ei_event_keyboard_get_key(e), ei_event_keyboard_get_key_is_press(e));
                }
                break;
            case EI_EVENT_DISCONNECT:
                ilog("libei disconnected");
                break;
            default:
                break;
        }
        ei_event_unref(e);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- the InputCapture portal ------------------------------------------------------------------------------------ */

typedef void (*resp_fn)(guint32 code, GVariant *results);

typedef struct {
    resp_fn fn;
    guint sub;
} pending_t;

static void on_response(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *sig,
                        GVariant *params, gpointer data) {
    (void) sender; (void) path; (void) iface; (void) sig;
    pending_t *p = data;
    guint32 code = 2;
    GVariant *results = NULL;
    g_variant_get(params, "(u@a{sv})", &code, &results);
    g_dbus_connection_signal_unsubscribe(c, p->sub);
    resp_fn fn = p->fn;
    g_free(p);
    fn(code, results);
    g_variant_unref(results);
}

static char *request_path(const char *token) {
    char *sender = g_strdup(g_dbus_connection_get_unique_name(ic_bus) + 1);
    for (char *s = sender; *s; s++) {
        if (*s == '.') *s = '_';
    }
    char *path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
    g_free(sender);
    return path;
}

/** Call @p method with a fresh handle_token in @p opts and route its Response to @p fn. @p head: the other arguments
 *  (NULL for CreateSession); the options dict goes in at index @p opts_at of the argument tuple (-1 = last). */
static void portal_request_iface(const char *iface, const char *method, GVariant *head, int opts_at, resp_fn fn,
                                 GVariantBuilder *opts);
static void portal_request_at(const char *method, GVariant *head, int opts_at, resp_fn fn, GVariantBuilder *opts) {
    portal_request_iface(IC, method, head, opts_at, fn, opts);
}
static void portal_request(const char *method, GVariant *head, resp_fn fn, GVariantBuilder *opts) {
    portal_request_at(method, head, -1, fn, opts);
}
static void portal_request_iface(const char *iface, const char *method, GVariant *head, int opts_at, resp_fn fn,
                                 GVariantBuilder *opts) {
    static unsigned n;
    char token[32];
    g_snprintf(token, sizeof(token), "eclipse_lp_%u", ++n);
    g_variant_builder_add(opts, "{sv}", "handle_token", g_variant_new_string(token));
    char *path = request_path(token);
    pending_t *p = g_new0(pending_t, 1);
    p->fn = fn;
    p->sub = g_dbus_connection_signal_subscribe(ic_bus, PORTAL, "org.freedesktop.portal.Request", "Response", path,
                                                NULL, G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE, on_response, p, NULL);
    GVariant *args;
    if (head == NULL) {
        args = g_variant_new("(sa{sv})", "", opts);              /* CreateSession(parent_window, options) */
    } else {
        GVariantBuilder ab;
        g_variant_builder_init(&ab, G_VARIANT_TYPE_TUPLE);
        GVariantIter it;
        g_variant_iter_init(&it, head);
        GVariant *child;
        GVariant *o = g_variant_builder_end(opts);
        int i = 0;
        while ((child = g_variant_iter_next_value(&it)) != NULL) {
            if (i++ == opts_at) {
                g_variant_builder_add_value(&ab, o);
                o = NULL;
            }
            g_variant_builder_add_value(&ab, child);
            g_variant_unref(child);
        }
        if (o != NULL) {
            g_variant_builder_add_value(&ab, o);
        }
        args = g_variant_builder_end(&ab);
        g_variant_unref(head);
    }
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_sync(ic_bus, PORTAL, PORTAL_PATH, iface, method, args, NULL,
                                              G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &err);
    if (r == NULL) {
        ilog("%s.%s failed: %s", strrchr(iface, '.') + 1, method, err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    g_variant_unref(r);
    g_free(path);
}

static void set_barriers(void);

static void barriers_done(guint32 code, GVariant *results) {
    GVariant *failed = g_variant_lookup_value(results, "failed_barriers", G_VARIANT_TYPE("au"));
    const gsize nfailed = failed != NULL ? g_variant_n_children(failed) : 0;
    if (failed != NULL) g_variant_unref(failed);
    ilog("barrier on the virtual output's outer edge: %s (%zu refused)", code == 0 ? "set" : "NOT set", nfailed);
    if (code != 0) {
        return;
    }
    /* ENABLE ONLY AFTER ConnectToEIS. MEASURED 2026-11-14 (GNOME 50.1, icprobe): Enable before the EIS connection was
     * accepted and the barrier was set, yet the capture never activated; the same barrier with Enable after
     * ConnectToEIS activated at once. Later barrier changes (zones changed) re-enable with the connection up. */
    GVariant *r;
    if (ei != NULL) {
        r = g_dbus_connection_call_sync(ic_bus, PORTAL, PORTAL_PATH, IC, "Enable", g_variant_new("(oa{sv})", ic_session, NULL),
                                        NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
        if (r != NULL) g_variant_unref(r);
        return;
    }
    GUnixFDList *fds = NULL;
    GError *err = NULL;
    r = g_dbus_connection_call_with_unix_fd_list_sync(ic_bus, PORTAL, PORTAL_PATH, IC, "ConnectToEIS",
                                                      g_variant_new("(oa{sv})", ic_session, NULL), G_VARIANT_TYPE("(h)"),
                                                      G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &fds, NULL, &err);
    if (r == NULL) {
        ilog("ConnectToEIS failed: %s", err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    gint32 idx = 0;
    g_variant_get(r, "(h)", &idx);
    const int fd = g_unix_fd_list_get(fds, idx, NULL);
    g_variant_unref(r);
    g_object_unref(fds);
    ei = ei_new_receiver(NULL);
    ei_configure_name(ei, "Eclipse co-op");
    if (ei_setup_backend_fd(ei, fd) != 0) {
        ilog("libei setup failed");
        ei_unref(ei);
        ei = NULL;
        return;
    }
    GSource *src = g_unix_fd_source_new(ei_get_fd(ei), G_IO_IN);
    g_source_set_callback(src, (GSourceFunc) (void *) ei_ready, NULL, NULL);
    ei_watch = g_source_attach(src, ic_ctx);
    g_source_unref(src);
    r = g_dbus_connection_call_sync(ic_bus, PORTAL, PORTAL_PATH, IC, "Enable", g_variant_new("(oa{sv})", ic_session, NULL),
                                    NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
    if (r != NULL) g_variant_unref(r);
    ilog("input capture ready: the stream's half takes the mouse and keyboard at the divider");
}

static void zones_done(guint32 code, GVariant *results) {
    if (code != 0) {
        ilog("GetZones refused (%u)", code);
        return;
    }
    g_variant_lookup(results, "zone_set", "u", &ic_zone_set);
    set_barriers();
}

static void set_barriers(void) {
    SDL_Rect v;
    if (!local_pane_linux_geometry(&v, NULL)) {
        return;
    }
    /* the virtual output's edge facing the stream's half -- an outer edge of the layout (local_pane_linux.c) */
    gint x1, y1, x2, y2;
    if (vertical_split) {
        const int x = local_first ? v.x + v.w : v.x;
        x1 = x2 = x;
        y1 = v.y;
        y2 = v.y + v.h - 1;
    } else {
        const int y = local_first ? v.y + v.h : v.y;
        y1 = y2 = y;
        x1 = v.x;
        x2 = v.x + v.w - 1;
    }
    GVariantBuilder barrier, list, opts;
    g_variant_builder_init(&barrier, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&barrier, "{sv}", "barrier_id", g_variant_new_uint32(1));
    g_variant_builder_add(&barrier, "{sv}", "position", g_variant_new("(iiii)", x1, y1, x2, y2));
    g_variant_builder_init(&list, G_VARIANT_TYPE("aa{sv}"));
    g_variant_builder_add_value(&list, g_variant_builder_end(&barrier));
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    ilog("barrier at %d,%d-%d,%d (zone set %u)", x1, y1, x2, y2, ic_zone_set);
    /* SetPointerBarriers(o session, a{sv} options, aa{sv} barriers, u zone_set): the options are SECOND */
    portal_request_at("SetPointerBarriers",
                      g_variant_new("(o@aa{sv}u)", ic_session, g_variant_builder_end(&list), ic_zone_set), 1,
                      barriers_done, &opts);
}

static void get_zones(void) {
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    portal_request("GetZones", g_variant_new("(o)", ic_session), zones_done, &opts);
}

static void gs_start(void);

static void session_done(guint32 code, GVariant *results) {
    const char *s = NULL;
    /* the reply carries it as an object path (measured: GNOME 50 answered 0 with no string "session_handle") */
    GVariant *h = g_variant_lookup_value(results, "session_handle", NULL);
    if (h != NULL && (g_variant_is_of_type(h, G_VARIANT_TYPE_OBJECT_PATH) || g_variant_is_of_type(h, G_VARIANT_TYPE_STRING))) {
        s = g_variant_get_string(h, NULL);
    }
    if (code != 0 || s == NULL) {
        if (h != NULL) g_variant_unref(h);
        ilog("input capture refused or cancelled (%u): the pointer comes back from this PC's half only by "
             "Ctrl+Alt+Shift+Z", code);
        gs_start();
        return;
    }
    ic_session = g_strdup(s);
    g_variant_unref(h);
    ilog("input capture allowed");
    /* ONE DIALOG AT A TIME. MEASURED 2026-11-15 (GNOME 50.1): asking for the shortcuts while GNOME's "Capture Input"
     * question was pending showed only the shortcuts dialog, and neither answer ever came back. */
    gs_start();
    get_zones();
}

static void on_ic_signal(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                         const char *sig, GVariant *params, gpointer data) {
    (void) c; (void) sender; (void) path; (void) iface; (void) data;
    const char *session = NULL;
    GVariant *opts = NULL;
    g_variant_get(params, "(&o@a{sv})", &session, &opts);
    if (ic_session == NULL || strcmp(session, ic_session) != 0) {
        g_variant_unref(opts);
        return;
    }
    if (strcmp(sig, "Activated") == 0) {
        g_variant_lookup(opts, "activation_id", "u", &activation_id);
        /* the client's pointer starts at the divider, on the stream's side, level with where the real one left */
        double px = 0, py = 0;
        g_variant_lookup(opts, "cursor_position", "(dd)", &px, &py);
        SDL_Rect v;
        int w, h, div;
        split_geometry(&w, &h, &div);
        if (local_pane_linux_geometry(&v, NULL) && w > 0 && h > 0) {
            if (vertical_split) {
                cur_x = local_first ? div + 2 : div - 3;
                cur_y = (py - v.y) * h / (double) v.h;
            } else {
                cur_y = local_first ? div + 2 : div - 3;
                cur_x = (px - v.x) * w / (double) v.w;
            }
        }
        captured = true;
        handed = false;
        mods = 0;
        push_motion(0, 0);
        ilog("pointer crossed to the stream's half (capture %u)", activation_id);
    } else if (strcmp(sig, "Deactivated") == 0) {
        captured = false;
    } else if (strcmp(sig, "ZonesChanged") == 0) {
        get_zones();
    } else if (strcmp(sig, "Disabled") == 0) {
        captured = false;
        ilog("input capture disabled by the compositor");
    }
    g_variant_unref(opts);
}

/* ---- the GlobalShortcuts portal: Ctrl+Alt+Shift+Q and Z while this PC's game has the keyboard ------------------- */
/* With the real pointer on this PC's half, this PC's game has the keyboard and Eclipse sees no key. Windows catches
 * both shortcuts there with a low-level hook. MEASURED 2026-11-15 (KDE): Ctrl+Alt+Shift+Q with vkcube focused on the
 * virtual output went to vkcube and the session went on; reading the keyboards' evdev nodes instead found none
 * (keyboards are root:input, not the seat user's -- the guard against keyloggers). The desktop's own way is the
 * GlobalShortcuts portal: the shortcuts work whichever window has the keyboard. */
#define GS "org.freedesktop.portal.GlobalShortcuts"
static char *gs_session;
static guint sig_gs;

static void gs_bound(guint32 code, GVariant *results) {
    (void) results;
    ilog("shortcuts: Ctrl+Alt+Shift+Q (leave) and Ctrl+Alt+Shift+Z (mouse back) %s",
         code == 0 ? "registered with the desktop" : "NOT registered (refused or cancelled)");
}

static void gs_session_done(guint32 code, GVariant *results) {
    GVariant *h = g_variant_lookup_value(results, "session_handle", NULL);
    if (code != 0 || h == NULL) {
        if (h != NULL) g_variant_unref(h);
        ilog("shortcuts: the desktop has no global shortcuts for Eclipse (%u)", code);
        return;
    }
    gs_session = g_strdup(g_variant_get_string(h, NULL));
    g_variant_unref(h);
    GVariantBuilder list, q, z, opts;
    g_variant_builder_init(&q, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&q, "{sv}", "description", g_variant_new_string("Leave the co-op session"));
    g_variant_builder_add(&q, "{sv}", "preferred_trigger", g_variant_new_string("CTRL+ALT+SHIFT+q"));
    g_variant_builder_init(&z, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&z, "{sv}", "description", g_variant_new_string("Bring the mouse back from this PC's game"));
    g_variant_builder_add(&z, "{sv}", "preferred_trigger", g_variant_new_string("CTRL+ALT+SHIFT+z"));
    g_variant_builder_init(&list, G_VARIANT_TYPE("a(sa{sv})"));
    g_variant_builder_add(&list, "(s@a{sv})", "leave", g_variant_builder_end(&q));
    g_variant_builder_add(&list, "(s@a{sv})", "mouse-back", g_variant_builder_end(&z));
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    /* BindShortcuts(o session, a(sa{sv}) shortcuts, s parent_window, a{sv} options) */
    portal_request_iface(GS, "BindShortcuts",
                         g_variant_new("(o@a(sa{sv})s)", gs_session, g_variant_builder_end(&list), ""), -1, gs_bound,
                         &opts);
}

static void on_gs_activated(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                            const char *sig, GVariant *params, gpointer data) {
    (void) c; (void) sender; (void) path; (void) iface; (void) sig; (void) data;
    const char *session = NULL, *id = NULL;
    guint64 ts = 0;
    GVariant *opts = NULL;
    g_variant_get(params, "(&o&st@a{sv})", &session, &id, &ts, &opts);
    if (gs_session != NULL && strcmp(session, gs_session) == 0) {
        if (strcmp(id, "leave") == 0) {
            ilog("shortcuts: Ctrl+Alt+Shift+Q -- leaving the session");
            bus_pushevent(USER_TASKBAR_COMMAND, (void *) (intptr_t) TASKBAR_CMD_DISCONNECT, NULL);
        } else if (strcmp(id, "mouse-back") == 0) {
            ilog("shortcuts: Ctrl+Alt+Shift+Z -- mouse back (%s)", handed ? "it was on this PC's half" : "already here");
            bus_pushevent(USER_TASKBAR_COMMAND, (void *) (intptr_t) TASKBAR_CMD_GIVE_BACK_MOUSE, NULL);
        }
    }
    g_variant_unref(opts);
}

static void gs_start(void) {
    sig_gs = g_dbus_connection_signal_subscribe(ic_bus, PORTAL, GS, "Activated", PORTAL_PATH, NULL, 0, on_gs_activated,
                                                NULL, NULL);
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    char token[32];
    g_snprintf(token, sizeof(token), "eclipse_gs%u", g_random_int() % 100000);
    g_variant_builder_add(&opts, "{sv}", "session_handle_token", g_variant_new_string(token));
    /* CreateSession(a{sv} options): an empty head, the options appended */
    portal_request_iface(GS, "CreateSession", g_variant_new("()"), -1, gs_session_done, &opts);
}

static void gs_stop(void) {
    if (gs_session != NULL) {
        GVariant *r = g_dbus_connection_call_sync(ic_bus, PORTAL, gs_session, "org.freedesktop.portal.Session", "Close",
                                                  NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
        if (r != NULL) g_variant_unref(r);
    }
    if (sig_gs != 0) g_dbus_connection_signal_unsubscribe(ic_bus, sig_gs);
    sig_gs = 0;
    g_clear_pointer(&gs_session, g_free);
}

static gpointer ic_main(gpointer data) {
    (void) data;
    g_main_context_push_thread_default(ic_ctx);
    ic_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (ic_bus != NULL) {
        sig_activated = g_dbus_connection_signal_subscribe(ic_bus, PORTAL, IC, "Activated", PORTAL_PATH, NULL, 0,
                                                           on_ic_signal, NULL, NULL);
        sig_deactivated = g_dbus_connection_signal_subscribe(ic_bus, PORTAL, IC, "Deactivated", PORTAL_PATH, NULL, 0,
                                                             on_ic_signal, NULL, NULL);
        sig_zones = g_dbus_connection_signal_subscribe(ic_bus, PORTAL, IC, "ZonesChanged", PORTAL_PATH, NULL, 0,
                                                       on_ic_signal, NULL, NULL);
        sig_disabled = g_dbus_connection_signal_subscribe(ic_bus, PORTAL, IC, "Disabled", PORTAL_PATH, NULL, 0,
                                                          on_ic_signal, NULL, NULL);
        /* OUTSIDE FLATPAK THE PORTAL KNOWS NO APP ID, and GlobalShortcuts refuses such a caller (MEASURED: "An app id is
         * required"). The host-app registry gives this connection Eclipse's id; it must come before any other call. */
        if (getenv("FLATPAK_ID") == NULL) {
            GVariant *r = g_dbus_connection_call_sync(ic_bus, PORTAL, PORTAL_PATH, "org.freedesktop.host.portal.Registry",
                                                      "Register", g_variant_new("(sa{sv})", "io.github.PreceptorOfMagic.Eclipse",
                                                                                NULL),
                                                      NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
            if (r != NULL) g_variant_unref(r);
        }
        GVariantBuilder opts;
        g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
        char token[32];
        g_snprintf(token, sizeof(token), "eclipse_lp_s%u", g_random_int() % 100000);
        g_variant_builder_add(&opts, "{sv}", "session_handle_token", g_variant_new_string(token));
        g_variant_builder_add(&opts, "{sv}", "capabilities", g_variant_new_uint32(1 | 2));   /* keyboard | pointer */
        ilog("asking for input capture (GNOME shows \"Capture Input\" once per session)");
        portal_request("CreateSession", NULL, session_done, &opts);
        g_main_loop_run(ic_loop);
        gs_stop();
        if (ic_session != NULL) {
            GVariant *r = g_dbus_connection_call_sync(ic_bus, PORTAL, ic_session, "org.freedesktop.portal.Session",
                                                      "Close", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
            if (r != NULL) g_variant_unref(r);
        }
        g_dbus_connection_signal_unsubscribe(ic_bus, sig_activated);
        g_dbus_connection_signal_unsubscribe(ic_bus, sig_deactivated);
        g_dbus_connection_signal_unsubscribe(ic_bus, sig_zones);
        g_dbus_connection_signal_unsubscribe(ic_bus, sig_disabled);
        g_clear_object(&ic_bus);
    }
    if (ei_watch != 0) {
        GSource *s = g_main_context_find_source_by_id(ic_ctx, ei_watch);
        if (s != NULL) g_source_destroy(s);
        ei_watch = 0;
    }
    if (ei != NULL) {
        ei_unref(ei);
        ei = NULL;
    }
    g_clear_pointer(&ic_session, g_free);
    g_main_context_pop_thread_default(ic_ctx);
    return NULL;
}

/** Runs on the input thread: release the capture with the real pointer at (@p x, @p y) (global logical). */
typedef struct {
    double x, y;
} release_t;

static gboolean do_release(gpointer data) {
    release_t *r = data;
    if (ic_session != NULL) {     /* hand_off already cleared `captured`: this is its release */
        GVariantBuilder opts;
        g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&opts, "{sv}", "activation_id", g_variant_new_uint32(activation_id));
        g_variant_builder_add(&opts, "{sv}", "cursor_position", g_variant_new("(dd)", r->x, r->y));
        GVariant *res = g_dbus_connection_call_sync(ic_bus, PORTAL, PORTAL_PATH, IC, "Release",
                                                    g_variant_new("(oa{sv})", ic_session, &opts), NULL,
                                                    G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
        if (res != NULL) g_variant_unref(res);
    }
    g_free(r);
    return G_SOURCE_REMOVE;
}

/* ---- local_input.h ----------------------------------------------------------------------------------------------- */

void local_input_arm(bool vertical, bool first, bool here, int keys) {
    (void) keys;
    if (armed) {
        return;
    }
    armed = true;
    vertical_split = vertical;
    local_first = first;
    mouse_here = here;
    handed = captured = false;
    overlay = false;
    ic_ctx = g_main_context_new();
    ic_loop = g_main_loop_new(ic_ctx, FALSE);
    ic_thread = g_thread_new("eclipse-capture", ic_main, NULL);

    ilog("armed: %s divider, this PC's half %s", vertical ? "vertical" : "horizontal",
         vertical ? (first ? "LEFT" : "RIGHT") : (first ? "TOP" : "BOTTOM"));
}

void local_input_disarm(void) {
    if (!armed) {
        return;
    }
    if (handed) {
        local_input_give_back();       /* never leave the pointer on the virtual output (local_pane_linux_stop) */
    }
    armed = false;
    captured = handed = false;
    if (ic_loop != NULL) {
        g_main_loop_quit(ic_loop);
    }
    if (ic_thread != NULL) {
        g_thread_join(ic_thread);
        ic_thread = NULL;
    }
    g_clear_pointer(&ic_loop, g_main_loop_unref);
    g_clear_pointer(&ic_ctx, g_main_context_unref);
    ilog("disarmed");
}

void local_input_pin_display(const char *name) { (void) name; }

void local_input_set_ui_overlay(bool showing) { overlay = showing; }

bool local_input_hand_off(int x, int y, int w, int h) {
    SDL_Rect v;
    if (!armed || overlay || w <= 0 || h <= 0 || !local_pane_linux_geometry(&v, NULL)) {
        return false;
    }
    const double vx = (double) x * v.w / w, vy = (double) y * v.h / h;
    if (captured) {
        ilog("pointer crossed back to this PC's half at %d,%d of %dx%d: capture released", x, y, w, h);
        /* crossing back over the divider: hand the mouse and keyboard back to the compositor at the same spot */
        captured = false;
        handed = true;
        release_t *r = g_new0(release_t, 1);
        r->x = v.x + SDL_clamp(vx, 1, v.w - 2);
        r->y = v.y + SDL_clamp(vy, 1, v.h - 2);
        g_main_context_invoke(ic_ctx, do_release, r);
        return true;
    }
    /* Not captured: a real pointer event over this PC's half means the real pointer is on the client's output (first
     * entry, or it came off the virtual output by an inner edge): put it on the virtual output, every time. */
    if (local_pane_linux_pointer_to_pane(vx, vy)) {
        handed = true;
        return true;
    }
    return false;
}

bool local_input_handed_off(void) { return handed; }

void local_input_give_back(void) {
    SDL_Rect c;
    int w, h, div;
    if (!handed || !local_pane_linux_geometry(NULL, &c)) {
        return;
    }
    split_geometry(&w, &h, &div);
    /* to the middle of the stream's half of the client window (that output's own pixels: scale 2 on VEGA) */
    double sx = vertical_split ? (local_first ? (div + w) / 2.0 : div / 2.0) : w / 2.0;
    double sy = vertical_split ? h / 2.0 : (local_first ? (div + h) / 2.0 : div / 2.0);
    SDL_Window *win = client_window();
    int ow = w, oh = h;
    SDL_Renderer *r = win != NULL ? SDL_GetRenderer(win) : NULL;
    if (r != NULL) SDL_GetRendererOutputSize(r, &ow, &oh);
    local_pane_linux_pointer_to_client_output(sx * ow / (w > 0 ? w : 1), sy * oh / (h > 0 ? h : 1));
    handed = false;
}

void local_input_pointer_seen(void) {
    /* A real pointer event over the stream's half while the pointer is supposed to be on this PC's half: it came off
     * the virtual output by an inner edge (the wall) -- put it back. Captured events are ours and expected. */
    if (handed && !captured) {
        SDL_Rect v;
        if (local_pane_linux_geometry(&v, NULL)) {
            int mx = 0, my = 0;
            SDL_GetMouseState(&mx, &my);
            int w, h, div;
            split_geometry(&w, &h, &div);
            const double ry = h > 0 ? (double) my / h : 0.5, rx = w > 0 ? (double) mx / w : 0.5;
            if (vertical_split) {
                local_pane_linux_pointer_to_pane(local_first ? 1 : v.w - 2, ry * v.h);
            } else {
                local_pane_linux_pointer_to_pane(rx * v.w, local_first ? 1 : v.h - 2);
            }
        }
    }
}

void local_input_click(int x, int y, int w, int h) { local_input_hand_off(x, y, w, h); }

void local_input_focus_gained(void) {}

bool local_input_pointer_is_real(void) { return true; }

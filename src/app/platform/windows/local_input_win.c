/*
 * THIS PC AS A CO-OP HOST: THE MOUSE AND KEYBOARD HAND-OVER (issue #35; design in memory
 * coop-client-as-host, "Phase 3").
 *
 * Input for the host that IS this machine can never go through the stream: its Apollo would
 * inject it on this same desktop, into the foreground window -- this client -- and it would come
 * back as new input (measured 2026-09-26; local_host_win.c blocks it for that reason). So when
 * the pointer enters this PC's pane the client hands over the REAL cursor instead: it moves it to
 * the same spot on the host's virtual display, where the game runs and Apollo captures, and
 * gives the window there the keyboard. The game then reads the physical mouse and keyboard
 * natively. Nothing is forwarded, so nothing is added to their latency.
 *
 * THE WAY BACK. Once handed over, the cursor is on the virtual display and the client window
 * sees no more mouse events, so a helper thread watches the cursor -- every millisecond, and
 * only while it is away. When it reaches the virtual display's edge that is the divider on
 * screen, or leaves that display altogether, the cursor is put back on the client window just
 * across the divider and the keyboard comes with it. The keyboard is taken back with the
 * helper briefly attached to the game's input queue (Windows lets a thread that shares the
 * foreground's input state change the foreground); if Windows refuses, the client retries on
 * the next pointer event it receives, and failing that the user's next click does it.
 *
 * A CURSOR NOBODY HANDED OVER. When the stream starts, the host's display hook makes the virtual
 * display primary and Windows moves the cursor onto it (MEASURED 2026-09-27: the cursor was at
 * (641,720) on the virtual display the moment the session came up, with the client window at
 * (-2560,-1440)-(0,0)). The two displays only touch at a corner, so it could never be moved back
 * -- which is how the user found the mouse unable to cross the divider (issue #35). So while no
 * hand-over is in force the watcher still looks, twenty times a second, and a cursor it finds on
 * the virtual display is adopted exactly as if it had been handed over.
 *
 * A GAME THAT HOLDS THE CURSOR -- recentres it every frame, or clips it inside its window --
 * never lets it reach an edge. Ctrl+Alt+Shift+Z, moonlight's "give the mouse back", is
 * registered as a global hotkey while the cursor is away and brings it back.
 *
 * ASSIGNED LIKE CONTROLLERS (issue #37). With the co-op setting on, the mouse and the keyboard
 * can each belong to one PC for the session. A mouse assigned to THIS PC is handed over and
 * stays: the divider does not bring it back, only the hotkey does, and then it waits on the
 * client for a click in this PC's pane before it goes over again. A keyboard assigned to this
 * PC stays with its game -- the way back leaves the focus alone, and the client hands the focus
 * straight back whenever it gets it. A keyboard assigned to the OTHER PC stays with the client,
 * which forwards it; a hand-over then leaves the focus where it is.
 *
 * THIS PC'S OTHER WINDOWS BELONG ON ITS VIRTUAL DISPLAY. The physical monitor is the client's for
 * the session: its window fills it and shows both panes. A window this PC opens there covers the
 * stream (user 2026-09-27: "When opening windows on pc1 they appear to end up on top of the
 * stream") and is not in this PC's pane either, because the host captures only the virtual
 * display. So the watcher also listens for windows being shown or brought to the front, and a
 * normal top-level window of another program that is on the client's monitor is moved onto the
 * virtual display -- a few times per window at most, so one that insists cannot start a fight.
 *
 * PADS KEEP WORKING WHILE THE GAME HAS THE KEYBOARD. SDL ignores controllers while its window
 * is in the background unless told otherwise, and in this mode the client is in the background
 * whenever the game on this PC has the focus -- so pads meant for the OTHER PC would stop
 * mid-game. The hint is switched on for the session and put back afterwards.
 */
#include "platform/local_input.h"
#include "local_host_win.h"

/* winsock2 before windows.h: app.h pulls in the sockets headers, which must come first. */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <SDL.h>
#include <SDL_syswm.h>

#include "app.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/** Ctrl+Alt+Shift+Z while the cursor is away: bring it back. */
#define RETURN_HOTKEY_ID 0xEC1
/** Pixels between a cursor placed on either side and the edge that would send it straight back:
 *  without them a hand-over lands ON the divider edge and returns on the next tick. */
#define EDGE_MARGIN 6
/** Windows the guard remembers, and how many times it will move any one of them. */
#define GUARD_TRACK 32
#define GUARD_MOVES_PER_WINDOW 3
/** How often the watcher looks for a cursor that is on the virtual display without a hand-over. */
#define ADOPT_POLL_MS 50
/** After a search that found no virtual display, how long before the next full search: the
 *  classification costs a few display-config queries, too many to repeat twenty times a second. */
#define SEARCH_BACKOFF_MS 1000
/** Log the first few hand-overs of a session in full, then every Nth: enough to see it work
 *  without a line per crossing for a whole evening. */
#define LOG_FIRST 20
#define LOG_EVERY 100

static struct {
    bool armed;
    bool vertical;                 /* panes side by side */
    bool local_first;              /* this PC's pane is the left/top one */
    bool mouse_here;               /* the mouse is ASSIGNED to this PC: no way back at the divider */
    int keys;                      /* LOCAL_INPUT_KEYS_* */
    volatile LONG released;        /* the hotkey took an assigned mouse back; a click re-hands it */
    bool keys_given;               /* LOCAL_INPUT_KEYS_HERE: the game has been given the keyboard */
    HWND client;                   /* this client's window */
    HANDLE thread, wake, timer;
    volatile LONG quit;
    volatile LONG handed;          /* 1 while the cursor is on the virtual display */
    volatile LONG handing;         /* 1 while the UI thread is mid-hand-over: not a stranded cursor */
    volatile LONG focus_pending;   /* the cursor is back but the keyboard is not */
    RECT vdd;                      /* the virtual display, written before `handed` is set */
    wchar_t vdd_dev[CCHDEVICENAME];/* its GDI name, so a hand-over need not re-classify */
    HWND game;                     /* the window given the keyboard at the last hand-over */
    struct { HWND hwnd; int moves; } guard[GUARD_TRACK];   /* watcher thread only */
    int guard_next;
    unsigned guard_moved;
    CRITICAL_SECTION vdd_lock;     /* find_virtual_display runs on the UI thread and the watcher */
    bool vdd_lock_ready;
    DWORD last_miss;               /* tick of the last search that found no virtual display */
    unsigned handoffs, returns;
    bool had_joy_hint;
    char joy_hint[16];
    bool had_click_hint;
    char click_hint[16];
} li;

/* ---- Finding the virtual display --------------------------------------------------------- */

/* The SAME three signals dispwake uses (xlink/dispwake/dispwake.cpp pathIsVirtual), any one of
 * which suffices: an indirect output technology, the VDD driver's EDID vendor ("SMK", the
 * SudoMaker VDD Apollo ships -- it reports its technology as HDMI, so technology alone misses
 * it), or one of Apollo's VDD names. */
static bool path_is_virtual(const DISPLAYCONFIG_PATH_INFO *p) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
    memset(&tn, 0, sizeof(tn));
    tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    tn.header.size = sizeof(tn);
    tn.header.adapterId = p->targetInfo.adapterId;
    tn.header.id = p->targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS) {
        return false;
    }
    if (tn.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED ||
        tn.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL ||
        tn.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST) {
        return true;
    }
    if (tn.flags.edidIdsValid) {
        const UINT16 id = (UINT16) ((tn.edidManufactureId >> 8) | (tn.edidManufactureId << 8));
        const char vendor[4] = {(char) ('A' + ((id >> 10) & 0x1F) - 1), (char) ('A' + ((id >> 5) & 0x1F) - 1),
                                (char) ('A' + (id & 0x1F) - 1), '\0'};
        if (strcmp(vendor, "SMK") == 0) {
            return true;
        }
    }
    return wcsstr(tn.monitorFriendlyDeviceName, L"Moonli") != NULL ||
           wcsstr(tn.monitorFriendlyDeviceName, L"Virtual Displ") != NULL;
}

typedef struct {
    const wchar_t *dev;
    RECT mon;
    bool found;
} find_monitor_t;

static BOOL CALLBACK find_cb(HMONITOR mon, HDC dc, LPRECT rect, LPARAM param) {
    (void) dc;
    (void) rect;
    find_monitor_t *f = (find_monitor_t *) param;
    MONITORINFOEXW mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, (MONITORINFO *) &mi) && wcscmp(mi.szDevice, f->dev) == 0) {
        f->mon = mi.rcMonitor;
        f->found = true;
        return FALSE;
    }
    return TRUE;
}

static bool monitor_rect(const wchar_t *dev, RECT *out) {
    find_monitor_t f = {.dev = dev, .found = false};
    EnumDisplayMonitors(NULL, NULL, find_cb, (LPARAM) &f);
    if (f.found) {
        *out = f.mon;
    }
    return f.found;
}

/** The virtual display's desktop rectangle. The classification costs a few display-config
 *  queries, so the display's name is remembered and only its rectangle looked up each time;
 *  a name that is gone (Apollo re-created the display) triggers a fresh search. */
static bool find_virtual_display_locked(RECT *out);

static bool find_virtual_display(RECT *out) {
    EnterCriticalSection(&li.vdd_lock);
    const bool found = find_virtual_display_locked(out);
    LeaveCriticalSection(&li.vdd_lock);
    return found;
}

static bool find_virtual_display_locked(RECT *out) {
    if (li.vdd_dev[0] != L'\0' && monitor_rect(li.vdd_dev, out)) {
        return true;
    }
    li.vdd_dev[0] = L'\0';
    if (li.last_miss != 0 && GetTickCount() - li.last_miss < SEARCH_BACKOFF_MS) {
        return false;
    }
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || np == 0) {
        return false;
    }
    DISPLAYCONFIG_PATH_INFO *paths = calloc(np, sizeof(*paths));
    DISPLAYCONFIG_MODE_INFO *modes = calloc(nm > 0 ? nm : 1, sizeof(*modes));
    bool found = false;
    if (paths != NULL && modes != NULL &&
        QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < np && !found; i++) {
            if (!path_is_virtual(&paths[i])) {
                continue;
            }
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sn;
            memset(&sn, 0, sizeof(sn));
            sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sn.header.size = sizeof(sn);
            sn.header.adapterId = paths[i].sourceInfo.adapterId;
            sn.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sn.header) == ERROR_SUCCESS && monitor_rect(sn.viewGdiDeviceName, out)) {
                wcsncpy(li.vdd_dev, sn.viewGdiDeviceName, CCHDEVICENAME - 1);
                li.vdd_dev[CCHDEVICENAME - 1] = L'\0';
                found = true;
            }
        }
    }
    free(paths);
    free(modes);
    li.last_miss = found ? 0 : GetTickCount();
    if (li.last_miss == 0 && !found) {
        li.last_miss = 1;
    }
    return found;
}

static bool log_this(unsigned n);

/* ---- The keyboard -------------------------------------------------------------------------- */

static void describe(HWND w, char *out, size_t len) {
    char cls[64] = "", title[64] = "";
    DWORD pid = 0;
    if (w != NULL) {
        GetClassNameA(w, cls, sizeof(cls));
        GetWindowTextA(w, title, sizeof(title));
        GetWindowThreadProcessId(w, &pid);
    }
    snprintf(out, len, "%p class=%s pid=%lu title=\"%s\"", (void *) w, cls, (unsigned long) pid, title);
}

/** The top-level window under @p p, unless it is one of ours: the keyboard's new owner. */
static HWND host_window_at(POINT p) {
    HWND w = WindowFromPoint(p);
    w = w != NULL ? GetAncestor(w, GA_ROOT) : NULL;
    DWORD pid = 0;
    if (w != NULL) {
        GetWindowThreadProcessId(w, &pid);
    }
    return (w == NULL || pid == GetCurrentProcessId()) ? NULL : w;
}

/** Make @p target the foreground window from a thread that is not the foreground's: attach to
 *  the foreground thread's input state for the one call, which is what makes Windows allow it. */
static bool take_foreground(HWND target) {
    HWND fg = GetForegroundWindow();
    if (fg == target) {
        return true;
    }
    const DWORD me = GetCurrentThreadId();
    const DWORD fg_tid = fg != NULL ? GetWindowThreadProcessId(fg, NULL) : 0;
    const bool attached = fg_tid != 0 && fg_tid != me && AttachThreadInput(me, fg_tid, TRUE);
    SetForegroundWindow(target);
    if (attached) {
        AttachThreadInput(me, fg_tid, FALSE);
    }
    return GetForegroundWindow() == target;
}

/** Give the keyboard to the window in the middle of the virtual display -- for a keyboard
 *  ASSIGNED to this PC while the cursor is elsewhere. Only works while the client is the
 *  foreground window, which is exactly when the keyboard is in the wrong place. */
static void keyboard_to_host(const char *why, bool throttled) {
    /* At most twice a second on the pointer path, which asks on every event until it works. */
    static DWORD last_try = 0;
    static unsigned tries = 0;
    const DWORD now = GetTickCount();
    if (throttled && last_try != 0 && now - last_try < 500) {
        return;
    }
    last_try = now;
    RECT v;
    if (!find_virtual_display(&v)) {
        return;
    }
    POINT mid = {(v.left + v.right) / 2, (v.top + v.bottom) / 2};
    HWND target = li.game != NULL && IsWindow(li.game) ? li.game : host_window_at(mid);
    if (target == NULL) {
        return;
    }
    const bool ok = SetForegroundWindow(target) && GetForegroundWindow() == target;
    li.keys_given = ok;
    if (log_this(++tries)) {
        char who[160];
        describe(target, who, sizeof(who));
        local_host_log("input: keyboard (assigned to this PC) given to %s on %s: %s", who, why, ok ? "ok" : "REFUSED");
    }
}

/* ---- The way back --------------------------------------------------------------------------- */

static bool log_this(unsigned n) {
    return n <= LOG_FIRST || n % LOG_EVERY == 0;
}

/** Put the cursor back on the client window just across the divider, at the matching point
 *  along it, and take the keyboard back. Runs on the helper thread, or on the UI thread once
 *  the helper is gone. */
static void come_back(POINT at, const char *why) {
    const RECT v = li.vdd;
    RECT c;
    POINT origin = {0, 0};
    if (!GetClientRect(li.client, &c) || !ClientToScreen(li.client, &origin) || c.right <= 0 || c.bottom <= 0) {
        InterlockedExchange(&li.handed, 0);
        return;
    }
    const LONG w = c.right, h = c.bottom;
    const LONG vw = v.right - v.left > 0 ? v.right - v.left : 1;
    const LONG vh = v.bottom - v.top > 0 ? v.bottom - v.top : 1;
    const LONG ax = at.x < v.left ? v.left : (at.x >= v.right ? v.right - 1 : at.x);
    const LONG ay = at.y < v.top ? v.top : (at.y >= v.bottom ? v.bottom - 1 : at.y);
    POINT to;
    if (li.mouse_here) {
        /* An ASSIGNED mouse only comes back to use the client itself: the middle of the window,
         * and it stays here until a click in this PC's pane sends it over again. */
        to.x = w / 2;
        to.y = h / 2;
        InterlockedExchange(&li.released, 1);
    } else if (li.vertical) {
        to.x = w / 2 + (li.local_first ? EDGE_MARGIN : -EDGE_MARGIN - 1);
        to.y = (LONG) ((long long) (ay - v.top) * h / vh);
    } else {
        to.x = (LONG) ((long long) (ax - v.left) * w / vw);
        to.y = h / 2 + (li.local_first ? EDGE_MARGIN : -EDGE_MARGIN - 1);
    }
    to.x += origin.x;
    to.y += origin.y;

    /* CURSOR FIRST, then the keyboard. SDL answers the client gaining the focus by reading the
     * cursor position (GetCursorPos) and sending it as a motion -- read while the cursor was still
     * on the virtual display, that became a jump to the window's corner. A game that confines
     * the cursor may clip it back before it loses the focus, so the move is checked and repeated
     * once the keyboard has changed hands. A keyboard ASSIGNED to this PC stays with its game. */
    ClipCursor(NULL);
    SetCursorPos(to.x, to.y);
    const bool keys = li.keys == LOCAL_INPUT_KEYS_HERE ? true : take_foreground(li.client);
    POINT placed;
    if (GetCursorPos(&placed) && (placed.x != to.x || placed.y != to.y)) {
        ClipCursor(NULL);
        SetCursorPos(to.x, to.y);
    }
    InterlockedExchange(&li.focus_pending, keys ? 0 : 1);
    InterlockedExchange(&li.handed, 0);
    li.returns++;
    if (log_this(li.returns)) {
        POINT now;
        GetCursorPos(&now);
        local_host_log("input: back #%u (%s) from (%ld,%ld) -> client (%ld,%ld), cursor now (%ld,%ld); keyboard %s",
                       li.returns, why, (long) at.x, (long) at.y, (long) to.x, (long) to.y, (long) now.x,
                       (long) now.y, li.keys == LOCAL_INPUT_KEYS_HERE ? "stays with this PC (assigned)"
                                     : (keys ? "back" : "REFUSED -- retried on the next pointer event"));
    }
}

static void check_cursor(void) {
    POINT p;
    if (!GetCursorPos(&p)) {
        return;   /* the secure desktop (UAC, Ctrl+Alt+Del) owns the cursor for now */
    }
    const RECT v = li.vdd;
    const bool out = p.x < v.left || p.x >= v.right || p.y < v.top || p.y >= v.bottom;
    if (li.mouse_here) {
        /* ASSIGNED here: no way back at the divider, and a cursor that strays off the display
         * (a monitor beside it) is put back on its edge rather than returned. */
        if (out) {
            SetCursorPos(p.x < v.left ? v.left : (p.x >= v.right ? v.right - 1 : p.x),
                         p.y < v.top ? v.top : (p.y >= v.bottom ? v.bottom - 1 : p.y));
        }
        return;
    }
    bool divider;
    if (li.vertical) {
        divider = li.local_first ? p.x >= v.right - 1 : p.x <= v.left;
    } else {
        divider = li.local_first ? p.y >= v.bottom - 1 : p.y <= v.top;
    }
    if (out || divider) {
        come_back(p, out ? "left the display" : "divider");
    }
}

/* ---- Keeping this PC's windows off the client's monitor ------------------------------------ */

/** Shell and transient windows the guard never touches, by class. */
static bool guard_skip_class(const char *cls) {
    static const char *const skip[] = {"#32768", "tooltips_class32", "Shell_TrayWnd", "Shell_SecondaryTrayWnd",
                                       "Progman", "WorkerW", "Windows.UI.Core.CoreWindow", "ForegroundStaging",
                                       "XamlExplorerHostIslandWindow", "TaskListThumbnailWnd", "MultitaskingViewFrame"};
    for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
        if (strcmp(cls, skip[i]) == 0) {
            return true;
        }
    }
    return false;
}

/** How many times this window has been moved; claims a slot for a new one. */
static int *guard_moves(HWND h) {
    for (int i = 0; i < GUARD_TRACK; i++) {
        if (li.guard[i].hwnd == h) {
            return &li.guard[i].moves;
        }
    }
    const int i = li.guard_next;
    li.guard_next = (li.guard_next + 1) % GUARD_TRACK;
    li.guard[i].hwnd = h;
    li.guard[i].moves = 0;
    return &li.guard[i].moves;
}

static void guard_consider(HWND h, const char *event) {
    if (h == NULL || !IsWindowVisible(h) || IsIconic(h) || GetAncestor(h, GA_ROOT) != h) {
        return;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) {
        return;
    }
    const LONG style = GetWindowLongW(h, GWL_STYLE);
    const LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if ((style & WS_CHILD) || (ex & WS_EX_TOOLWINDOW)) {
        return;
    }
    char cls[64] = "";
    GetClassNameA(h, cls, sizeof(cls));
    if (guard_skip_class(cls)) {
        return;
    }
    HMONITOR home = MonitorFromWindow(li.client, MONITOR_DEFAULTTONULL);
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONULL);
    if (home == NULL || mon != home) {
        return;
    }
    RECT r;
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetWindowRect(h, &r) || !GetMonitorInfoW(mon, &mi)) {
        return;
    }
    /* A normal window (it has a caption), or a borderless one big enough to be a game; not the
     * small popups -- menus, toasts, pickers -- that belong where they appeared. */
    const long long area = (long long) (r.right - r.left) * (r.bottom - r.top);
    const long long mon_area = (long long) (mi.rcMonitor.right - mi.rcMonitor.left) *
                               (mi.rcMonitor.bottom - mi.rcMonitor.top);
    const bool captioned = (style & WS_CAPTION) == WS_CAPTION;
    const bool big_popup = (style & WS_POPUP) && area * 10 >= mon_area * 6;
    if (!captioned && !big_popup) {
        return;
    }
    int *moves = guard_moves(h);
    if (*moves >= GUARD_MOVES_PER_WINDOW) {
        return;
    }
    RECT v;
    if (!find_virtual_display(&v)) {
        return;
    }
    MONITORINFO vmi;
    vmi.cbSize = sizeof(vmi);
    HMONITOR vmon = MonitorFromRect(&v, MONITOR_DEFAULTTONEAREST);
    if (vmon == home || !GetMonitorInfoW(vmon, &vmi)) {
        return;
    }
    const RECT wa = vmi.rcWork;
    const bool zoomed = IsZoomed(h) != 0;
    const LONG ww = r.right - r.left < wa.right - wa.left ? r.right - r.left : wa.right - wa.left;
    const LONG wh = r.bottom - r.top < wa.bottom - wa.top ? r.bottom - r.top : wa.bottom - wa.top;
    const LONG x = wa.left + (wa.right - wa.left - ww) / 2;
    const LONG y = wa.top + (wa.bottom - wa.top - wh) / 2;
    /* ASYNC throughout: the window belongs to another program, and a hung one must not stall
     * this thread -- the cursor watcher runs on it. The three requests reach it in order. */
    if (zoomed) {
        ShowWindowAsync(h, SW_RESTORE);
    }
    const BOOL ok = SetWindowPos(h, NULL, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    if (zoomed) {
        ShowWindowAsync(h, SW_MAXIMIZE);
    }
    (*moves)++;
    li.guard_moved++;
    if (log_this(li.guard_moved)) {
        char who[160];
        describe(h, who, sizeof(who));
        local_host_log("input: window %s %s on the client's monitor at (%ld,%ld)-(%ld,%ld)%s -> moved to the virtual "
                       "display at (%ld,%ld) %ldx%ld: %s (move %d of this window)", who, event, (long) r.left,
                       (long) r.top, (long) r.right, (long) r.bottom, zoomed ? " maximized" : "", (long) x, (long) y,
                       (long) ww, (long) wh, ok ? "ok" : "REFUSED", *moves);
    }
}

static void CALLBACK guard_event(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG id_object, LONG id_child,
                                 DWORD thread, DWORD time) {
    (void) hook;
    (void) thread;
    (void) time;
    if (id_object != OBJID_WINDOW || id_child != CHILDID_SELF) {
        return;
    }
    guard_consider(hwnd, event == EVENT_SYSTEM_FOREGROUND ? "came to the front" : "opened");
}

/** A cursor on the virtual display with no hand-over in force: adopt it (see the header). */
static void adopt_stranded_cursor(void) {
    static unsigned adopted = 0;
    POINT p;
    RECT v;
    /* MEASURED K2 22:15:43.492: the watcher's poll landed between a hand-over's SetCursorPos and
     * its `handed`, and "adopted" a cursor the UI thread was placing. */
    if (InterlockedCompareExchange(&li.handing, 0, 0) != 0 || !GetCursorPos(&p) || !find_virtual_display(&v)) {
        return;
    }
    if (p.x < v.left || p.x >= v.right || p.y < v.top || p.y >= v.bottom) {
        return;
    }
    li.vdd = v;
    /* The keyboard follows the pointer onto this PC's pane, as a hand-over would give it. The
     * client is usually in front here (the session has just started from it), which is what
     * lets Windows accept the change. */
    bool keys = false;
    HWND target = host_window_at(p);
    if (li.keys != LOCAL_INPUT_KEYS_ELSEWHERE && target != NULL) {
        keys = take_foreground(target);
        if (keys) {
            li.game = target;
            li.keys_given = true;
        }
    }
    InterlockedExchange(&li.released, 0);
    InterlockedExchange(&li.focus_pending, 0);
    InterlockedExchange(&li.handed, 1);
    if (log_this(++adopted)) {
        char who[160];
        describe(target, who, sizeof(who));
        local_host_log("input: cursor found on the virtual display at (%ld,%ld) with no hand-over (#%u) -- adopted, "
                       "the divider brings it back; keyboard to %s: %s", (long) p.x, (long) p.y, adopted, who,
                       li.keys == LOCAL_INPUT_KEYS_ELSEWHERE ? "kept by the client" : (keys ? "ok" : "NOT given"));
    }
}

static DWORD WINAPI watch_thread(LPVOID arg) {
    (void) arg;
    MSG m;
    /* This thread's message queue must exist before RegisterHotKey can post to it. */
    PeekMessageW(&m, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    bool ticking = false, hotkey = false;
    /* Out-of-context: the callbacks run on THIS thread, from inside its PeekMessage calls. */
    HWINEVENTHOOK shown = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, NULL, guard_event, 0, 0,
                                          WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK front = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, guard_event, 0, 0,
                                          WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    for (;;) {
        HANDLE hs[2] = {li.wake, li.timer};
        const DWORD timeout = !ticking ? ADOPT_POLL_MS : (li.timer == NULL ? 1 : INFINITE);
        MsgWaitForMultipleObjects(li.timer != NULL ? 2 : 1, hs, FALSE, timeout, QS_ALLINPUT);
        if (InterlockedCompareExchange(&li.quit, 0, 0) != 0) {
            break;
        }
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_HOTKEY && m.wParam == RETURN_HOTKEY_ID &&
                InterlockedCompareExchange(&li.handed, 0, 0) != 0) {
                POINT p;
                GetCursorPos(&p);
                come_back(p, "hotkey");
            }
        }
        const bool handed = InterlockedCompareExchange(&li.handed, 0, 0) != 0;
        if (handed != ticking) {
            if (handed) {
                if (li.timer != NULL) {
                    LARGE_INTEGER due;
                    due.QuadPart = -10000;   /* 1 ms, then every 1 ms */
                    SetWaitableTimer(li.timer, &due, 1, NULL, NULL, FALSE);
                }
                hotkey = RegisterHotKey(NULL, RETURN_HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'Z');
            } else {
                if (li.timer != NULL) {
                    CancelWaitableTimer(li.timer);
                }
                if (hotkey) {
                    UnregisterHotKey(NULL, RETURN_HOTKEY_ID);
                    hotkey = false;
                }
            }
            ticking = handed;
        }
        if (handed) {
            check_cursor();
        } else {
            adopt_stranded_cursor();
        }
    }
    if (hotkey) {
        UnregisterHotKey(NULL, RETURN_HOTKEY_ID);
    }
    if (shown != NULL) {
        UnhookWinEvent(shown);
    }
    if (front != NULL) {
        UnhookWinEvent(front);
    }
    if (li.timer != NULL) {
        CancelWaitableTimer(li.timer);
    }
    return 0;
}

/* ---- Public ---------------------------------------------------------------------------------- */

void local_input_arm(bool vertical, bool local_first, bool mouse_here, int keys) {
    if (li.armed) {
        local_input_disarm();
    }
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (global == NULL || global->ui.window == NULL || !SDL_GetWindowWMInfo(global->ui.window, &info) ||
        info.subsystem != SDL_SYSWM_WINDOWS) {
        local_host_log("input: NOT armed -- no client window");
        return;
    }
    if (!li.vdd_lock_ready) {
        InitializeCriticalSection(&li.vdd_lock);   /* process lifetime: arm/disarm repeat per session */
        li.vdd_lock_ready = true;
    }
    li.last_miss = 0;
    li.client = info.info.win.window;
    li.vertical = vertical;
    li.local_first = local_first;
    li.mouse_here = mouse_here;
    li.keys = keys;
    li.released = 0;
    li.keys_given = false;
    li.quit = 0;
    li.handed = 0;
    li.focus_pending = 0;
    li.game = NULL;
    li.handoffs = li.returns = 0;
    memset(li.guard, 0, sizeof(li.guard));
    li.guard_next = 0;
    li.guard_moved = 0;
    li.vdd_dev[0] = L'\0';

    const char *hint = SDL_GetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
    li.had_joy_hint = hint != NULL;
    snprintf(li.joy_hint, sizeof(li.joy_hint), "%s", hint != NULL ? hint : "");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    /* A click that brings the client to the front must still reach the stream: with the keyboard
     * assigned to this PC the client is never the active window, so without this EVERY click on
     * it was swallowed -- and a taken-back assigned mouse could never be sent over again (K3,
     * 2026-09-28 00:32). SDL's own header: "0" (the default) ignores activating clicks. */
    const char *click = SDL_GetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH);
    li.had_click_hint = click != NULL;
    snprintf(li.click_hint, sizeof(li.click_hint), "%s", click != NULL ? click : "");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");

    li.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    /* A high-resolution timer when Windows has one (1803+); otherwise the wait below polls
     * at the scheduler's own granularity, which only makes the way back a little later. */
    li.timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    li.thread = li.wake != NULL ? CreateThread(NULL, 0, watch_thread, NULL, 0, NULL) : NULL;
    li.armed = li.thread != NULL;
    RECT v = {0, 0, 0, 0};
    const bool have = find_virtual_display(&v);
    local_host_log("input: %s -- this PC's pane is the %s one; mouse %s, keyboard %s; virtual display %s "
                   "(%ld,%ld)-(%ld,%ld); high-res timer %s; pads read in the background",
                   li.armed ? "armed" : "NOT armed (no helper thread)",
                   vertical ? (local_first ? "LEFT" : "RIGHT") : (local_first ? "TOP" : "BOTTOM"),
                   mouse_here ? "ASSIGNED to this PC" : "follows the pointer",
                   keys == LOCAL_INPUT_KEYS_HERE ? "ASSIGNED to this PC"
                                                 : (keys == LOCAL_INPUT_KEYS_ELSEWHERE ? "ASSIGNED to the other PC"
                                                                                       : "follows the pointer"),
                   have ? "found now" : "not up yet", (long) v.left, (long) v.top, (long) v.right, (long) v.bottom,
                   li.timer != NULL ? "yes" : "no");
}

void local_input_disarm(void) {
    if (!li.armed) {
        return;
    }
    InterlockedExchange(&li.quit, 1);
    SetEvent(li.wake);
    WaitForSingleObject(li.thread, 2000);
    CloseHandle(li.thread);
    li.thread = NULL;
    if (InterlockedCompareExchange(&li.handed, 0, 0) != 0) {
        POINT p;
        GetCursorPos(&p);
        come_back(p, "session ended");
    }
    CloseHandle(li.wake);
    li.wake = NULL;
    if (li.timer != NULL) {
        CloseHandle(li.timer);
        li.timer = NULL;
    }
    /* "0" is SDL's documented default for this hint, so an unset hint is put back as "0". */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, li.had_joy_hint ? li.joy_hint : "0");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, li.had_click_hint ? li.click_hint : "0");
    li.armed = false;
    local_host_log("input: disarmed after %u hand-over(s), %u return(s); %u window move(s) off the client's monitor",
                   li.handoffs, li.returns, li.guard_moved);
}

bool local_input_hand_off(int x, int y, int w, int h) {
    if (!li.armed || w <= 0 || h <= 0) {
        return false;
    }
    if (InterlockedCompareExchange(&li.handed, 0, 0) != 0) {
        return true;
    }
    if (InterlockedCompareExchange(&li.released, 0, 0) != 0) {
        return false;   /* an assigned mouse the hotkey took back: it waits for a click */
    }
    RECT v;
    if (!find_virtual_display(&v)) {
        static unsigned missing = 0;
        if (log_this(++missing)) {
            local_host_log("input: pointer in this PC's pane but NO virtual display found (#%u) -- not handed over",
                           missing);
        }
        return false;
    }
    /* The same spot on the virtual display, kept EDGE_MARGIN off the divider edge. */
    POINT p;
    p.x = v.left + (LONG) ((long long) x * (v.right - v.left) / w);
    p.y = v.top + (LONG) ((long long) y * (v.bottom - v.top) / h);
    if (li.vertical) {
        if (li.local_first && p.x > v.right - 1 - EDGE_MARGIN) p.x = v.right - 1 - EDGE_MARGIN;
        if (!li.local_first && p.x < v.left + EDGE_MARGIN) p.x = v.left + EDGE_MARGIN;
    } else {
        if (li.local_first && p.y > v.bottom - 1 - EDGE_MARGIN) p.y = v.bottom - 1 - EDGE_MARGIN;
        if (!li.local_first && p.y < v.top + EDGE_MARGIN) p.y = v.top + EDGE_MARGIN;
    }
    li.vdd = v;
    InterlockedExchange(&li.handing, 1);
    SetCursorPos(p.x, p.y);
    /* The client is the foreground window here -- the user is moving the pointer over it -- so
     * Windows lets it hand the foreground to the game. A keyboard assigned to the other PC stays. */
    HWND target = host_window_at(p);
    const bool keys = li.keys != LOCAL_INPUT_KEYS_ELSEWHERE && target != NULL && SetForegroundWindow(target);
    if (keys) {
        li.game = target;
        li.keys_given = true;
    }
    InterlockedExchange(&li.focus_pending, 0);
    InterlockedExchange(&li.handed, 1);
    InterlockedExchange(&li.handing, 0);
    SetEvent(li.wake);
    li.handoffs++;
    if (log_this(li.handoffs)) {
        char who[160];
        describe(target, who, sizeof(who));
        local_host_log("input: handed over #%u at pane (%d,%d) of %dx%d -> display (%ld,%ld) in (%ld,%ld)-(%ld,%ld); "
                       "keyboard to %s: %s",
                       li.handoffs, x, y, w, h, (long) p.x, (long) p.y, (long) v.left, (long) v.top, (long) v.right,
                       (long) v.bottom, who,
                       li.keys == LOCAL_INPUT_KEYS_ELSEWHERE ? "kept by the client (assigned to the other PC)"
                                                             : (keys ? "ok" : "NOT given"));
    }
    return true;
}

bool local_input_handed_off(void) {
    return li.armed && InterlockedCompareExchange(&li.handed, 0, 0) != 0;
}

void local_input_pointer_seen(void) {
    if (!li.armed) {
        return;
    }
    if (li.keys == LOCAL_INPUT_KEYS_HERE && !li.keys_given) {
        /* A keyboard assigned to this PC, and the cursor has not been over yet (or never will:
         * the mouse may belong to the other PC) -- the client is in front, so it can give it. */
        keyboard_to_host("the first pointer event", true);
        return;
    }
    if (InterlockedCompareExchange(&li.focus_pending, 0, 0) == 0) {
        return;
    }
    /* This pointer event was input delivered to this process -- which is one of the cases in
     * which Windows lets a background process take the foreground. */
    SetForegroundWindow(li.client);
    if (GetForegroundWindow() == li.client) {
        InterlockedExchange(&li.focus_pending, 0);
        local_host_log("input: keyboard back on a pointer event");
    }
}

void local_input_click(int x, int y, int w, int h) {
    if (!li.armed || InterlockedCompareExchange(&li.released, 0, 0) == 0) {
        return;
    }
    InterlockedExchange(&li.released, 0);
    local_host_log("input: click in this PC's pane -> the assigned mouse goes over again");
    local_input_hand_off(x, y, w, h);
}

void local_input_focus_gained(void) {
    if (!li.armed || li.keys != LOCAL_INPUT_KEYS_HERE) {
        return;
    }
    keyboard_to_host("the client taking the focus", false);
}

bool local_input_pointer_is_real(void) {
    if (!li.armed) {
        return true;
    }
    /* SDL synthesizes a motion when the cursor LEAVES the window (and when the window gains the
     * focus), clamped into the window -- the bottom-right corner, since the virtual display lies
     * below and right of the panel. MEASURED 2026-09-27 on PC2's own cursor: every hand-over
     * sent it to (1279,1439). An event is real only while the real cursor is over the window. */
    POINT p, o = {0, 0};
    RECT c;
    if (!GetCursorPos(&p) || !GetClientRect(li.client, &c) || !ClientToScreen(li.client, &o)) {
        return true;
    }
    return p.x >= o.x && p.x < o.x + c.right && p.y >= o.y && p.y < o.y + c.bottom;
}

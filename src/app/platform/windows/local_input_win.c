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
#include <Limelight.h>

#include "app.h"
#include "platform/taskbar_menu.h"
#include "util/bus.h"
#include "util/user_event.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/** Ctrl+Alt+Shift+Z while the cursor is away: bring it back. */
#define RETURN_HOTKEY_ID 0xEC1
/** Ctrl+Alt+Shift+Q while this PC's game has the keyboard: the stream's own Disconnect combo
 *  (session_keyboard.c KeyComboQuit), which the client cannot see then. */
#define LEAVE_HOTKEY_ID 0xEC2
/** Pixels between a cursor placed on either side and the edge that would send it straight back:
 *  without them a hand-over lands ON the divider edge and returns on the next tick. */
#define EDGE_MARGIN 6
/** Windows the guard remembers, and how many times it will move any one of them. */
#define GUARD_TRACK 32
/* A window is moved back at most once per this long (user 2026-11-04: Steam dragged back over the picture
 * stayed there after the old cap of 3 moves). Every time, but never fast enough for a window that
 * insists to start a flickering fight. */
#define GUARD_RETRY_MS 1500
/* Moves of one window made at once before GUARD_RETRY_MS applies (see guard_consider_ex). */
#define GUARD_FREE_MOVES 3
/* How often the watcher looks again at the windows it has moved (guard_sweep). */
#define GUARD_SWEEP_MS 200
#define GUARD_OVERLAP_MIN 16
/** How often the watcher looks for a cursor that is on the virtual display without a hand-over. */
#define ADOPT_POLL_MS 50
/** After a search that found no virtual display, how long before the next full search: the
 *  classification costs a few display-config queries, too many to repeat twenty times a second. */
#define SEARCH_BACKOFF_MS 1000
/** Log the first few hand-overs of a session in full, then every Nth: enough to see it work
 *  without a line per crossing for a whole evening. */
#define LOG_FIRST 20
#define LOG_EVERY 100
/** While handed over, how often check_cursor re-reads the virtual display's rectangle (a game may change
 *  its mode): one EnumDisplayMonitors when the display is pinned. */
#define VDD_REFRESH_MS 250

static struct {
    bool armed;
    bool vertical;                 /* panes side by side */
    bool local_first;              /* this PC's pane is the left/top one */
    bool mouse_here;               /* the mouse is ASSIGNED to this PC: no way back at the divider */
    bool game_lock;                /* coop_game_mouse_lock: a game confining the cursor keeps it (see check_cursor) */
    int keys;                      /* LOCAL_INPUT_KEYS_* */
    volatile LONG give_back;       /* the taskbar menu asked for the mouse back (local_input_give_back) */
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
    wchar_t pin_dev[CCHDEVICENAME];/* local_input_pin_display: THE display, named by who made it */
    HWND game;                     /* the window given the keyboard at the last hand-over */
    struct { HWND hwnd; int moves; DWORD last; } guard[GUARD_TRACK];   /* watcher thread only */
    int guard_next;
    unsigned guard_moved;
    CRITICAL_SECTION vdd_lock;     /* find_virtual_display runs on the UI thread and the watcher */
    bool vdd_lock_ready;
    CRITICAL_SECTION cursor_lock;  /* a hand-over's cursor move vs the watcher's "slid" put-back and fence */
    DWORD last_miss;               /* tick of the last search that found no virtual display */
    unsigned handoffs, returns;
    bool had_joy_hint;
    char joy_hint[16];
    bool had_click_hint;
    char click_hint[16];
    bool had_kb_grab;              /* the client window's SDL keyboard grab before arming */
    bool had_on_top;               /* the client window was always-on-top before arming */
} li;

/* The client's own menu is up over the session (local_input_set_ui_overlay). */
static volatile LONG ui_overlay;
static DWORD watch_tid;   /* the watcher thread: the leave key is posted to it from the keyboard hook */

/* ---- Finding the virtual display --------------------------------------------------------- */

/* The SAME three signals dispwake uses (xlink/dispwake/dispwake.cpp pathIsVirtual), any one of
 * which suffices: an indirect output technology, the VDD driver's EDID vendor ("SMK", the
 * SudoMaker VDD Apollo ships -- it reports its technology as HDMI, so technology alone misses
 * it), or one of Apollo's VDD names. PLUS "SDD": Umbra's own bundled virtual display driver
 * (Umbra display_helper_v2/snapshot_codec.cpp: "SDD = bundled Sunshine virtual display driver").
 * MEASURED 2026-11-04 on SKYNET (Umbra Vibepollo): maker SDD, technology DisplayPort (10), and named
 * after the CLIENT ("This pc") -- none of the Apollo signals matched, so host-and-client co-op logged
 * "NO virtual display found" on every move and never handed the mouse over. */
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
        if (strcmp(vendor, "SMK") == 0 || strcmp(vendor, "SDD") == 0) {
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
    /* A display the client made itself is named, not searched for: whether it would pass path_is_virtual
     * is beside the point, and another virtual display on this PC must not be taken for it. */
    if (li.pin_dev[0] != L'\0') {
        return monitor_rect(li.pin_dev, out);
    }
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

/** Which half of the client window's client area a screen point is over: 0 = neither (or unknown),
 *  1 = this PC's pane, 2 = the stream's pane. Split at the middle, as coop_pointer.c divider() does. */
static int client_half_at(POINT p) {
    RECT c;
    POINT o = {0, 0};
    if (li.client == NULL || !GetClientRect(li.client, &c) || !ClientToScreen(li.client, &o) || c.right <= 0 ||
        c.bottom <= 0) {
        return 0;
    }
    const LONG x = p.x - o.x, y = p.y - o.y;
    if (x < 0 || y < 0 || x >= c.right || y >= c.bottom) {
        return 0;
    }
    const bool first = li.vertical ? x < c.right / 2 : y < c.bottom / 2;
    return first == li.local_first ? 1 : 2;
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
    /* ALREADY OVER THE STREAM'S PANE: leave it there. MEASURED 2026-11-11 (back #5 from (2678,328), 19 ms after a
     * hand-over): the hand-over's SetCursorPos did not hold and the cursor carried on across the stream's pane.
     * Moving it to the divider would be a visible jump backwards; where it is, is where the user is pointing. */
    const bool stay = !li.mouse_here && client_half_at(at) == 2;
    if (stay) {
        to = at;
    } else if (li.mouse_here) {
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
    if (!stay) {
        to.x += origin.x;
        to.y += origin.y;
    }

    /* CURSOR FIRST, then the keyboard. SDL answers the client gaining the focus by reading the
     * cursor position (GetCursorPos) and sending it as a motion -- read while the cursor was still
     * on the virtual display, that became a jump to the window's corner. A game that confines
     * the cursor may clip it back before it loses the focus, so the move is checked and repeated
     * once the keyboard has changed hands. A keyboard ASSIGNED to this PC stays with its game. */
    ClipCursor(NULL);
    SetCursorPos(to.x, to.y);
    /* The keyboard reaches the stream through syskey_hook while the pointer is over its half; this PC's game keeps
     * the foreground (and reads its pads). Only the client's own menu takes the foreground. */
    const bool keys = li.keys == LOCAL_INPUT_KEYS_HERE || InterlockedCompareExchange(&ui_overlay, 0, 0) == 0
                      ? true : take_foreground(li.client);
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

/* 1 while a drag on this PC's display is held inside it (drag_clamp): check_cursor then leaves the divider alone. */
static volatile LONG drag_clamped;

static void check_cursor(void) {
    POINT p;
    if (!GetCursorPos(&p)) {
        return;   /* the secure desktop (UAC, Ctrl+Alt+Del) owns the cursor for now */
    }
    /* THE DISPLAY CAN CHANGE UNDER A HAND-OVER. MEASURED 2026-11-11: a game set this PC's display to 1280x720
     * while the cursor was on it, and the rectangle cached at the hand-over walled the cursor at (640,720) over
     * a hundred times in six seconds. Re-read every VDD_REFRESH_MS, and at once before acting on an "out". */
    static DWORD refreshed = 0;
    const DWORD now = GetTickCount();
    bool out = p.x < li.vdd.left || p.x >= li.vdd.right || p.y < li.vdd.top || p.y >= li.vdd.bottom;
    if (out || now - refreshed >= VDD_REFRESH_MS) {
        RECT nv;
        refreshed = now;
        if (find_virtual_display(&nv) && !EqualRect(&nv, &li.vdd)) {
            local_host_log("input: this PC's display moved/resized while handed over: (%ld,%ld)-(%ld,%ld) -> "
                           "(%ld,%ld)-(%ld,%ld)", (long) li.vdd.left, (long) li.vdd.top, (long) li.vdd.right,
                           (long) li.vdd.bottom, (long) nv.left, (long) nv.top, (long) nv.right, (long) nv.bottom);
            li.vdd = nv;
        }
    }
    const RECT v = li.vdd;
    out = p.x < v.left || p.x >= v.right || p.y < v.top || p.y >= v.bottom;
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
    /* ONLY THE DIVIDER LEADS BACK (user 2026-11-04: "the far left and far right edges are still warping
     * back around"). Umbra puts its virtual display BESIDE the client's monitor -- measured on SKYNET:
     * monitor (-2560,0)-(0,1440), virtual display (0,0)-(1280,1440) -- so with this PC's pane on the LEFT
     * the display's OUTER (left) edge touches the monitor, and a cursor pushed against it slid off and
     * was "returned" to the divider in the middle of the screen. `divider` already covers crossing the
     * divider edge itself; leaving by any other side puts the cursor back on that edge: a wall, as the
     * edge of a real monitor is. (The Apollo-era layout touched only at a corner, which hid this.) */
    /* A GAME CONFINING THE CURSOR (ClipCursor) must not trap it on this PC's pane (user 2026-11-13: RDR2 "is holding
     * the cursor from moving across the planes until alt tab is clicked" -- not wanted by default, wanted as an
     * option). Reaching the confinement's edge on the divider's side counts as reaching the divider. With the
     * option (coop_game_mouse_lock) the game keeps the cursor, as it would on a PC of its own. */
    RECT clip;
    if (!divider && !li.game_lock && InterlockedCompareExchange(&drag_clamped, 0, 0) == 0 && GetClipCursor(&clip) &&
        (clip.right - clip.left < GetSystemMetrics(SM_CXVIRTUALSCREEN) ||
         clip.bottom - clip.top < GetSystemMetrics(SM_CYVIRTUALSCREEN)) && PtInRect(&clip, p)) {
        bool at;
        if (li.vertical) {
            at = li.local_first ? p.x >= clip.right - 1 : p.x <= clip.left;
        } else {
            at = li.local_first ? p.y >= clip.bottom - 1 : p.y <= clip.top;
        }
        if (at) {
            static unsigned clipped = 0;
            if (log_this(++clipped)) {
                local_host_log("input: a program confines the cursor to (%ld,%ld)-(%ld,%ld); its edge facing the "
                               "divider counts as the divider (#%u)", (long) clip.left, (long) clip.top,
                               (long) clip.right, (long) clip.bottom, clipped);
            }
            divider = true;
        }
    }
    if (divider && InterlockedCompareExchange(&drag_clamped, 0, 0) != 0) {
        return;   /* a window is being dragged: the cursor is held on this display until it is let go */
    }
    if (divider) {
        come_back(p, "divider");
    } else if (out) {
        static unsigned walls = 0;
        static DWORD wall_logged = 0;
        SetCursorPos(p.x < v.left ? v.left : (p.x >= v.right ? v.right - 1 : p.x),
                     p.y < v.top ? v.top : (p.y >= v.bottom ? v.bottom - 1 : p.y));
        /* At most a line a second: pushing against a wall is held there, not a crossing. */
        if (log_this(++walls) && (walls == 1 || now - wall_logged >= 1000)) {
            wall_logged = now;
            local_host_log("input: cursor pushed off an outer edge of this PC's pane at (%ld,%ld) -- held at the "
                           "edge (#%u)", (long) p.x, (long) p.y, walls);
        }
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

/** This window's slot (moves made, time of the last); claims a slot for a new one. */
static int guard_slot(HWND h) {
    for (int i = 0; i < GUARD_TRACK; i++) {
        if (li.guard[i].hwnd == h) {
            return i;
        }
    }
    const int i = li.guard_next;
    li.guard_next = (li.guard_next + 1) % GUARD_TRACK;
    li.guard[i].hwnd = h;
    li.guard[i].moves = 0;
    li.guard[i].last = GetTickCount() - GUARD_RETRY_MS;
    return i;
}

/* @p early: the window is not shown yet (created or positioned while hidden) -- moving it now means it first
 * appears in this PC's pane. MEASURED 2026-11-13 11:44 on SKYNET: RDR2's borderless window opened centred on the
 * client's monitor and was moved only once shown, so it sat over both panes for a second or two. */
static void guard_consider_ex(HWND h, const char *event, bool early) {
    if (h == NULL || (!early && !IsWindowVisible(h)) || IsIconic(h) || GetAncestor(h, GA_ROOT) != h) {
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
    /* ANY REAL OVERLAP with the client's monitor counts (user 2026-11-04: Steam dragged left from this PC's
     * pane hung over the other pane and stayed -- most of it was still on the virtual display, so "which
     * monitor holds most of it" said not ours). GUARD_OVERLAP_MIN keeps out the few pixels of shadow a
     * maximized window on the virtual display reaches across the shared edge. */
    HMONITOR home = MonitorFromWindow(li.client, MONITOR_DEFAULTTONULL);
    RECT r;
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (home == NULL || !GetWindowRect(h, &r) || !GetMonitorInfoW(home, &mi)) {
        return;
    }
    RECT overlap;
    if (!IntersectRect(&overlap, &r, &mi.rcMonitor) || overlap.right - overlap.left < GUARD_OVERLAP_MIN ||
        overlap.bottom - overlap.top < GUARD_OVERLAP_MIN) {
        return;
    }
    /* A normal window (it has a caption), or a borderless top-level one -- a game. Not the popups that belong
     * where they appeared: those are OWNED by another window (menus, pickers, dialogs' drop-downs) or never take
     * the focus (toasts). WAS "a popup covering 60 % of the monitor": MEASURED 2026-11-13 on SKYNET, RDR2's
     * borderless 1280x720 window (25 % of the 2560x1440 monitor) opened centred over both panes and was never
     * even considered (no guard line). A tiny floor keeps out stray 1-pixel helper windows. */
    const bool captioned = (style & WS_CAPTION) == WS_CAPTION;
    const bool game_popup = (style & WS_POPUP) && GetWindow(h, GW_OWNER) == NULL && !(ex & WS_EX_NOACTIVATE) &&
                            r.right - r.left >= 200 && r.bottom - r.top >= 150;
    if (!captioned && !game_popup) {
        return;
    }
    const int slot = guard_slot(h);
    int *moves = &li.guard[slot].moves;
    /* THE FIRST MOVES ARE IMMEDIATE (user 2026-11-13 16:22: RDR2 sat centred over both panes for ~5 s). MEASURED in
     * that session: the guard moved RDR2's window as it was created, RDR2 re-centred it at once, and the 1.5 s
     * limit dropped that event; the next one came 34 s later. The limit is for a window that fights back
     * forever, so it starts after GUARD_FREE_MOVES, and guard_sweep comes back to any window it held off. */
    if (*moves >= GUARD_FREE_MOVES && GetTickCount() - li.guard[slot].last < GUARD_RETRY_MS) {
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
    /* A BORDERLESS GAME THAT FILLS THE AREA GETS THE WHOLE DISPLAY (user 2026-11-13, photo: SKYNET's taskbar showed
     * under RDR2). MEASURED: RDR2's full-monitor window (2560,-1440)-(5120,0) was fitted into the work area,
     * 2560x672 of the 2560x720 display, leaving the taskbar's 48 px uncovered -- and RDR2 then confined the cursor
     * to those 672 rows, so it could never reach the divider row (719). Captioned windows still respect the
     * taskbar. */
    const bool fills = !captioned && r.right - r.left >= vmi.rcWork.right - vmi.rcWork.left &&
                       r.bottom - r.top >= vmi.rcWork.bottom - vmi.rcWork.top;
    const RECT wa = fills ? vmi.rcMonitor : vmi.rcWork;
    const bool zoomed = IsZoomed(h) != 0;
    /* Filling: the whole display, not only on it (16:22: RDR2 kept its own 2560x672 at (0,0), taskbar beneath). */
    const LONG ww = fills || r.right - r.left > wa.right - wa.left ? wa.right - wa.left : r.right - r.left;
    const LONG wh = fills || r.bottom - r.top > wa.bottom - wa.top ? wa.bottom - wa.top : r.bottom - r.top;
    /* Kept where it was as far as the display allows: a window dragged half over the edge comes back just
     * inside it, and one wholly on the client's monitor lands at the near edge. */
    const LONG x = r.left < wa.left ? wa.left : (r.left + ww > wa.right ? wa.right - ww : r.left);
    const LONG y = r.top < wa.top ? wa.top : (r.top + wh > wa.bottom ? wa.bottom - wh : r.top);
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
    li.guard[slot].last = GetTickCount();
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

static void guard_consider(HWND h, const char *event) {
    guard_consider_ex(h, event, false);
}

/* The window the user is dragging right now: left alone until they let go (EVENT_SYSTEM_MOVESIZEEND). */
static HWND guard_dragging;

/* LOOK AGAIN at every window the guard has dealt with: a move held off by GUARD_RETRY_MS, or a window that moved
 * itself without an event we act on, cannot stay on the client's monitor longer than this. A window no longer on
 * it is left alone by guard_consider_ex's own checks. Watcher thread. */
static void guard_sweep(void) {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last < GUARD_SWEEP_MS) {
        return;
    }
    last = now;
    for (int i = 0; i < GUARD_TRACK; i++) {
        const HWND h = li.guard[i].hwnd;
        if (h == NULL) {
            continue;
        }
        if (!IsWindow(h)) {
            li.guard[i].hwnd = NULL;
            continue;
        }
        if (h == guard_dragging) {
            continue;
        }
        guard_consider_ex(h, "is still", !IsWindowVisible(h));
    }
    /* THE CLIENT STAYS ON TOP (set at arm; see local_input_arm). MEASURED 2026-11-13 18:57 on SKYNET: mid-session the
     * client's extended style read 0x10 -- no WS_EX_TOPMOST -- and RDR2, re-centring itself over both panes while it
     * started, was drawn above the session for ~8 s (screen recording) instead of behind it. Who clears it is not
     * known yet; it is put back here, and the first loss is logged. Async: the window is the UI thread's. */
    /* THE CLIENT IS NEVER LEFT MINIMIZED (user 2026-11-13 22:55: "I managed to still alt tab out of eclipse").
     * MEASURED: after Alt+Tab over this PC's half the client sat at (-32000,-32000), iconic -- SDL minimizes a
     * fullscreen window that loses the focus (SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, turned off at arm); this is
     * the backstop for anything else that minimizes it. Restored WITHOUT taking the focus. */
    static unsigned restored = 0;
    if (li.client != NULL && IsIconic(li.client)) {
        if (log_this(++restored)) {
            HWND fg = GetForegroundWindow();
            char who[160] = "";
            if (fg != NULL) {
                describe(fg, who, sizeof(who));
            }
            local_host_log("input: the client was minimized (#%u, foreground %s) -- restored", restored, who);
        }
        ShowWindowAsync(li.client, SW_SHOWNOACTIVATE);
    }
    static unsigned lost = 0;
    if (li.client != NULL && !IsIconic(li.client) &&
        (GetWindowLongW(li.client, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
        if (log_this(++lost)) {
            HWND fg = GetForegroundWindow();
            char who[160] = "";
            if (fg != NULL) {
                describe(fg, who, sizeof(who));
            }
            local_host_log("input: the client was no longer always-on-top (#%u, foreground %s) -- put back", lost, who);
        }
        SetWindowPos(li.client, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
    }
}


/* A DRAGGED WINDOW CANNOT ENTER THE CLIENT'S MONITOR (user 2026-11-13: windows dragged from this PC's half over to
 * the stream's half snapped back -- "I would prefer if they can't enter at all"). While another program's window
 * is moved or sized on this PC's display, the cursor is confined so that the window's edges stay on that display:
 * for a move, the cursor's offset into the window is kept off each edge; for a size (grabbed at a border), the
 * cursor stays on the display. Released when the drag ends. */
static void drag_clamp(HWND h) {
    if (InterlockedCompareExchange(&li.handed, 0, 0) == 0 || h == NULL) {
        return;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    RECT r, v = li.vdd;
    POINT p;
    if (pid == GetCurrentProcessId() || !GetWindowRect(h, &r) || !GetCursorPos(&p) || v.right <= v.left) {
        return;
    }
    RECT clip = v;
    const LONG border = 10;
    const bool sizing = p.x < r.left + border || p.x >= r.right - border || p.y >= r.bottom - border;
    if (!sizing) {
        clip.left = v.left + (p.x - r.left);
        clip.top = v.top + (p.y - r.top);
        clip.right = v.right - (r.right - p.x);
        clip.bottom = v.bottom - (r.bottom - p.y);
        if (clip.right <= clip.left) { clip.left = v.left; clip.right = v.right; }   /* wider than the display */
        if (clip.bottom <= clip.top) { clip.top = v.top; clip.bottom = v.bottom; }
    }
    ClipCursor(&clip);
    InterlockedExchange(&drag_clamped, 1);
}

static void drag_release(void) {
    if (InterlockedExchange(&drag_clamped, 0) != 0) {
        ClipCursor(NULL);
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
    if (event == EVENT_SYSTEM_MOVESIZESTART) {
        guard_dragging = hwnd;
        drag_clamp(hwnd);
        return;
    }
    if (event == EVENT_SYSTEM_MOVESIZEEND) {
        guard_dragging = NULL;
        drag_release();
    } else if (event == EVENT_OBJECT_LOCATIONCHANGE && hwnd == guard_dragging) {
        return;
    }
    if ((event == EVENT_OBJECT_LOCATIONCHANGE || event == EVENT_OBJECT_CREATE) && hwnd != NULL &&
        !IsWindowVisible(hwnd)) {
        guard_consider_ex(hwnd, "is being placed (not shown yet)", true);
        return;
    }
    guard_consider(hwnd, event == EVENT_SYSTEM_FOREGROUND ? "came to the front"
                         : event == EVENT_SYSTEM_MOVESIZEEND ? "was dragged"
                         : event == EVENT_OBJECT_LOCATIONCHANGE ? "moved itself" : "opened");
}

/* THE FENCE (user 2026-11-04: pushing the cursor off the far right of the other PC's pane still jumped to
 * this PC's pane and "glitched"). The poll below caught it only after up to 50 ms on the virtual display --
 * long enough to be seen in this PC's pane. So while no hand-over is in force the cursor is confined to the
 * client window: it cannot reach the virtual display at all. A hand-over lifts it (local_input_hand_off, under
 * `handing`); the return (come_back) re-arms it on the next poll. Only this thread sets it, and it releases only
 * its own. It used to need the client to be the FOREGROUND window as well, and on 2026-11-10 that let the cursor
 * through ~50 times in one session: Windows often refuses the client the foreground on a return ("keyboard
 * REFUSED" in the log), so the fence was off and the far right of the other pane led onto the virtual display.
 * Re-applied on every poll, so a ClipCursor reset by Windows (a desktop switch, another program) is undone. */
static bool fence_on;

static void fence_update(bool want) {
    RECT cw;
    if (want && li.client != NULL && !IsIconic(li.client) && GetWindowRect(li.client, &cw)) {
        RECT cur;
        if (!fence_on || !GetClipCursor(&cur) || !EqualRect(&cur, &cw)) {
            ClipCursor(&cw);
            fence_on = true;
        }
    } else if (fence_on) {
        ClipCursor(NULL);
        fence_on = false;
    }
}

/** A cursor on the virtual display with no hand-over in force: adopt it (see the header). */
static void adopt_stranded_cursor(void) {
    static unsigned adopted = 0;
    POINT p;
    RECT v;
    /* MEASURED K2 22:15:43.492: the watcher's poll landed between a hand-over's SetCursorPos and
     * its `handed`, and "adopted" a cursor the UI thread was placing. */
    /* Where the cursor was at the previous poll: on the client's window, or not. */
    static bool was_on_client = false;
    if (InterlockedCompareExchange(&li.handing, 0, 0) != 0 || !GetCursorPos(&p) || !find_virtual_display(&v)) {
        return;
    }
    RECT cw;
    const bool have_client = li.client != NULL && GetWindowRect(li.client, &cw);
    if (p.x < v.left || p.x >= v.right || p.y < v.top || p.y >= v.bottom) {
        was_on_client = have_client && PtInRect(&cw, p);
        return;
    }
    /* SLID IN, NOT STRANDED (user 2026-11-04: "going to the far right comes in from the far left"). The
     * client's monitor and the virtual display share an edge (see check_cursor), so pushing the cursor
     * off the client's outer edge carried it onto the virtual display, where it was adopted -- appearing
     * at the far side of this PC's pane. A cursor that was on the client a poll ago got here that way:
     * put it back on the client's edge. Only a cursor that was NOT (Windows moving it when the virtual
     * display appeared, the case below exists for) is adopted. */
    if (was_on_client && have_client) {
        static unsigned slid = 0;
        /* Under cursor_lock and re-checked: a hand-over that began since the read above put the cursor on the
         * display ON PURPOSE and must not be undone (2026-11-11: that session's one "slid" line acted on a
         * hand-over's cursor). */
        EnterCriticalSection(&li.cursor_lock);
        const bool still = InterlockedCompareExchange(&li.handing, 0, 0) == 0 &&
                           InterlockedCompareExchange(&li.handed, 0, 0) == 0;
        if (still) {
            SetCursorPos(p.x < cw.left ? cw.left : (p.x >= cw.right ? cw.right - 1 : p.x),
                         p.y < cw.top ? cw.top : (p.y >= cw.bottom ? cw.bottom - 1 : p.y));
        }
        LeaveCriticalSection(&li.cursor_lock);
        if (still && log_this(++slid)) {
            local_host_log("input: cursor slid off the client's edge onto the virtual display at (%ld,%ld) -- put "
                           "back on the client's edge (#%u)", (long) p.x, (long) p.y, slid);
        }
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

/* SYSTEM SHORTCUTS STAY IN THE SESSION (user 2026-11-03: "All inputs need to go into that session (since things like
 * alt tab wouldn't work we need fallbacks...)"). While the CLIENT has the focus, SDL's keyboard grab (armed in
 * local_input_arm) captures them and the stream gets them. While THIS PC's GAME has it, nothing of ours sees them
 * and this PC's shell would act: the Windows key opened the Start menu, Alt+Tab could switch to anything on this
 * PC. So this hook swallows the Windows keys, Alt+Tab, Alt+Esc and Ctrl+Esc whenever the foreground window is not
 * the client. The exits stay: Ctrl+Alt+Shift+Z and Ctrl+Alt+Shift+Q are hotkeys, controllers' combos read pads. */
/* THIS PC'S GAME KEEPS THE FOREGROUND; THE KEYBOARD FOLLOWS THE POINTER THROUGH THIS HOOK (user 2026-11-13, option 2).
 * Measured with RDR2: SKYNET's controller only worked while the mouse was over SKYNET's half -- every return made the
 * client the foreground window, and a game in the background stops reading pads. And Alt+Tab over the stream's half
 * still switched windows on this PC (syskey_capture off: SDL never grabbed it). So the client no longer takes the
 * foreground on a return (nor on a click: WM_MOUSEACTIVATE answers MA_NOACTIVATE); while the pointer is over the
 * stream's half, this hook takes EVERY key -- the Windows key and Alt+Tab included -- and sends it to the stream. A
 * release goes where its press went. Ctrl+Alt+Shift+Q still leaves: the hook acts on it itself, since the forwarded
 * modifiers never reach Windows' hotkey. Not while the client's own menu is up (local_input_set_ui_overlay). */
static LRESULT CALLBACK syskey_hook_local(int code, WPARAM wp, LPARAM lp);
static unsigned char fwd_down[256];
static char fwd_mods;
static WNDPROC client_old_proc;

static bool keys_to_stream_now(void) {
    if (InterlockedCompareExchange(&ui_overlay, 0, 0) != 0 || li.keys == LOCAL_INPUT_KEYS_HERE) {
        return false;
    }
    if (li.keys == LOCAL_INPUT_KEYS_ELSEWHERE) {
        return true;
    }
    POINT p;
    return InterlockedCompareExchange(&li.handed, 0, 0) == 0 && GetCursorPos(&p) && client_half_at(p) == 2;
}

static char modifier_bit(DWORD vk) {
    switch (vk) {
        case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT: return MODIFIER_SHIFT;
        case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: return MODIFIER_CTRL;
        case VK_LMENU: case VK_RMENU: case VK_MENU: return MODIFIER_ALT;
        case VK_LWIN: case VK_RWIN: return MODIFIER_META;
        default: return 0;
    }
}

static int fwd_last_rc;   /* LiSendKeyboardEvent's last result: 0 = queued for the stream */

static void forward_key(DWORD vk, bool down) {
    const char bit = modifier_bit(vk);
    if (bit != 0) {
        fwd_mods = down ? (char) (fwd_mods | bit) : (char) (fwd_mods & ~bit);
    }
    fwd_down[vk & 0xFF] = down ? 1 : 0;
    fwd_last_rc = LiSendKeyboardEvent((short) (0x8000 | (vk & 0xFF)), down ? KEY_ACTION_DOWN : KEY_ACTION_UP, fwd_mods);
}

/* Every key the stream still holds gets its release (pointer moved away, session ending). */
static void release_forwarded(void) {
    for (int vk = 0; vk < 256; vk++) {
        if (fwd_down[vk]) {
            forward_key((DWORD) vk, false);
        }
    }
    fwd_mods = 0;
}

/* WHO MINIMIZES THE CLIENT (2026-11-13 23:15, measured on SKYNET: after the client had the focus and Alt+Tab took it
 * away, the client was minimized -- with SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS "0", and the client is FULLSCREEN_DESKTOP,
 * which SDL does not minimize anyway). The stack of the call that delivers the minimize, as module+offset, says
 * whether it is SDL, this program or Windows. */
static void log_minimize_stack(const char *what) {
    void *frames[24];
    const USHORT n = CaptureStackBackTrace(1, 24, frames, NULL);
    char line[1400];
    size_t used = 0;
    line[0] = '\0';
    for (USHORT i = 0; i < n && used + 64 < sizeof(line); i++) {
        HMODULE mod = NULL;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR) frames[i], &mod) && mod != NULL) {
            GetModuleFileNameA(mod, path, sizeof(path));
        }
        const char *base = strrchr(path, '\\');
        base = base != NULL ? base + 1 : path;
        used += (size_t) snprintf(line + used, sizeof(line) - used, " %s+0x%llx", base,
                                  (unsigned long long) ((const char *) frames[i] - (const char *) mod));
    }
    HWND fg = GetForegroundWindow();
    char who[160] = "";
    if (fg != NULL) {
        describe(fg, who, sizeof(who));
    }
    local_host_log("input: client %s (foreground %s); stack:%s", what, who, line);
}

static LRESULT CALLBACK client_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (li.armed && msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_MINIMIZE) {
        log_minimize_stack("asked to minimize (WM_SYSCOMMAND) -- refused during the session");
        return 0;
    }
    if (li.armed && msg == WM_SIZE && wp == SIZE_MINIMIZED) {
        log_minimize_stack("minimized (WM_SIZE)");
    }
    /* ONE POINTER OVER THE STREAM (user 2026-11-13, photo: Vega's game drew its pointer and this PC's arrow showed
     * beside it). The host draws its own cursor into the picture; this PC's is hidden over the stream's half, as
     * a streaming client does. Over this PC's half the cursor is on the virtual display, not here. */
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && li.armed && InterlockedCompareExchange(&ui_overlay, 0, 0) == 0) {
        POINT p;
        if (GetCursorPos(&p) && client_half_at(p) == 2) {
            SetCursor(NULL);
            return TRUE;
        }
    }
    if (msg == WM_MOUSEACTIVATE && li.armed && li.keys != LOCAL_INPUT_KEYS_HERE &&
        InterlockedCompareExchange(&ui_overlay, 0, 0) == 0) {
        return MA_NOACTIVATE;   /* the click still arrives (SDL click-through); this PC's game keeps the focus */
    }
    return CallWindowProcW(client_old_proc, h, msg, wp, lp);
}

void local_input_set_ui_overlay(bool showing) {
    InterlockedExchange(&ui_overlay, showing ? 1 : 0);
    if (showing && li.armed && li.client != NULL) {
        take_foreground(li.client);   /* the client's own menu takes the keyboard the ordinary way */
    }
}

/* EVIDENCE for where a key went (user 2026-11-13: Alt+Tab over the stream's half "did nothing" on either PC, and
 * nothing in the log could say whether the hook forwarded it). Every press of a system key (Alt, Tab, Windows,
 * Escape) is logged with the decision's inputs and the send result; other keys only the first few per session. */
static int key_log_budget = 8;
/* TEST ONLY: ProgramData/Eclipse/test-forward-injected present at arm -> injected keys take the physical keys'
 * path, so a remote test can drive it with SendInput. Off otherwise: Steam Input's keyboard bindings and the
 * on-screen keyboard inject keys meant for THIS PC's game. */
static bool forward_injected;
static unsigned char local_down[256];   /* keys this PC kept: their autorepeat is not logged again */

/* THE HOOK NEVER WRITES THE LOG ITSELF: a file write inside a low-level hook can outlast Windows' hook timeout, and
 * Windows 7+ then REMOVES the hook silently (see key_thread). The hook fills this ring; the watcher writes it. */
#define KEY_LOG_RING 64
static struct key_log_entry {
    DWORD vk;
    bool forwarded;
    LONG overlay, handed;
    int half, rc;
    POINT at;
    bool client_front;
} key_log_ring[KEY_LOG_RING];
static volatile LONG key_log_head, key_log_tail;   /* head: hook thread writes; tail: watcher reads */
static volatile LONG keys_seen;                    /* every key event the hook was called for */

static void log_key(DWORD vk, bool forwarded) {
    const bool syskey = vk == VK_TAB || vk == VK_LWIN || vk == VK_RWIN || vk == VK_ESCAPE || vk == VK_LMENU ||
                        vk == VK_RMENU;
    if (!syskey && key_log_budget <= 0) {
        return;
    }
    const LONG head = key_log_head;
    if (head - InterlockedCompareExchange(&key_log_tail, 0, 0) >= KEY_LOG_RING) {
        return;   /* the watcher is behind: drop the line, never wait */
    }
    if (!syskey) {
        key_log_budget--;
    }
    struct key_log_entry *e = &key_log_ring[head % KEY_LOG_RING];
    e->vk = vk;
    e->forwarded = forwarded;
    e->overlay = InterlockedCompareExchange(&ui_overlay, 0, 0);
    e->handed = InterlockedCompareExchange(&li.handed, 0, 0);
    e->at.x = e->at.y = 0;
    GetCursorPos(&e->at);
    e->half = client_half_at(e->at);
    e->rc = forwarded ? fwd_last_rc : 0;
    e->client_front = GetForegroundWindow() == li.client;
    InterlockedExchange(&key_log_head, head + 1);
}

/* Watcher thread: write what the hook queued. */
static void key_log_drain(void) {
    while (InterlockedCompareExchange(&key_log_tail, 0, 0) != InterlockedCompareExchange(&key_log_head, 0, 0)) {
        const LONG tail = key_log_tail;
        const struct key_log_entry e = key_log_ring[tail % KEY_LOG_RING];
        InterlockedExchange(&key_log_tail, tail + 1);
        local_host_log("input: key 0x%02lx down -> %s (overlay %ld, keys %d, handed %ld, half %d at (%ld,%ld), "
                       "foreground %s, send %d)", (unsigned long) e.vk, e.forwarded ? "the stream" : "this PC",
                       (long) e.overlay, li.keys, (long) e.handed, e.half, (long) e.at.x, (long) e.at.y,
                       e.client_front ? "client" : "other", e.rc);
    }
}

static LRESULT CALLBACK syskey_hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && li.client != NULL) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *) lp;
        const bool up = wp == WM_KEYUP || wp == WM_SYSKEYUP;
        const DWORD vk = k->vkCode & 0xFF;
        const bool repeat = !up && fwd_down[vk];
        InterlockedIncrement(&keys_seen);
        if (!(k->flags & LLKHF_INJECTED) || forward_injected) {
            if (up && fwd_down[vk]) {
                forward_key(vk, false);
                return 1;
            }
            if (!up && keys_to_stream_now()) {
                if (vk == 'Q' && (fwd_mods & (MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT)) ==
                                     (MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT)) {
                    release_forwarded();
                    PostThreadMessageW(watch_tid, WM_HOTKEY, LEAVE_HOTKEY_ID, 0);   /* the watcher acts on it */
                    return 1;
                }
                forward_key(vk, true);
                if (!repeat) {
                    log_key(vk, true);
                }
                return 1;
            }
            if (!up && !local_down[vk]) {
                log_key(vk, false);
            }
            local_down[vk] = up ? 0 : 1;
        }
    }
    return syskey_hook_local(code, wp, lp);
}

static LRESULT CALLBACK syskey_hook_local(int code, WPARAM wp, LPARAM lp) {
    /* Whichever window has the focus -- the client too (user 2026-11-13: after a click on the client, Alt+Tab left the
     * session for this PC's desktop). Nothing on this PC is meant to be switched to during the session. */
    if (code == HC_ACTION && li.client != NULL && li.armed) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *) lp;
        const bool alt = (k->flags & LLKHF_ALTDOWN) != 0;
        const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        /* ALT+TAB WORKS INSIDE THIS PC'S HALF (user 2026-11-13: "Alt tab is working for Vega but just does nothing
         * for skynet"). It was blocked because it used to leave the session; with the client kept on top (guard_sweep)
         * and this PC's windows kept on its display, switching among them stays in this PC's half. Only while the
         * pointer is handed to that half -- over the client (its menu, the stream) it is still swallowed. */
        const bool here = InterlockedCompareExchange(&li.handed, 0, 0) != 0;
        const bool swallow = k->vkCode == VK_LWIN || k->vkCode == VK_RWIN ||
                             (k->vkCode == VK_TAB && alt && !here) || (k->vkCode == VK_ESCAPE && (alt || ctrl));
        if (swallow) {
            return 1;
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

/* THE KEYBOARD HOOK HAS A THREAD OF ITS OWN (user 2026-11-13 16:28: with RDR2 running, Alt+Tab over the stream's
 * half went to this PC and left the session). MEASURED in that session's log: not one "input: key" line in 130,
 * from a build that logs every system key -- the hook was never called. It ran on the watcher thread, whose
 * queue also carries every window event of every program (EVENT_OBJECT_LOCATIONCHANGE fires on any move), and
 * Windows 7+ silently REMOVES a low-level hook that misses its timeout. Here the thread does nothing but answer
 * the hook, and re-installs it every KEY_REHOOK_MS so that even a removal costs at most that long. */
#define KEY_REHOOK_MS 5000
static DWORD key_tid;
static HANDLE key_thread_handle;
static volatile LONG key_rehooks;

static DWORD WINAPI key_thread(LPVOID arg) {
    (void) arg;
    MSG m;
    PeekMessageW(&m, NULL, WM_USER, WM_USER, PM_NOREMOVE);   /* the queue, so WM_QUIT can be posted to it */
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, syskey_hook, GetModuleHandleW(NULL), 0);
    if (hook == NULL) {
        local_host_log("input: keyboard hook NOT installed (%lu) -- the Windows key and Alt+Tab reach this PC",
                       GetLastError());
    }
    DWORD hooked_at = GetTickCount();
    for (;;) {
        MsgWaitForMultipleObjects(0, NULL, FALSE, 500, QS_ALLINPUT);
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) {
                goto done;
            }
        }
        if (InterlockedCompareExchange(&li.quit, 0, 0) != 0) {
            break;
        }
        if (GetTickCount() - hooked_at >= KEY_REHOOK_MS) {
            /* The new one first: no moment without a hook. */
            HHOOK fresh = SetWindowsHookExW(WH_KEYBOARD_LL, syskey_hook, GetModuleHandleW(NULL), 0);
            if (fresh != NULL) {
                if (hook != NULL) {
                    UnhookWindowsHookEx(hook);
                }
                hook = fresh;
                InterlockedIncrement(&key_rehooks);
            }
            hooked_at = GetTickCount();
        }
    }
done:
    if (hook != NULL) {
        UnhookWindowsHookEx(hook);
    }
    return 0;
}

static DWORD WINAPI watch_thread(LPVOID arg) {
    (void) arg;
    MSG m;
    watch_tid = GetCurrentThreadId();
    /* This thread's message queue must exist before RegisterHotKey can post to it. */
    PeekMessageW(&m, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    bool ticking = false, hotkey = false, leave_hotkey = false;
    /* The keyboard hook: its own thread (key_thread), started once this thread's id is known for the leave key. */
    InterlockedExchange(&key_rehooks, 0);
    InterlockedExchange(&keys_seen, 0);
    key_thread_handle = CreateThread(NULL, 0, key_thread, NULL, 0, &key_tid);
    if (key_thread_handle == NULL) {
        local_host_log("input: keyboard hook thread NOT started (%lu)", GetLastError());
    }
    /* Out-of-context: the callbacks run on THIS thread, from inside its PeekMessage calls. */
    HWINEVENTHOOK created = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, NULL, guard_event, 0, 0,
                                            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK shown = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, NULL, guard_event, 0, 0,
                                          WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK front = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, guard_event, 0, 0,
                                          WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    /* A window DRAGGED onto the client's monitor fires neither of the two above. */
    HWINEVENTHOOK dragged = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, NULL, guard_event, 0,
                                            0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    /* ...and neither fires for a window that moves ITSELF (user 2026-11-04: RDR2, once clicked, re-centred its
     * window over both panes and stayed). The rate limit in guard_consider bounds a window that insists. */
    HWINEVENTHOOK moved = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, NULL, guard_event,
                                          0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
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
            if (m.message == WM_HOTKEY && m.wParam == LEAVE_HOTKEY_ID) {
                local_host_log("input: Ctrl+Alt+Shift+Q while this PC's game had the keyboard -- disconnecting");
                bus_pushevent(USER_TASKBAR_COMMAND, (void *) (intptr_t) TASKBAR_CMD_DISCONNECT, NULL);
            }
        }
        /* The taskbar menu's "Give back the mouse": the hotkey's path, asked for from the UI thread. */
        if (InterlockedExchange(&li.give_back, 0) != 0 && InterlockedCompareExchange(&li.handed, 0, 0) != 0) {
            POINT p;
            GetCursorPos(&p);
            come_back(p, "taskbar menu");
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
                leave_hotkey = RegisterHotKey(NULL, LEAVE_HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT,
                                              'Q');
            } else {
                if (li.timer != NULL) {
                    CancelWaitableTimer(li.timer);
                }
                if (hotkey) {
                    UnregisterHotKey(NULL, RETURN_HOTKEY_ID);
                    hotkey = false;
                }
                if (leave_hotkey) {
                    UnregisterHotKey(NULL, LEAVE_HOTKEY_ID);
                    leave_hotkey = false;
                }
            }
            ticking = handed;
        }
        guard_sweep();
        key_log_drain();
        if (handed) {
            fence_update(false);
            check_cursor();
        } else {
            adopt_stranded_cursor();
            EnterCriticalSection(&li.cursor_lock);
            fence_update(InterlockedCompareExchange(&li.handing, 0, 0) == 0 &&
                         InterlockedCompareExchange(&li.handed, 0, 0) == 0);
            LeaveCriticalSection(&li.cursor_lock);
        }
    }
    fence_update(false);
    if (hotkey) {
        UnregisterHotKey(NULL, RETURN_HOTKEY_ID);
    }
    if (leave_hotkey) {
        UnregisterHotKey(NULL, LEAVE_HOTKEY_ID);
    }
    if (key_thread_handle != NULL) {
        PostThreadMessageW(key_tid, WM_QUIT, 0, 0);
        WaitForSingleObject(key_thread_handle, 1500);
        CloseHandle(key_thread_handle);
        key_thread_handle = NULL;
    }
    key_log_drain();
    local_host_log("input: keyboard hook saw %ld key event(s); re-installed %ld time(s)",
                   (long) InterlockedCompareExchange(&keys_seen, 0, 0),
                   (long) InterlockedCompareExchange(&key_rehooks, 0, 0));
    if (shown != NULL) {
        UnhookWinEvent(shown);
    }
    if (created != NULL) {
        UnhookWinEvent(created);
    }
    if (front != NULL) {
        UnhookWinEvent(front);
    }
    if (dragged != NULL) {
        UnhookWinEvent(dragged);
    }
    if (moved != NULL) {
        UnhookWinEvent(moved);
    }
    if (li.timer != NULL) {
        CancelWaitableTimer(li.timer);
    }
    return 0;
}

/* ---- Public ---------------------------------------------------------------------------------- */

void local_input_pin_display(const char *gdi_name) {
    if (!li.vdd_lock_ready) {
        InitializeCriticalSection(&li.vdd_lock);   /* as local_input_arm does; same (UI) thread */
        InitializeCriticalSection(&li.cursor_lock);
        li.vdd_lock_ready = true;
    }
    EnterCriticalSection(&li.vdd_lock);
    li.pin_dev[0] = L'\0';
    if (gdi_name != NULL && gdi_name[0] != '\0' &&
        MultiByteToWideChar(CP_UTF8, 0, gdi_name, -1, li.pin_dev, CCHDEVICENAME) == 0) {
        li.pin_dev[0] = L'\0';
    }
    li.vdd_dev[0] = L'\0';
    li.last_miss = 0;
    LeaveCriticalSection(&li.vdd_lock);
    if (li.pin_dev[0] != L'\0') {
        local_host_log("input: this PC's display is %ls (named by the session, not searched for)", li.pin_dev);
    }
}

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
        InitializeCriticalSection(&li.cursor_lock);
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

    /* NO SDL KEYBOARD GRAB IN THIS MODE. It used to be set here so that, while the client had the focus, the Windows
     * key and Alt+Tab reached the stream through SDL. Since syskey_hook routes EVERY key itself (to the stream over
     * its half; Alt+Tab to this PC over this PC's half; the Windows key never to this PC), the grab only got in the
     * way. MEASURED 2026-11-14 00:00 on SKYNET: with the client focused, SDL's grab swallowed Alt+Tab over this PC's
     * half (the foreground stayed on the client), and before SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED was "0" the same
     * grab made SDL minimize the client on Alt+Tab. Restored at disarm. */
    li.had_kb_grab = SDL_GetWindowKeyboardGrab(global->ui.window) == SDL_TRUE;
    SDL_SetWindowKeyboardGrab(global->ui.window, SDL_FALSE);

    /* NOTHING OF THIS PC'S IS DRAWN OVER THE SESSION (user 2026-11-10: Steam could still be dragged over the other
     * PC's pane, only snapping back on release; RDR2 opened windowed, centred over both panes). The guard moves
     * such windows to the virtual display, but only once a drag ends, and not at all for a window it cannot see or
     * move -- RDR2 was never reported to it (10:27:53-10:30:55, after a UAC prompt: running as administrator, out of
     * reach of this non-elevated process). Always-on-top keeps every ordinary window behind the client on its
     * monitor, whoever owns it. Through SDL, so SDL's own window flag agrees and its resizes keep it. */
    li.had_on_top = (SDL_GetWindowFlags(global->ui.window) & SDL_WINDOW_ALWAYS_ON_TOP) != 0;
    SDL_SetWindowAlwaysOnTop(global->ui.window, SDL_TRUE);
    /* The client is fullscreen and, by design, rarely the focused window (this PC's game is): SDL must not minimize
     * it when it loses the focus (22:55 measurement in guard_sweep). */
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0", SDL_HINT_OVERRIDE);
    /* ...nor on Alt+Tab. MEASURED 2026-11-13 23:38 on SKYNET (log_minimize_stack): with the client focused, Alt+Tab
     * minimized it from inside SDL's own key handling (window proc -> SDL -> ShowWindow), with the focus-loss hint
     * already "0" -- SDL minimizes a grabbed fullscreen window on Alt+Tab "so the user can escape" unless this hint
     * says no. Alt+Tab is this PC's to handle in its own half (syskey_hook_local). */
    SDL_SetHintWithPriority(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0", SDL_HINT_OVERRIDE);

    /* A click on the client must not take the foreground from this PC's game (see syskey_hook). */
    InterlockedExchange(&ui_overlay, 0);
    memset(fwd_down, 0, sizeof(fwd_down));
    memset(local_down, 0, sizeof(local_down));
    key_log_budget = 8;
    li.game_lock = app_configuration != NULL && app_configuration->coop_game_mouse_lock;
    local_host_log("input: a game confining the cursor %s (setting coop_game_mouse_lock)",
                   li.game_lock ? "KEEPS it on this PC's pane" : "does not stop it crossing the divider");
    forward_injected = GetFileAttributesW(L"C:\\ProgramData\\Eclipse\\test-forward-injected") != INVALID_FILE_ATTRIBUTES;
    if (forward_injected) {
        local_host_log("input: TEST: test-forward-injected present -- injected keys are forwarded like physical ones");
    }
    fwd_mods = 0;
    if (client_old_proc == NULL) {
        client_old_proc = (WNDPROC) SetWindowLongPtrW(li.client, GWLP_WNDPROC, (LONG_PTR) client_proc);
    }

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

void local_input_give_back(void) {
    if (li.wake != NULL) {
        InterlockedExchange(&li.give_back, 1);
        SetEvent(li.wake);
    }
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
    drag_release();   /* a drag in progress when the session ended must not leave the cursor confined */
    release_forwarded();   /* no key left held on the stream's host */
    if (client_old_proc != NULL && li.client != NULL &&
        (WNDPROC) GetWindowLongPtrW(li.client, GWLP_WNDPROC) == client_proc) {
        SetWindowLongPtrW(li.client, GWLP_WNDPROC, (LONG_PTR) client_old_proc);
    }
    client_old_proc = NULL;
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
    if (global != NULL && global->ui.window != NULL) {
        SDL_SetWindowKeyboardGrab(global->ui.window, li.had_kb_grab ? SDL_TRUE : SDL_FALSE);
        SDL_SetWindowAlwaysOnTop(global->ui.window, li.had_on_top ? SDL_TRUE : SDL_FALSE);
        SDL_ResetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS);
        SDL_ResetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED);
    }
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
    /* STALE MOTION IS NOT A CROSSING. MEASURED 2026-11-11 (left-to-right "teleports to the far left"): the cursor
     * overshot the virtual display's edge onto the monitor, the watcher brought it back into the stream's pane,
     * and 6-12 ms later SDL delivered the queued motion for the overshoot point (window x 0..8 = this PC's pane),
     * which handed it over again at pane x 0..8. Hand over only while the REAL cursor is over this PC's pane. */
    POINT real;
    if (!GetCursorPos(&real)) {
        return false;   /* the secure desktop owns the cursor */
    }
    if (client_half_at(real) != 1) {
        static unsigned stale = 0;
        if (log_this(++stale)) {
            local_host_log("input: stale motion for pane (%d,%d) ignored -- the cursor is at (%ld,%ld), not over "
                           "this PC's pane (#%u)", x, y, (long) real.x, (long) real.y, stale);
        }
        return false;
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
    EnterCriticalSection(&li.cursor_lock);
    li.vdd = v;
    InterlockedExchange(&li.handing, 1);
    ClipCursor(NULL);      /* the fence (fence_update) would hold the cursor on the client */
    SetCursorPos(p.x, p.y);
    LeaveCriticalSection(&li.cursor_lock);
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
        POINT now = {-1, -1};
        GetCursorPos(&now);
        local_host_log("input: handed over #%u at pane (%d,%d) of %dx%d -> display (%ld,%ld) in (%ld,%ld)-(%ld,%ld), "
                       "cursor now (%ld,%ld); keyboard to %s: %s",
                       li.handoffs, x, y, w, h, (long) p.x, (long) p.y, (long) v.left, (long) v.top, (long) v.right,
                       (long) v.bottom, (long) now.x, (long) now.y, who,
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

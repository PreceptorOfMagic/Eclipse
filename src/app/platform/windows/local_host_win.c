/*
 * CO-OP WITH THIS PC AS ONE OF THE HOSTS. When a stream's host is the machine this client runs
 * on, the host's display hook must not do what it does for a TV -- turn the physical panel off
 * and leave the virtual display as the only one -- because the panel is where this client's
 * window is: the user would see nothing, and the capture would hold the client inside its own
 * pane. The hook (xlink/dispwake/apollo-display.cmd) keeps the panel on and makes the virtual
 * display primary instead, when %ProgramData%\Eclipse\local-host-session names a live process.
 * This file writes that file -- only for a host whose address belongs to this machine -- before
 * the launch request, and removes it when the session ends. A client that crashes leaves a file
 * naming a dead process, which the hook ignores.
 */
#include "local_host_win.h"
#include "platform/this_pc.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

#include <SDL_syswm.h>
#include <Limelight.h>

static bool written;
static bool input_blocked;

/** Does @p address name one of this machine's own addresses (loopback included)? Binding a
 *  socket to an address succeeds only when an interface here owns it, so this needs no adapter
 *  enumeration and follows the live addresses however DHCP has moved them. */
static bool address_is_local(const char *address) {
    if (address == NULL || address[0] == '\0') {
        return false;
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return false;
    }
    bool local = false;
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(address, NULL, &hints, &res) == 0) {
        for (struct addrinfo *ai = res; ai != NULL && !local; ai = ai->ai_next) {
            SOCKET s = socket(ai->ai_family, SOCK_DGRAM, IPPROTO_UDP);
            if (s == INVALID_SOCKET) {
                continue;
            }
            local = bind(s, ai->ai_addr, (int) ai->ai_addrlen) == 0;
            closesocket(s);
        }
        freeaddrinfo(res);
    }
    WSACleanup();
    return local;
}

bool local_host_is_this_pc(const char *address) {
    return address_is_local(address);
}

/** %ProgramData%\Eclipse\local-host-session, creating the directory when @p create. */
static bool session_file_path(wchar_t *out, size_t len, bool create) {
    const wchar_t *base = _wgetenv(L"ProgramData");
    if (base == NULL || base[0] == L'\0') {
        base = L"C:\\ProgramData";
    }
    if (_snwprintf(out, len, L"%ls\\Eclipse", base) < 0) {
        return false;
    }
    out[len - 1] = L'\0';
    if (create && !CreateDirectoryW(out, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    if (_snwprintf(out, len, L"%ls\\Eclipse\\local-host-session", base) < 0) {
        return false;
    }
    out[len - 1] = L'\0';
    return true;
}

/** Append a line to %ProgramData%\\Eclipse\\local-host.log. Its own file because aurora-diag.log is
 *  rewritten when the video session starts, which erased every line written during the connect --
 *  exactly when a local-host session moves displays around (measured 2026-09-26). */
void local_host_log(const char *fmt, ...) {
    const wchar_t *base = _wgetenv(L"ProgramData");
    wchar_t path[MAX_PATH];
    if (_snwprintf(path, MAX_PATH, L"%ls\\Eclipse\\local-host.log",
                   base != NULL && base[0] != L'\0' ? base : L"C:\\ProgramData") < 0) {
        return;
    }
    path[MAX_PATH - 1] = L'\0';
    FILE *f = _wfopen(path, L"ab");
    if (f == NULL) {
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02u:%02u:%02u.%03u pid=%lu ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            (unsigned long) GetCurrentProcessId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputs("\r\n", f);
    fclose(f);
}

bool local_host_session_begin(const char *address) {
    if (!address_is_local(address)) {
        return false;
    }
    /* NO INPUT TO OUR OWN HOST, decided by the address alone -- never by whether the file below
     * could be written. The host would inject it into this machine's foreground window, this
     * client, and it comes back as new input: MEASURED 2026-09-26, one physical DS4 became four
     * virtual pads in 0.7 s (Apollo: "Gamepad 0..3 will be DualShock 4"); keys loop the same way.
     * The host's game is left with this PC's own devices. */
    LiSetLocalInputBlocked(true);
    input_blocked = true;
    wchar_t path[MAX_PATH];
    FILE *f = session_file_path(path, MAX_PATH, true) ? _wfopen(path, L"wb") : NULL;
    if (f != NULL) {
        /* ASCII, CRLF: the hook reads it with cmd's `set /p`. */
        fprintf(f, "%lu\r\n", (unsigned long) GetCurrentProcessId());
        written = fclose(f) == 0;
    }
    local_host_log("session: host %s is this machine -> input to it BLOCKED, local-host-session %s", address,
                   written ? "written" : "WRITE FAILED");
    return true;
}

void local_host_session_end(void) {
    if (input_blocked) {
        LiSetLocalInputBlocked(false);
        input_blocked = false;
    }
    if (!written) {
        return;
    }
    written = false;
    wchar_t path[MAX_PATH];
    if (session_file_path(path, MAX_PATH, false)) {
        DeleteFileW(path);
    }
    local_host_log("session: ended -> local-host-session removed");
}

/* ---- Keeping the window home ------------------------------------------------------------ */

/* MEASURED 2026-09-26 (first live local-host run): when Apollo created its virtual display,
 * Windows re-applied a stored layout with the physical panel OFF, every window moved to the
 * virtual display, and when the host's hook lit the panel again a moment later the windows
 * stayed where they were -- the client on a display nobody can see, captured into its own
 * pane. Windows does not move windows back, so the client does. */
static struct {
    wchar_t home[CCHDEVICENAME];   /* monitor the window belongs on, e.g. \\.\DISPLAY1 */
    unsigned long layout;          /* monitor-layout signature at the previous tick */
    int calm;                      /* ticks since the layout last changed */
    bool displaced;                /* a layout change, not the user, moved it off home */
    bool maximized;                /* maximized at home in a calm period: Windows may un-maximize
                                    * a window it moves, before any tick sees it (issue #6) */
    RECT rel;                      /* outer rect at home in a calm period, relative to the home
                                    * monitor's origin -- restored exactly (issue #6) */
    bool have_rel;
    bool restored_exact;           /* the last move_home used rel (logged: which path ran) */
} win_home;

/* A move this soon after a layout change is the change's doing: Windows repositions windows
 * a little after the monitor list changes, so a tick can land between the two. */
#define LAYOUT_SETTLE_TICKS 4

static BOOL CALLBACK layout_cb(HMONITOR mon, HDC dc, LPRECT rect, LPARAM param) {
    (void) dc;
    (void) rect;
    MONITORINFOEXW mi = {0};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, (MONITORINFO *) &mi)) {
        /* FNV-1a per monitor, summed: independent of enumeration order. */
        unsigned long h = 2166136261UL;
        for (const wchar_t *c = mi.szDevice; *c; c++) { h = (h ^ (unsigned long) *c) * 16777619UL; }
        const LONG v[4] = {mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom};
        for (int i = 0; i < 4; i++) { h = (h ^ (unsigned long) v[i]) * 16777619UL; }
        *(unsigned long *) param += h;
    }
    return TRUE;
}

static unsigned long layout_signature(void) {
    unsigned long sig = 1;
    EnumDisplayMonitors(NULL, NULL, layout_cb, (LPARAM) &sig);
    return sig;
}

typedef struct {
    const wchar_t *dev;
    RECT work;
    RECT mon;
    bool found;
} find_monitor_t;

static BOOL CALLBACK find_cb(HMONITOR mon, HDC dc, LPRECT rect, LPARAM param) {
    (void) dc;
    (void) rect;
    find_monitor_t *f = (find_monitor_t *) param;
    MONITORINFOEXW mi = {0};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, (MONITORINFO *) &mi) && wcscmp(mi.szDevice, f->dev) == 0) {
        f->work = mi.rcWork;
        f->mon = mi.rcMonitor;
        f->found = true;
        return FALSE;
    }
    return TRUE;
}

/** Is a local-host session running: the file exists AND names a live Eclipse client, as the
 *  hook checks. A file left by a crashed client (dead pid) must not keep the guard armed. */
static bool local_session_active(void) {
    wchar_t path[MAX_PATH];
    if (!session_file_path(path, MAX_PATH, false)) {
        return false;
    }
    FILE *f = _wfopen(path, L"rb");
    if (f == NULL) {
        return false;
    }
    unsigned long pid = 0;
    const bool parsed = fscanf(f, "%lu", &pid) == 1;
    fclose(f);
    if (!parsed || pid == 0) {
        return false;
    }
    if (pid == GetCurrentProcessId()) {
        return true;
    }
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid);
    if (proc == NULL) {
        return false;
    }
    DWORD code = 0;
    wchar_t image[MAX_PATH];
    DWORD image_len = MAX_PATH;
    bool alive = GetExitCodeProcess(proc, &code) && code == STILL_ACTIVE &&
                 QueryFullProcessImageNameW(proc, 0, image, &image_len);
    CloseHandle(proc);
    if (!alive) {
        return false;
    }
    const wchar_t *base = wcsrchr(image, L'\\');
    return _wcsicmp(base != NULL ? base + 1 : image, L"moonlight-tv.exe") == 0;
}

static LONG min_long(LONG a, LONG b) { return a < b ? a : b; }

static void move_home(SDL_Window *window, HWND hwnd, const RECT *work, const RECT *mon) {
    const Uint32 flags = SDL_GetWindowFlags(window);
    const Uint32 fullscreen = flags & SDL_WINDOW_FULLSCREEN_DESKTOP;
    const bool maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0 || win_home.maximized;
    if (fullscreen) {
        SDL_SetWindowFullscreen(window, 0);
    }
    if (maximized) {
        SDL_RestoreWindow(window);
    }
    /* NOACTIVATE: on the virtual display the game may own the focus by now. A saved rect larger
     * than the monitor (it shrank mid-session) would land mostly on a neighbour: clamp instead. */
    const bool fits = win_home.have_rel &&
                      win_home.rel.right - win_home.rel.left <= mon->right - mon->left &&
                      win_home.rel.bottom - win_home.rel.top <= mon->bottom - mon->top;
    win_home.restored_exact = fits;
    if (fits) {
        /* EXACTLY where it was, relative to its monitor (which may itself have moved in the
         * desktop). Clamping the OUTER rect to the work area shrank a window whose frame hung
         * past the screen edges -- 2560x1392 client came back 2542x1351, and the stream was
         * letterboxed 6% smaller for the whole session (run W1, 2026-09-27). */
        SetWindowPos(hwnd, NULL, mon->left + win_home.rel.left, mon->top + win_home.rel.top,
                     win_home.rel.right - win_home.rel.left, win_home.rel.bottom - win_home.rel.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        RECT r;
        GetWindowRect(hwnd, &r);
        const LONG ww = min_long(r.right - r.left, work->right - work->left);
        const LONG wh = min_long(r.bottom - r.top, work->bottom - work->top);
        SetWindowPos(hwnd, NULL, work->left + (work->right - work->left - ww) / 2,
                     work->top + (work->bottom - work->top - wh) / 2, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (maximized) {
        SDL_MaximizeWindow(window);
    }
    if (fullscreen) {
        SDL_SetWindowFullscreen(window, fullscreen);
    }
}

void local_host_window_tick(SDL_Window *window) {
    if (window == NULL) {
        return;
    }
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS) {
        return;
    }
    HWND hwnd = info.info.win.window;
    const Uint32 flags = SDL_GetWindowFlags(window);
    if (flags & SDL_WINDOW_MINIMIZED) {
        return;   /* nowhere to judge or move it; resume when it is restored */
    }
    MONITORINFOEXW mi = {0};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (mon == NULL || !GetMonitorInfoW(mon, (MONITORINFO *) &mi)) {
        return;
    }
    const unsigned long layout = layout_signature();
    const bool layout_changed = win_home.layout != 0 && layout != win_home.layout;
    win_home.layout = layout;
    win_home.calm = layout_changed ? 0 : (win_home.calm < LAYOUT_SETTLE_TICKS ? win_home.calm + 1 : win_home.calm);

    if (win_home.home[0] == L'\0' || wcscmp(mi.szDevice, win_home.home) == 0) {
        if (win_home.displaced) {
            local_host_log("window: back on %ls without being moved here (Windows restored it)", win_home.home);
        }
        wcsncpy(win_home.home, mi.szDevice, CCHDEVICENAME - 1);
        win_home.displaced = false;
        if (win_home.calm >= LAYOUT_SETTLE_TICKS) {
            win_home.maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
            RECT r;
            if (!win_home.maximized && !(flags & SDL_WINDOW_FULLSCREEN) && GetWindowRect(hwnd, &r)) {
                win_home.rel = (RECT) {r.left - mi.rcMonitor.left, r.top - mi.rcMonitor.top,
                                       r.right - mi.rcMonitor.left, r.bottom - mi.rcMonitor.top};
                win_home.have_rel = true;
            }
        }
        return;
    }
    if (win_home.calm < LAYOUT_SETTLE_TICKS && !win_home.displaced) {
        win_home.displaced = true;
        local_host_log("window: on %ls after a display change, home %ls (local session: %s)",
                       mi.szDevice, win_home.home, local_session_active() ? "yes" : "no");
    }
    /* Moved with the layout unchanged is the user's own move: that is home now. So is any
     * displacement outside a local-host session -- this only undoes what one did. */
    if (!win_home.displaced || !local_session_active()) {
        if (local_session_active()) {
            /* Adopting the new monitor mid-session is right for a user's drag and wrong for a
             * late system move: say so, or the second case is invisible. */
            local_host_log("window: now on %ls, adopted as home (was %ls) -- no recent display change",
                           mi.szDevice, win_home.home);
        }
        wcsncpy(win_home.home, mi.szDevice, CCHDEVICENAME - 1);
        win_home.displaced = false;
        win_home.maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
        win_home.have_rel = false;   /* relative to the OLD monitor; the next calm tick re-takes it */
        return;
    }
    find_monitor_t f = {.dev = win_home.home, .found = false};
    EnumDisplayMonitors(NULL, NULL, find_cb, (LPARAM) &f);
    if (!f.found) {
        return;   /* home is dark for now; stay displaced and try again next tick */
    }
    move_home(window, hwnd, &f.work, &f.mon);
    win_home.displaced = false;
    local_host_log("window: moved to %ls by a display change -> returned to %ls (sdl flags 0x%x, was maximized %d, "
                   "rect %s)", mi.szDevice, win_home.home, (unsigned) flags, win_home.maximized,
                   win_home.restored_exact ? "exact" : "clamped");
}

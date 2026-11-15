/*
 * Stand-alone test of src/app/platform/windows/local_pane_win.c: makes the pane's virtual display, shows it in a
 * window on a real monitor, reports the rates, takes a screenshot, removes the display and prints the layout
 * before and after. It exists so the two things that module depends on (the driver accepting an ordinary
 * user's ADD, and Windows Graphics Capture of that display) are measured without the client around them.
 *
 * Build (MSYS2 MINGW64), from the repository root:
 *   gcc -O2 -Wall -Isrc/app/platform/windows -o local_pane_probe.exe scripts/windows/local_pane_probe.c \
 *       src/app/platform/windows/local_pane_win.c -lsetupapi -ld3d11 -ldxgi -ldxguid -lole32 -luuid -ldwmapi -lgdi32
 *
 * Use (run it NOT elevated, in the signed-in user's session):
 *   local_pane_probe list
 *   local_pane_probe add W H HZ [SECONDS] [--view] [--anim] [--shot FILE.bmp] [--log FILE]
 *   local_pane_probe view GDI_NAME [SECONDS] [--shot FILE.bmp] [--log FILE]      (no display is made)
 *
 * --view  a window on the first real monitor shows the display (what the client's half will do)
 * --anim  a window on the new display repaints once per composition, so there is something to capture
 * "add" CHANGES THE DISPLAY LAYOUT of the PC for SECONDS (default 10) and puts it back by removing the display.
 */

#define COBJMACROS
#include <windows.h>
#include <dwmapi.h>
#include <dxgi.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "local_pane_win.h"

static FILE *log_file = NULL;
static DWORD t_start = 0;

static void out(const char *fmt, ...) {
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    const unsigned long ms = (unsigned long) (GetTickCount() - t_start);
    printf("[%6lu] %s\n", ms, line);
    fflush(stdout);
    if (log_file != NULL) {
        fprintf(log_file, "[%6lu] %s\n", ms, line);
        fflush(log_file);
    }
}

static void module_log(const char *line) {
    out("%s", line);
}

static void list_dxgi(void);

/** Every display attached to the desktop: name, mode, position, primary. */
static void list_displays(const char *title) {
    out("-- displays (%s) --", title);
    DISPLAY_DEVICEW dd = {.cb = sizeof(dd)};
    for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); ++i) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) {
            continue;
        }
        DEVMODEW dm = {0};
        dm.dmSize = sizeof(dm);
        EnumDisplaySettingsExW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0);
        DISPLAY_DEVICEW mon = {.cb = sizeof(mon)};
        EnumDisplayDevicesW(dd.DeviceName, 0, &mon, 0);
        out("   %ls  %lux%lu@%lu at %ld,%ld%s  adapter \"%ls\"  monitor \"%ls\"", dd.DeviceName,
            (unsigned long) dm.dmPelsWidth, (unsigned long) dm.dmPelsHeight, (unsigned long) dm.dmDisplayFrequency,
            (long) dm.dmPosition.x, (long) dm.dmPosition.y,
            (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? "  PRIMARY" : "", dd.DeviceString, mon.DeviceString);
    }
}

/** The rectangle of a display by GDI name, or of the first one that is NOT @p not_this when @p name is NULL. */
static bool display_rect(const WCHAR *name, const WCHAR *not_this, RECT *rc, WCHAR found[CCHDEVICENAME]) {
    DISPLAY_DEVICEW dd = {.cb = sizeof(dd)};
    for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); ++i) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) {
            continue;
        }
        if (name != NULL ? wcscmp(dd.DeviceName, name) != 0 : (not_this != NULL && wcscmp(dd.DeviceName, not_this) == 0)) {
            continue;
        }
        DEVMODEW dm = {0};
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsExW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0)) {
            continue;
        }
        rc->left = dm.dmPosition.x;
        rc->top = dm.dmPosition.y;
        rc->right = rc->left + (LONG) dm.dmPelsWidth;
        rc->bottom = rc->top + (LONG) dm.dmPelsHeight;
        if (found != NULL) {
            wcsncpy(found, dd.DeviceName, CCHDEVICENAME - 1);
            found[CCHDEVICENAME - 1] = L'\0';
        }
        return true;
    }
    return false;
}

/** The whole desktop (every display) as a 24-bit BMP. */
static void save_shot(const char *path) {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    bool ok = false;
    if (bmp != NULL) {
        HGDIOBJ old = SelectObject(mem, bmp);
        ok = BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT) != 0;
        GdiFlush();
        const DWORD stride = ((DWORD) w * 3 + 3) & ~3u;
        const DWORD size = stride * (DWORD) h;
        BITMAPFILEHEADER fh = {0};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
        fh.bfSize = fh.bfOffBits + size;
        FILE *f = fopen(path, "wb");
        if (ok && f != NULL) {
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
            fwrite(bits, 1, size, f);
        } else {
            ok = false;
        }
        if (f != NULL) {
            fclose(f);
        }
        SelectObject(mem, old);
        DeleteObject(bmp);
    }
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    out("screenshot %dx%d (desktop origin %d,%d) -> %s: %s", w, h, x, y, path, ok ? "ok" : "FAILED");
}

/* ── --anim: something moving on the new display ─────────────────────────── */

static volatile LONG anim_stop = 0;
static volatile LONG anim_frames = 0;
static RECT anim_rect;

static DWORD WINAPI anim_main(void *arg) {
    (void) arg;
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"PaneProbeAnim";
    RegisterClassExW(&wc);
    const int w = anim_rect.right - anim_rect.left, h = anim_rect.bottom - anim_rect.top;
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"pane probe animation",
                                WS_POPUP | WS_VISIBLE, anim_rect.left, anim_rect.top, w, h, NULL, NULL, wc.hInstance,
                                NULL);
    if (hwnd == NULL) {
        out("anim: no window, error %lu", (unsigned long) GetLastError());
        return 1;
    }
    HBRUSH bg = CreateSolidBrush(RGB(20, 40, 90));
    HBRUSH bar = CreateSolidBrush(RGB(255, 220, 0));
    unsigned n = 0;
    while (!anim_stop) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            DispatchMessageW(&msg);
        }
        HDC dc = GetDC(hwnd);
        RECT full = {0, 0, w, h};
        FillRect(dc, &full, bg);
        const int bx = (int) ((n * 12u) % (unsigned) (w > 40 ? w - 40 : 1));
        RECT r = {bx, 0, bx + 40, h};
        FillRect(dc, &r, bar);
        char text[80];
        const int len = snprintf(text, sizeof(text), "PANE DISPLAY  frame %u  %dx%d", n, w, h);
        TextOutA(dc, 60, 60, text, len);
        ReleaseDC(hwnd, dc);
        ++n;
        InterlockedIncrement(&anim_frames);
        DwmFlush();
    }
    DeleteObject(bg);
    DeleteObject(bar);
    DestroyWindow(hwnd);
    return 0;
}

/* ── the window that stands in for the client ────────────────────────────── */

static LRESULT CALLBACK parent_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SIZE) {
        local_pane_view_set_area(0, 0, LOWORD(lp), HIWORD(lp));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND make_parent(const RECT *mon) {
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = parent_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hbrBackground = CreateSolidBrush(RGB(90, 20, 20));
    wc.lpszClassName = L"PaneProbeParent";
    RegisterClassExW(&wc);
    /* Half the monitor, the way the client gives this PC half its window. */
    const int mw = mon->right - mon->left, mh = mon->bottom - mon->top;
    return CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"pane probe: the client's half", WS_POPUP | WS_VISIBLE,
                           mon->left, mon->top, mw / 2, mh, NULL, NULL, wc.hInstance, NULL);
}

static void run_for(int seconds, HWND parent, const char *shot, bool anim) {
    const DWORD t0 = GetTickCount();
    DWORD next_stat = t0 + 1000;
    bool shot_taken = shot == NULL;
    local_pane_stats_t prev = {0};
    LONG prev_anim = 0;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const DWORD now = GetTickCount();
        if (now - t0 >= (DWORD) seconds * 1000u) {
            break;
        }
        if ((LONG) (now - next_stat) >= 0) {
            next_stat += 1000;
            local_pane_stats_t st = {0};
            local_pane_view_stats(&st);
            const LONG a = anim_frames;
            if (parent != NULL) {
                out("1 s: captured %lu  shown %lu  superseded %lu  (%dx%d, %s)%s%ld", st.frames - prev.frames,
                    st.presents - prev.presents, st.dropped - prev.dropped, st.cap_w, st.cap_h,
                    st.running ? "running" : "NOT running", anim ? "  painted " : "  ", anim ? (long) (a - prev_anim) : 0L);
            } else if (anim) {
                out("1 s: painted %ld", (long) (a - prev_anim));
            }
            prev = st;
            prev_anim = a;
        }
        if (!shot_taken && now - t0 >= (DWORD) seconds * 500u) {
            shot_taken = true;
            save_shot(shot);
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
    }
}

int main(int argc, char **argv) {
    t_start = GetTickCount();
    SetProcessDPIAware();
    const char *mode = argc > 1 ? argv[1] : "";
    const char *shot = NULL;
    bool view = false, anim = false;
    const char *pos[4] = {NULL, NULL, NULL, NULL};
    int n_pos = 0;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--view") == 0) {
            view = true;
        } else if (strcmp(argv[i], "--anim") == 0) {
            anim = true;
        } else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            shot = argv[++i];
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_file = fopen(argv[++i], "w");
        } else if (n_pos < 4) {
            pos[n_pos++] = argv[i];
        }
    }
    local_pane_set_log(module_log);

    if (strcmp(mode, "list") == 0) {
        list_displays("now");
        list_dxgi();
        out("virtual display driver available to this user: %s", local_pane_available() ? "yes" : "NO");
        return 0;
    }

    if (strcmp(mode, "view") == 0 && n_pos >= 1) {
        const int seconds = n_pos >= 2 ? atoi(pos[1]) : 10;
        WCHAR name[CCHDEVICENAME] = L"", other[CCHDEVICENAME] = L"";
        MultiByteToWideChar(CP_UTF8, 0, pos[0], -1, name, CCHDEVICENAME);
        RECT mon;
        list_displays("now");
        /* Show it on another monitor when there is one, else on itself (a hall of mirrors, still a valid rate). */
        if (!display_rect(NULL, name, &mon, other) && !display_rect(name, NULL, &mon, other)) {
            out("no display to put the window on");
            return 1;
        }
        HWND parent = make_parent(&mon);
        RECT rc;
        GetClientRect(parent, &rc);
        if (!local_pane_view_start(parent, pos[0], 0, 0, rc.right, rc.bottom)) {
            out("the view did not start");
            return 1;
        }
        run_for(seconds, parent, shot, false);
        local_pane_view_stop();
        DestroyWindow(parent);
        return 0;
    }

    if (strcmp(mode, "add") == 0 && n_pos >= 3) {
        const int w = atoi(pos[0]), h = atoi(pos[1]), hz = atoi(pos[2]);
        const int seconds = n_pos >= 4 ? atoi(pos[3]) : 10;
        list_displays("before");
        out("virtual display driver available to this user: %s", local_pane_available() ? "yes" : "NO");
        if (!local_pane_display_add(w, h, hz)) {
            out("RESULT: the display was NOT made");
            list_displays("after the failed add");
            return 1;
        }
        char name[64] = "";
        local_pane_display_name(name, sizeof(name));
        WCHAR wname[CCHDEVICENAME] = L"", real[CCHDEVICENAME] = L"";
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, CCHDEVICENAME);
        Sleep(500);
        list_displays("with the pane's display");
        list_dxgi();

        HANDLE anim_thread = NULL;
        if (anim && display_rect(wname, NULL, &anim_rect, NULL)) {
            anim_thread = CreateThread(NULL, 0, anim_main, NULL, 0, NULL);
        }
        HWND parent = NULL;
        RECT mon;
        if (view) {
            if (display_rect(NULL, wname, &mon, real)) {
                out("the view goes on %ls", real);
                parent = make_parent(&mon);
                RECT rc;
                GetClientRect(parent, &rc);
                if (!local_pane_view_start(parent, NULL, 0, 0, rc.right, rc.bottom)) {
                    out("RESULT: the view did not start");
                }
            } else {
                out("no real monitor to show the view on");
            }
        }
        run_for(seconds, parent, shot, anim_thread != NULL);

        local_pane_view_stop();
        if (parent != NULL) {
            DestroyWindow(parent);
        }
        if (anim_thread != NULL) {
            InterlockedExchange(&anim_stop, 1);
            WaitForSingleObject(anim_thread, 3000);
            CloseHandle(anim_thread);
        }
        local_pane_display_remove();
        Sleep(2500);
        list_displays("after removing it");
        out("RESULT: done");
        return 0;
    }

    printf("usage:\n  %s list\n  %s add W H HZ [SECONDS] [--view] [--anim] [--shot FILE.bmp] [--log FILE]\n"
           "  %s view GDI_NAME [SECONDS] [--shot FILE.bmp] [--log FILE]\n", argv[0], argv[0], argv[0]);
    return 2;
}

/** What a game sees: each DXGI adapter and the outputs listed under it (a game picks "adapter N, output M"). */
static void list_dxgi(void) {
    IDXGIFactory1 *f = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **) &f))) {
        out("dxgi: no factory");
        return;
    }
    IDXGIAdapter1 *a = NULL;
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        IDXGIAdapter1_GetDesc1(a, &d);
        out("dxgi adapter %u: %ls (%lu MiB)%s", i, d.Description, (unsigned long) (d.DedicatedVideoMemory >> 20),
            (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " [software]" : "");
        IDXGIOutput *o = NULL;
        for (UINT j = 0; IDXGIAdapter1_EnumOutputs(a, j, &o) != DXGI_ERROR_NOT_FOUND; j++) {
            DXGI_OUTPUT_DESC od;
            IDXGIOutput_GetDesc(o, &od);
            out("   output %u: %ls (%ld,%ld)-(%ld,%ld)", j, od.DeviceName, od.DesktopCoordinates.left,
                od.DesktopCoordinates.top, od.DesktopCoordinates.right, od.DesktopCoordinates.bottom);
            IDXGIOutput_Release(o);
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
}

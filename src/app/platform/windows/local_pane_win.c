/*
 * This PC's own co-op pane without streaming it: see local_pane_win.h for what and why.
 *
 * WHAT IS MEASURED AND WHAT IS NOT (keep this honest; a comment here is a claim, not evidence):
 *   - The SudoVDA control device opens for a non-elevated user and answers GET_PROTOCOL_VERSION (0.2.1) and
 *     GET_WATCHDOG (timeout 3 s): SKYNET, 2026-11-10.
 *   - Everything else in this file is written from the driver's public ioctl header (SudoMaker/SudoVDA,
 *     sudovda-ioctl.h) and Microsoft's documented Windows.Graphics.Capture ABI. Results of running it are in
 *     docs/design-host-and-client-local-pane.md, not here.
 *
 * C, not C++, on purpose: the app links no C++ runtime, and the WinRT capture interfaces are plain COM vtables
 * in mingw-w64's headers (COBJMACROS). The four WinRT entry points are looked up at run time so the program
 * still starts on a Windows without them and the package's DLL check never meets an API-set name.
 */

#define COBJMACROS
#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Graphics
#define WIDL_using_Windows_Graphics_Capture
#define WIDL_using_Windows_Graphics_DirectX
#define WIDL_using_Windows_Graphics_DirectX_Direct3D11

#include <initguid.h>
#include <windows.h>
#include <setupapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <winstring.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "local_pane_win.h"

/* ── logging ──────────────────────────────────────────────────────────────── */

static local_pane_log_fn log_fn = NULL;

void local_pane_set_log(local_pane_log_fn fn) {
    log_fn = fn;
}

static void plog(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (log_fn != NULL) {
        log_fn(line);
    } else {
        OutputDebugStringA(line);
        OutputDebugStringA("\n");
    }
}

/* ══ DISPLAY: the SudoVDA control device ══════════════════════════════════════
 *
 * The ioctl contract, copied from the driver's sudovda-ioctl.h (protocol 0.2.1). FILE_ANY_ACCESS on every code,
 * so the handle's access rights do not gate them. */

#define VDA_IOCTL(fn) CTL_CODE(FILE_DEVICE_UNKNOWN, (fn), METHOD_BUFFERED, FILE_ANY_ACCESS)
#define VDA_ADD VDA_IOCTL(0x800)
#define VDA_REMOVE VDA_IOCTL(0x801)
#define VDA_SET_RENDER_ADAPTER VDA_IOCTL(0x802)   /* in: LUID (sudovda-ioctl.h VIRTUAL_DISPLAY_SET_RENDER_ADAPTER_PARAMS) */
#define VDA_GET_WATCHDOG VDA_IOCTL(0x803)
#define VDA_PING VDA_IOCTL(0x888)
#define VDA_GET_VERSION VDA_IOCTL(0x8FF)

/* {e5bcc234-1e0c-418a-a0d4-ef8b7501414d} */
static const GUID VDA_INTERFACE = {0xe5bcc234, 0x1e0c, 0x418a, {0xa0, 0xd4, 0xef, 0x8b, 0x75, 0x01, 0x41, 0x4d}};

/* ONE FIXED IDENTITY FOR ECLIPSE'S PANE. The driver keeps a display until it is removed or its watchdog runs
 * out, and the watchdog is the driver's, not the handle's: another program pinging the same driver (a host
 * program on this PC) would keep a display alive after this process crashed. A fixed monitor id lets the next
 * start remove exactly that leftover before adding, and lets Windows remember the layout for it.
 * {7b0c3e52-ec1f-4c6d-9a57-45434c495053} */
static const GUID VDA_ECLIPSE_MONITOR = {0x7b0c3e52, 0xec1f, 0x4c6d, {0x9a, 0x57, 0x45, 0x43, 0x4c, 0x49, 0x50, 0x53}};

typedef struct {
    UINT Width;
    UINT Height;
    UINT RefreshRate;   /* millihertz: what Umbra passes (fps_millihz). Read the mode back; do not trust this. */
    GUID MonitorGuid;
    CHAR DeviceName[14];
    CHAR SerialNumber[14];
} vda_add_params;

typedef struct {
    LUID AdapterLuid;
    UINT TargetId;
} vda_add_out;

typedef struct {
    GUID MonitorGuid;
} vda_remove_params;

typedef struct {
    UINT Timeout;
    UINT Countdown;
} vda_watchdog_out;

typedef struct {
    unsigned char Major, Minor, Incremental, TestBuild;
} vda_version_out;

static struct {
    SRWLOCK lock;
    HANDLE dev;
    HANDLE ping_thread;
    HANDLE ping_stop;
    DWORD ping_ms;
    bool added;
    LUID adapter;
    UINT target;
    WCHAR gdi[CCHDEVICENAME];
    int w, h, hz;
} vd = {.lock = SRWLOCK_INIT, .dev = INVALID_HANDLE_VALUE};

static HANDLE vda_open(void) {
    HANDLE handle = INVALID_HANDLE_VALUE;
    HDEVINFO set = SetupDiGetClassDevsW(&VDA_INTERFACE, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        return handle;
    }
    SP_DEVICE_INTERFACE_DATA itf = {.cbSize = sizeof(itf)};
    for (DWORD i = 0; handle == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, NULL, &VDA_INTERFACE, i, &itf);
         ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &itf, NULL, 0, &need, NULL);
        if (need == 0) {
            continue;
        }
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = calloc(1, need);
        if (detail == NULL) {
            break;
        }
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(set, &itf, detail, need, NULL, NULL)) {
            handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_EXISTING, 0, NULL);
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(set);
    return handle;
}

static bool vda_ioctl(HANDLE dev, DWORD code, void *in, DWORD in_len, void *out, DWORD out_len) {
    DWORD got = 0;
    return DeviceIoControl(dev, code, in, in_len, out, out_len, &got, NULL) != 0;
}

bool local_pane_available(void) {
    HANDLE dev = vda_open();
    if (dev == INVALID_HANDLE_VALUE) {
        return false;
    }
    vda_version_out ver = {0};
    const bool ok = vda_ioctl(dev, VDA_GET_VERSION, NULL, 0, &ver, sizeof(ver));
    CloseHandle(dev);
    return ok;
}

static DWORD WINAPI ping_thread_main(void *arg) {
    (void) arg;
    unsigned long failures = 0;
    while (WaitForSingleObject(vd.ping_stop, vd.ping_ms) == WAIT_TIMEOUT) {
        if (!vda_ioctl(vd.dev, VDA_PING, NULL, 0, NULL, 0)) {
            if (failures++ == 0) {
                plog("[pane] display keep-alive failed, error %lu", (unsigned long) GetLastError());
            }
        } else {
            failures = 0;
        }
    }
    return 0;
}

/** The GDI name of the active path that ends at (adapter, target), or false while Windows has not activated it. */
static bool active_path_name(LUID adapter, UINT target, WCHAR out[CCHDEVICENAME]) {
    UINT32 n_paths = 0, n_modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &n_paths, &n_modes) != ERROR_SUCCESS || n_paths == 0) {
        return false;
    }
    DISPLAYCONFIG_PATH_INFO *paths = calloc(n_paths, sizeof(*paths));
    DISPLAYCONFIG_MODE_INFO *modes = calloc(n_modes ? n_modes : 1, sizeof(*modes));
    bool found = false;
    if (paths != NULL && modes != NULL &&
        QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &n_paths, paths, &n_modes, modes, NULL) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < n_paths && !found; ++i) {
            const DISPLAYCONFIG_PATH_INFO *p = &paths[i];
            if (p->targetInfo.id != target || p->targetInfo.adapterId.LowPart != adapter.LowPart ||
                p->targetInfo.adapterId.HighPart != adapter.HighPart) {
                continue;
            }
            DISPLAYCONFIG_SOURCE_DEVICE_NAME name = {0};
            name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            name.header.size = sizeof(name);
            name.header.adapterId = p->sourceInfo.adapterId;
            name.header.id = p->sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS && name.viewGdiDeviceName[0] != L'\0') {
                wcsncpy(out, name.viewGdiDeviceName, CCHDEVICENAME - 1);
                out[CCHDEVICENAME - 1] = L'\0';
                found = true;
            }
        }
    }
    free(paths);
    free(modes);
    return found;
}

static bool current_mode(const WCHAR *gdi, DEVMODEW *dm) {
    memset(dm, 0, sizeof(*dm));
    dm->dmSize = sizeof(*dm);
    return EnumDisplaySettingsExW(gdi, ENUM_CURRENT_SETTINGS, dm, 0) != 0;
}

/* THE PANE'S DISPLAY BECOMES THE PRIMARY ONE, at (0,0), with every real monitor moved as a block below and to
 * its right, touching it ONLY AT A CORNER. Primary because that is where Windows opens new programs, and this PC's game must open in the pane, not on
 * the monitor the client window covers. Where the real monitors sit relative to it does not matter to the
 * user: the cursor never crosses by itself (local_input_win.c fences it and hands it over explicitly) -- which is
 * exactly why they must not share an EDGE. MEASURED 2026-11-11 on SKYNET with the block beside it (shared edge at
 * x=1280): a fast left-to-right crossing overshot the virtual display's divider edge onto the monitor between two
 * 1 ms polls, and the queued motion for that point (window x 0..8 = this PC's pane) handed the cursor back over at
 * the far left. A corner leaves no edge to overshoot across.
 * THE TOP-RIGHT CORNER, NOT THE BOTTOM-RIGHT. MEASURED 2026-11-12 22:54 on SKYNET: a game switched this display
 * from 1280x1440 to 1280x720; with the monitors below-right (touching at (1280,1440)) the corner no longer touched,
 * Windows moved the monitor up to (1280,720), and the client window -- left at its old desktop position -- hung
 * half off the screen. With the block ABOVE-right, touching at (w,0), the contact does not depend on this
 * display's height: a game may change it and nothing moves. (Its width is the pane's, which 16:9 games keep.)
 * Staged with CDS_NORESET and applied in one go, so there is one mode change, not one per display.
 * Nothing is saved for a restore: removing the display leaves the real monitors' own set, and Windows keeps a
 * remembered layout per set of connected monitors. */
static bool apply_layout(const WCHAR *vdd, int w, int h, int hz) {
    DEVMODEW cur;
    /* The other attached displays, and their bounding box's top-left corner. */
    enum { MAX_OTHERS = 16 };
    struct {
        WCHAR name[CCHDEVICENAME];
        DEVMODEW dm;
    } others[MAX_OTHERS];
    int n_others = 0;
    LONG min_x = LONG_MAX, max_bottom = LONG_MIN;
    DISPLAY_DEVICEW dd = {.cb = sizeof(dd)};
    for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); ++i) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) || wcscmp(dd.DeviceName, vdd) == 0 ||
            n_others == MAX_OTHERS) {
            continue;
        }
        if (!current_mode(dd.DeviceName, &others[n_others].dm)) {
            continue;
        }
        wcsncpy(others[n_others].name, dd.DeviceName, CCHDEVICENAME - 1);
        others[n_others].name[CCHDEVICENAME - 1] = L'\0';
        if (others[n_others].dm.dmPosition.x < min_x) { min_x = others[n_others].dm.dmPosition.x; }
        const LONG bottom = others[n_others].dm.dmPosition.y + (LONG) others[n_others].dm.dmPelsHeight;
        if (bottom > max_bottom) { max_bottom = bottom; }
        ++n_others;
    }

    /* Windows remembers a layout per set of monitors, so "already primary" is common -- but it may be a layout
     * from a build that put the monitors beside it: done only if the others are at the corner too. */
    if (current_mode(vdd, &cur) && cur.dmPosition.x == 0 && cur.dmPosition.y == 0 && (int) cur.dmPelsWidth == w &&
        (int) cur.dmPelsHeight == h && (int) cur.dmDisplayFrequency == hz &&
        (n_others == 0 || (min_x == w && max_bottom == 0))) {
        plog("[pane] layout: %ls is already primary at %dx%d@%d, %d other display(s) at its corner", vdd, w, h, hz,
             n_others);
        return true;
    }

    DEVMODEW dm = {0};
    dm.dmSize = sizeof(dm);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_POSITION;
    dm.dmPelsWidth = (DWORD) w;
    dm.dmPelsHeight = (DWORD) h;
    dm.dmDisplayFrequency = (DWORD) hz;
    dm.dmPosition.x = 0;
    dm.dmPosition.y = 0;
    LONG rc = ChangeDisplaySettingsExW(vdd, &dm, NULL, CDS_UPDATEREGISTRY | CDS_NORESET | CDS_SET_PRIMARY, NULL);
    if (rc != DISP_CHANGE_SUCCESSFUL) {
        /* The size is the pane's geometry and cannot give; the rate can. */
        plog("[pane] layout: %ls refused %dx%d@%d (%ld), trying without the rate", vdd, w, h, hz, rc);
        dm.dmFields &= ~DM_DISPLAYFREQUENCY;
        rc = ChangeDisplaySettingsExW(vdd, &dm, NULL, CDS_UPDATEREGISTRY | CDS_NORESET | CDS_SET_PRIMARY, NULL);
    }
    if (rc != DISP_CHANGE_SUCCESSFUL) {
        plog("[pane] layout: %ls refused %dx%d as primary (%ld)", vdd, w, h, rc);
        return false;
    }
    for (int i = 0; i < n_others; ++i) {
        DEVMODEW *o = &others[i].dm;
        o->dmFields = DM_POSITION;
        o->dmPosition.x = o->dmPosition.x - min_x + w;
        o->dmPosition.y = o->dmPosition.y - max_bottom;   /* the block's bottom edge on y = 0 */
        const LONG orc = ChangeDisplaySettingsExW(others[i].name, o, NULL, CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
        if (orc != DISP_CHANGE_SUCCESSFUL) {
            plog("[pane] layout: %ls refused its new position (%ld)", others[i].name, orc);
        }
    }
    rc = ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
    if (current_mode(vdd, &cur)) {
        plog("[pane] layout applied (%ld): %ls %lux%lu@%lu at %ld,%ld, %d other display(s)", rc, vdd,
             (unsigned long) cur.dmPelsWidth, (unsigned long) cur.dmPelsHeight,
             (unsigned long) cur.dmDisplayFrequency, (long) cur.dmPosition.x, (long) cur.dmPosition.y, n_others);
    }
    /* Read back where Windows actually put the others: it may snap a corner-only block to an edge. */
    for (int i = 0; i < n_others; ++i) {
        DEVMODEW now;
        if (current_mode(others[i].name, &now)) {
            plog("[pane] layout: %ls now at %ld,%ld (asked %ld,%ld)", others[i].name, (long) now.dmPosition.x,
                 (long) now.dmPosition.y, (long) others[i].dm.dmPosition.x, (long) others[i].dm.dmPosition.y);
        }
    }
    return rc == DISP_CHANGE_SUCCESSFUL;
}

static void display_remove_locked(void) {
    if (vd.ping_thread != NULL) {
        SetEvent(vd.ping_stop);
        WaitForSingleObject(vd.ping_thread, 2000);
        CloseHandle(vd.ping_thread);
        vd.ping_thread = NULL;
    }
    if (vd.ping_stop != NULL) {
        CloseHandle(vd.ping_stop);
        vd.ping_stop = NULL;
    }
    if (vd.dev != INVALID_HANDLE_VALUE) {
        if (vd.added) {
            vda_remove_params rm = {VDA_ECLIPSE_MONITOR};
            const bool ok = vda_ioctl(vd.dev, VDA_REMOVE, &rm, sizeof(rm), NULL, 0);
            plog("[pane] display %ls removed: %s", vd.gdi, ok ? "ok" : "FAILED");
        }
        CloseHandle(vd.dev);
        vd.dev = INVALID_HANDLE_VALUE;
    }
    vd.added = false;
    vd.gdi[0] = L'\0';
}

/* THE PANE'S DISPLAY RENDERS ON THE MAIN GPU. User 2026-11-13: RDR2 still opened centred over both panes; its own
 * menu picks "Output Adapter 0 / Output Monitor 0", and a game can only choose among the outputs DXGI lists under
 * the GPU it renders on. The driver takes a render adapter (IOCTL 0x802, as Umbra uses it: virtual_display_sudovda.cpp
 * setRenderAdapterByLuid, default = the adapter with the most dedicated memory). Whether that also lists this
 * display under that GPU for games is MEASURED with local_pane_probe ("dxgi" lines), not assumed. */
static void vda_set_render_adapter(HANDLE dev) {
    IDXGIFactory1 *factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **) &factory))) {
        plog("[pane] render adapter: no DXGI factory -- left to the driver");
        return;
    }
    LUID best = {0};
    SIZE_T best_mem = 0, best_shared = 0;
    WCHAR best_name[128] = L"";
    IDXGIAdapter1 *a = NULL;
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(factory, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        /* Umbra's rule (virtual_display_sudovda.cpp setRenderAdapterWithMostDedicatedMemory): most dedicated
         * memory, then most shared; the first of equals (a GPU is listed once per virtual-display driver too). */
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            (d.DedicatedVideoMemory > best_mem ||
             (d.DedicatedVideoMemory == best_mem && d.SharedSystemMemory > best_shared))) {
            best_mem = d.DedicatedVideoMemory;
            best_shared = d.SharedSystemMemory;
            best = d.AdapterLuid;
            wcsncpy(best_name, d.Description, 127);
            best_name[127] = 0;
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(factory);
    if (best_mem == 0) {
        plog("[pane] render adapter: no hardware GPU found -- left to the driver");
        return;
    }
    const bool ok = vda_ioctl(dev, VDA_SET_RENDER_ADAPTER, &best, sizeof(best), NULL, 0);
    plog("[pane] render adapter %ls (%lu MiB): %s", best_name, (unsigned long) (best_mem >> 20),
         ok ? "set" : "REFUSED by the driver");
}

/* Which GPU lists the pane's display as an output, and at which index: what a game choosing "adapter N, output M"
 * sees. Logged once per display so a user's log shows whether the render adapter took on their hardware. */
static void log_dxgi_owner(const WCHAR *gdi) {
    IDXGIFactory1 *f = NULL;
    if (gdi == NULL || gdi[0] == 0 || FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **) &f))) {
        return;
    }
    bool found = false;
    IDXGIAdapter1 *a = NULL;
    for (UINT i = 0; !found && IDXGIFactory1_EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        IDXGIAdapter1_GetDesc1(a, &d);
        IDXGIOutput *o = NULL;
        for (UINT j = 0; !found && IDXGIAdapter1_EnumOutputs(a, j, &o) != DXGI_ERROR_NOT_FOUND; j++) {
            DXGI_OUTPUT_DESC od;
            if (SUCCEEDED(IDXGIOutput_GetDesc(o, &od)) && _wcsicmp(od.DeviceName, gdi) == 0) {
                plog("[pane] games see this display as adapter %u (%ls), output %u", i, d.Description, j);
                found = true;
            }
            IDXGIOutput_Release(o);
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
    if (!found) {
        plog("[pane] games do not see this display under any GPU (DXGI lists no output for it)");
    }
}

bool local_pane_display_add(int width, int height, int hz) {
    if (width <= 0 || height <= 0 || hz <= 0) {
        return false;
    }
    AcquireSRWLockExclusive(&vd.lock);
    display_remove_locked();

    vd.dev = vda_open();
    if (vd.dev == INVALID_HANDLE_VALUE) {
        plog("[pane] no virtual display driver (SudoVDA control device did not open, error %lu)",
             (unsigned long) GetLastError());
        ReleaseSRWLockExclusive(&vd.lock);
        return false;
    }
    vda_version_out ver = {0};
    vda_watchdog_out dog = {0};
    vda_ioctl(vd.dev, VDA_GET_VERSION, NULL, 0, &ver, sizeof(ver));
    if (!vda_ioctl(vd.dev, VDA_GET_WATCHDOG, NULL, 0, &dog, sizeof(dog)) || dog.Timeout == 0) {
        dog.Timeout = 3;   /* the value measured on SKYNET; pinging a driver without a watchdog is harmless */
    }
    vd.ping_ms = dog.Timeout * 1000 / 3;
    if (vd.ping_ms < 100) {
        vd.ping_ms = 100;
    }

    /* A leftover of ours from a run that never removed it (see VDA_ECLIPSE_MONITOR). Failure = there was none. */
    vda_remove_params rm = {VDA_ECLIPSE_MONITOR};
    if (vda_ioctl(vd.dev, VDA_REMOVE, &rm, sizeof(rm), NULL, 0)) {
        plog("[pane] removed a display left by an earlier run");
        Sleep(300);
    }

    vda_set_render_adapter(vd.dev);

    vda_add_params add = {0};
    add.Width = (UINT) width;
    add.Height = (UINT) height;
    add.RefreshRate = (UINT) hz * 1000u;
    add.MonitorGuid = VDA_ECLIPSE_MONITOR;
    strncpy(add.DeviceName, "Eclipse Pane", sizeof(add.DeviceName) - 1);
    strncpy(add.SerialNumber, "ECLIPSE-PANE", sizeof(add.SerialNumber) - 1);
    vda_add_out out = {0};
    if (!vda_ioctl(vd.dev, VDA_ADD, &add, sizeof(add), &out, sizeof(out))) {
        plog("[pane] the driver refused a %dx%d@%d display, error %lu (driver %u.%u.%u)", width, height, hz,
             (unsigned long) GetLastError(), ver.Major, ver.Minor, ver.Incremental);
        display_remove_locked();
        ReleaseSRWLockExclusive(&vd.lock);
        return false;
    }
    vd.added = true;
    vd.adapter = out.AdapterLuid;
    vd.target = out.TargetId;
    vd.w = width;
    vd.h = height;
    vd.hz = hz;

    /* The watchdog starts counting at once: ping before anything slow. */
    vd.ping_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    vd.ping_thread = CreateThread(NULL, 0, ping_thread_main, NULL, 0, NULL);

    /* Windows activates a new monitor by itself when it extends the desktop (its default for a monitor it has
     * not seen). If it has not after 2 s, ask for the extend topology once. */
    bool active = false;
    bool asked = false;
    const DWORD t0 = GetTickCount();
    while (!(active = active_path_name(vd.adapter, vd.target, vd.gdi))) {
        const DWORD waited = GetTickCount() - t0;
        if (waited > 6000) {
            break;
        }
        if (waited > 2000 && !asked) {
            asked = true;
            const LONG rc = SetDisplayConfig(0, NULL, 0, NULL, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
            plog("[pane] display not active after %lu ms, asked Windows to extend (%ld)", (unsigned long) waited, rc);
        }
        Sleep(50);
    }
    if (!active) {
        plog("[pane] the display was added (target %u) but Windows did not activate it", vd.target);
        display_remove_locked();
        ReleaseSRWLockExclusive(&vd.lock);
        return false;
    }
    DEVMODEW dm;
    current_mode(vd.gdi, &dm);
    plog("[pane] display %ls up after %lu ms: %lux%lu@%lu at %ld,%ld (asked %dx%d@%d; driver %u.%u.%u, watchdog %u s)",
         vd.gdi, (unsigned long) (GetTickCount() - t0), (unsigned long) dm.dmPelsWidth,
         (unsigned long) dm.dmPelsHeight, (unsigned long) dm.dmDisplayFrequency, (long) dm.dmPosition.x,
         (long) dm.dmPosition.y, width, height, hz, ver.Major, ver.Minor, ver.Incremental, dog.Timeout);
    apply_layout(vd.gdi, width, height, hz);
    log_dxgi_owner(vd.gdi);
    ReleaseSRWLockExclusive(&vd.lock);
    return true;
}

void local_pane_display_remove(void) {
    AcquireSRWLockExclusive(&vd.lock);
    display_remove_locked();
    ReleaseSRWLockExclusive(&vd.lock);
}

bool local_pane_display_name(char *out, size_t len) {
    bool have = false;
    AcquireSRWLockShared(&vd.lock);
    if (vd.added && vd.gdi[0] != L'\0' && out != NULL && len > 0) {
        have = WideCharToMultiByte(CP_UTF8, 0, vd.gdi, -1, out, (int) len, NULL, NULL) > 0;
    }
    ReleaseSRWLockShared(&vd.lock);
    return have;
}

/* ══ VIEW: Windows Graphics Capture of the display, in a child window ═════════
 *
 * WHY WINDOWS GRAPHICS CAPTURE: desktop duplication was measured stale under a full-screen game here
 * (2026-09-28). WHY A CHILD WINDOW WITH ITS OWN SWAP CHAIN rather than a texture in the client's renderer: the
 * pane then updates at the display's own rate on its own thread, with no copy through the CPU and no
 * dependence on the client's UI loop or on which SDL render driver is in use.
 *
 * The swap chain's buffers are the captured display's size and DXGI stretches them to the window, so a frame
 * is one GPU copy and no shader exists here. Letterboxing is done by sizing the window.
 */

/* IDirect3DDxgiInterfaceAccess: classic COM, declared here so only one interop header is needed.
 * {a9b3d012-3df2-4ee3-b8d1-8695f457d3c1} */
typedef struct pane_dxgi_access pane_dxgi_access;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(pane_dxgi_access *self, REFIID riid, void **out);
    ULONG (STDMETHODCALLTYPE *AddRef)(pane_dxgi_access *self);
    ULONG (STDMETHODCALLTYPE *Release)(pane_dxgi_access *self);
    HRESULT (STDMETHODCALLTYPE *GetInterface)(pane_dxgi_access *self, REFIID riid, void **out);
} pane_dxgi_access_vtbl;
struct pane_dxgi_access {
    const pane_dxgi_access_vtbl *lpVtbl;
};
static const GUID PANE_IID_DXGI_ACCESS = {0xa9b3d012, 0x3df2, 0x4ee3, {0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1}};
/* {94ea2b94-e9cc-49e0-c0ff-ee64ca8f5b90} */
static const GUID PANE_IID_AGILE = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
/* {00000000-0000-0000-c000-000000000046} */
static const GUID PANE_IID_UNKNOWN = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

typedef __FITypedEventHandler_2_Windows__CGraphics__CCapture__CDirect3D11CaptureFramePool_IInspectable frame_handler_itf;
typedef __FITypedEventHandler_2_Windows__CGraphics__CCapture__CDirect3D11CaptureFramePool_IInspectableVtbl
        frame_handler_vtbl;
#define PANE_IID_FRAME_HANDLER \
    IID___FITypedEventHandler_2_Windows__CGraphics__CCapture__CDirect3D11CaptureFramePool_IInspectable

#define PANE_PIXEL_FORMAT_BGRA8 87   /* DirectXPixelFormat_B8G8R8A8UIntNormalized */
#define PANE_WM_REFIT (WM_APP + 0x51)
#define PANE_CLASS L"EclipseLocalPaneView"

static struct {
    HWND parent, child;
    RECT area;                  /* in the parent's client pixels */
    bool want_visible;
    WCHAR gdi[CCHDEVICENAME];
    HANDLE thread, stop_event, frame_event;
    volatile LONG cap_w, cap_h;
    volatile LONG frames, presents, dropped;
    volatile LONG alive;        /* the thread is capturing */
    volatile LONG visible;      /* the thread should present */
} vw;

/* The WinRT entry points, looked up once. */
static struct {
    bool tried, ok;
    HRESULT (WINAPI *RoInitialize)(int type);
    void (WINAPI *RoUninitialize)(void);
    HRESULT (WINAPI *RoGetActivationFactory)(HSTRING id, REFIID iid, void **factory);
    HRESULT (WINAPI *WindowsCreateStringReference)(PCWSTR str, UINT32 len, HSTRING_HEADER *header, HSTRING *out);
    HRESULT (WINAPI *CreateDirect3D11DeviceFromDXGIDevice)(IDXGIDevice *dev, IInspectable **out);
} rt;

static bool rt_load(void) {
    if (rt.tried) {
        return rt.ok;
    }
    rt.tried = true;
    HMODULE combase = LoadLibraryW(L"combase.dll");
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    if (combase == NULL || d3d11 == NULL) {
        return false;
    }
    rt.RoInitialize = (void *) GetProcAddress(combase, "RoInitialize");
    rt.RoUninitialize = (void *) GetProcAddress(combase, "RoUninitialize");
    rt.RoGetActivationFactory = (void *) GetProcAddress(combase, "RoGetActivationFactory");
    rt.WindowsCreateStringReference = (void *) GetProcAddress(combase, "WindowsCreateStringReference");
    rt.CreateDirect3D11DeviceFromDXGIDevice = (void *) GetProcAddress(d3d11, "CreateDirect3D11DeviceFromDXGIDevice");
    rt.ok = rt.RoInitialize != NULL && rt.RoUninitialize != NULL && rt.RoGetActivationFactory != NULL &&
            rt.WindowsCreateStringReference != NULL && rt.CreateDirect3D11DeviceFromDXGIDevice != NULL;
    return rt.ok;
}

static HRESULT rt_factory(const WCHAR *class_name, REFIID iid, void **out) {
    HSTRING_HEADER header;
    HSTRING name = NULL;
    HRESULT hr = rt.WindowsCreateStringReference(class_name, (UINT32) wcslen(class_name), &header, &name);
    if (FAILED(hr)) {
        return hr;
    }
    return rt.RoGetActivationFactory(name, iid, out);
}

/* The FrameArrived handler: a static COM object whose only job is to wake the view thread. Agile, because a
 * free-threaded frame pool calls it from its own worker thread. Never freed (static), so the counts are only
 * what COM's contract asks for. */
typedef struct {
    frame_handler_itf itf;
    LONG refs;
} frame_handler;

static HRESULT STDMETHODCALLTYPE fh_query(frame_handler_itf *self, REFIID riid, void **out) {
    if (out == NULL) {
        return E_POINTER;
    }
    if (IsEqualGUID(riid, &PANE_IID_UNKNOWN) || IsEqualGUID(riid, &PANE_IID_AGILE) ||
        IsEqualGUID(riid, &PANE_IID_FRAME_HANDLER)) {
        *out = self;
        InterlockedIncrement(&((frame_handler *) self)->refs);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE fh_addref(frame_handler_itf *self) {
    return (ULONG) InterlockedIncrement(&((frame_handler *) self)->refs);
}

static ULONG STDMETHODCALLTYPE fh_release(frame_handler_itf *self) {
    return (ULONG) InterlockedDecrement(&((frame_handler *) self)->refs);
}

static HRESULT STDMETHODCALLTYPE fh_invoke(frame_handler_itf *self, IDirect3D11CaptureFramePool *sender,
                                           IInspectable *args) {
    (void) self;
    (void) sender;
    (void) args;
    if (vw.frame_event != NULL) {
        SetEvent(vw.frame_event);
    }
    return S_OK;
}

static frame_handler_vtbl fh_vtbl = {
        .QueryInterface = fh_query,
        .AddRef = fh_addref,
        .Release = fh_release,
        .Invoke = fh_invoke,
};
static frame_handler fh = {.itf = {.lpVtbl = &fh_vtbl}, .refs = 1};

static BOOL CALLBACK find_monitor_cb(HMONITOR mon, HDC dc, LPRECT rc, LPARAM param) {
    (void) dc;
    (void) rc;
    MONITORINFOEXW info = {0};
    info.cbSize = sizeof(info);
    HMONITOR *found = (HMONITOR *) param;
    if (GetMonitorInfoW(mon, (MONITORINFO *) &info) && wcscmp(info.szDevice, vw.gdi) == 0) {
        *found = mon;
        return FALSE;
    }
    return TRUE;
}

#define RELEASE(type, p) do { if ((p) != NULL) { type##_Release(p); (p) = NULL; } } while (0)

static DWORD WINAPI view_thread_main(void *arg) {
    (void) arg;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    IDXGIDevice *dxgi_dev = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *factory = NULL;
    IDXGISwapChain1 *chain = NULL;
    IInspectable *rt_dev_insp = NULL;
    IDirect3DDevice *rt_dev = NULL;
    IGraphicsCaptureItemInterop *interop = NULL;
    IGraphicsCaptureItem *item = NULL;
    IDirect3D11CaptureFramePoolStatics2 *pool_statics = NULL;
    IDirect3D11CaptureFramePool *pool = NULL;
    IGraphicsCaptureSession *session = NULL;
    EventRegistrationToken token = {0};
    bool have_handler = false;
    const char *step = "start";
    HRESULT hr = rt.RoInitialize(1 /* RO_INIT_MULTITHREADED */);
    const bool ro_inited = SUCCEEDED(hr);

    do {
        step = "find the display's monitor";
        HMONITOR monitor = NULL;
        EnumDisplayMonitors(NULL, NULL, find_monitor_cb, (LPARAM) &monitor);
        if (monitor == NULL) {
            hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            break;
        }

        step = "create the D3D11 device";
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                               D3D11_SDK_VERSION, &dev, NULL, &ctx);
        if (FAILED(hr)) { break; }
        /* The frame pool uses this device from its own threads. */
        ID3D10Multithread *mt = NULL;
        if (SUCCEEDED(ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D10Multithread, (void **) &mt))) {
            ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
            ID3D10Multithread_Release(mt);
        }
        step = "wrap the device for capture";
        hr = ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **) &dxgi_dev);
        if (FAILED(hr)) { break; }
        hr = rt.CreateDirect3D11DeviceFromDXGIDevice(dxgi_dev, &rt_dev_insp);
        if (FAILED(hr)) { break; }
        hr = IInspectable_QueryInterface(rt_dev_insp, &IID_IDirect3DDevice, (void **) &rt_dev);
        if (FAILED(hr)) { break; }

        step = "open the monitor for capture";
        hr = rt_factory(L"Windows.Graphics.Capture.GraphicsCaptureItem", &IID_IGraphicsCaptureItemInterop,
                        (void **) &interop);
        if (FAILED(hr)) { break; }
        hr = IGraphicsCaptureItemInterop_CreateForMonitor(interop, monitor, &IID_IGraphicsCaptureItem, (void **) &item);
        if (FAILED(hr)) { break; }
        SizeInt32 size = {0, 0};
        hr = IGraphicsCaptureItem_get_Size(item, &size);
        if (FAILED(hr) || size.Width <= 0 || size.Height <= 0) {
            if (SUCCEEDED(hr)) { hr = E_UNEXPECTED; }
            break;
        }

        step = "create the frame pool";
        hr = rt_factory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", &IID_IDirect3D11CaptureFramePoolStatics2,
                        (void **) &pool_statics);
        if (FAILED(hr)) { break; }
        hr = IDirect3D11CaptureFramePoolStatics2_CreateFreeThreaded(pool_statics, rt_dev, PANE_PIXEL_FORMAT_BGRA8, 2,
                                                                    size, &pool);
        if (FAILED(hr)) { break; }
        have_handler = SUCCEEDED(IDirect3D11CaptureFramePool_add_FrameArrived(pool, &fh.itf, &token));

        step = "start the capture";
        hr = IDirect3D11CaptureFramePool_CreateCaptureSession(pool, item, &session);
        if (FAILED(hr)) { break; }
        const char *cursor = "n/a", *border = "n/a";
        IGraphicsCaptureSession2 *s2 = NULL;
        if (SUCCEEDED(IGraphicsCaptureSession_QueryInterface(session, &IID_IGraphicsCaptureSession2, (void **) &s2))) {
            cursor = SUCCEEDED(IGraphicsCaptureSession2_put_IsCursorCaptureEnabled(s2, TRUE)) ? "on" : "refused";
            IGraphicsCaptureSession2_Release(s2);
        }
        IGraphicsCaptureSession3 *s3 = NULL;
        if (SUCCEEDED(IGraphicsCaptureSession_QueryInterface(session, &IID_IGraphicsCaptureSession3, (void **) &s3))) {
            border = SUCCEEDED(IGraphicsCaptureSession3_put_IsBorderRequired(s3, FALSE)) ? "off" : "refused";
            IGraphicsCaptureSession3_Release(s3);
        }
        hr = IGraphicsCaptureSession_StartCapture(session);
        if (FAILED(hr)) { break; }

        step = "create the swap chain";
        hr = IDXGIDevice_GetAdapter(dxgi_dev, &adapter);
        if (FAILED(hr)) { break; }
        hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **) &factory);
        if (FAILED(hr)) { break; }
        DXGI_SWAP_CHAIN_DESC1 desc = {0};
        desc.Width = (UINT) size.Width;
        desc.Height = (UINT) size.Height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *) dev, vw.child, &desc, NULL, NULL, &chain);
        if (FAILED(hr)) { break; }
        IDXGIFactory2_MakeWindowAssociation(factory, vw.child, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);

        DXGI_ADAPTER_DESC adesc = {0};
        IDXGIAdapter_GetDesc(adapter, &adesc);
        plog("[pane] view of %ls started: %dx%d on %ls, cursor %s, border %s, wake by %s", vw.gdi, (int) size.Width,
             (int) size.Height, adesc.Description, cursor, border, have_handler ? "event" : "1 ms poll");
        InterlockedExchange(&vw.cap_w, size.Width);
        InterlockedExchange(&vw.cap_h, size.Height);
        InterlockedExchange(&vw.alive, 1);
        PostMessageW(vw.child, PANE_WM_REFIT, 0, 0);

        step = "run";
        DWORD stat_t0 = GetTickCount();
        LONG stat_frames0 = 0, stat_presents0 = 0;
        while (WaitForSingleObject(vw.stop_event, 0) == WAIT_TIMEOUT) {
            WaitForSingleObject(vw.frame_event, have_handler ? 100 : 1);

            /* Only the newest frame is worth showing; older ones only add latency. */
            IDirect3D11CaptureFrame *frame = NULL, *next = NULL;
            while (SUCCEEDED(IDirect3D11CaptureFramePool_TryGetNextFrame(pool, &next)) && next != NULL) {
                if (frame != NULL) {
                    IDirect3D11CaptureFrame_Release(frame);
                    InterlockedIncrement(&vw.dropped);
                }
                frame = next;
                next = NULL;
                InterlockedIncrement(&vw.frames);
            }
            if (frame != NULL) {
                SizeInt32 content = size;
                IDirect3D11CaptureFrame_get_ContentSize(frame, &content);
                IDirect3DSurface *surface = NULL;
                pane_dxgi_access *access = NULL;
                ID3D11Texture2D *tex = NULL, *back = NULL;
                if (vw.visible && SUCCEEDED(IDirect3D11CaptureFrame_get_Surface(frame, &surface)) &&
                    SUCCEEDED(IDirect3DSurface_QueryInterface(surface, &PANE_IID_DXGI_ACCESS, (void **) &access)) &&
                    SUCCEEDED(access->lpVtbl->GetInterface(access, &IID_ID3D11Texture2D, (void **) &tex)) &&
                    SUCCEEDED(IDXGISwapChain1_GetBuffer(chain, 0, &IID_ID3D11Texture2D, (void **) &back))) {
                    D3D11_TEXTURE2D_DESC td, bd;
                    ID3D11Texture2D_GetDesc(tex, &td);
                    ID3D11Texture2D_GetDesc(back, &bd);
                    D3D11_BOX box = {0, 0, 0, td.Width < bd.Width ? td.Width : bd.Width,
                                     td.Height < bd.Height ? td.Height : bd.Height, 1};
                    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *) back, 0, 0, 0, 0,
                                                              (ID3D11Resource *) tex, 0, &box);
                    const HRESULT phr = IDXGISwapChain1_Present(chain, 0, 0);
                    if (SUCCEEDED(phr)) {
                        InterlockedIncrement(&vw.presents);
                    } else if (phr == DXGI_ERROR_DEVICE_REMOVED || phr == DXGI_ERROR_DEVICE_RESET) {
                        hr = phr;
                        step = "present (the graphics device was lost)";
                    }
                }
                if (back != NULL) { ID3D11Texture2D_Release(back); }
                if (tex != NULL) { ID3D11Texture2D_Release(tex); }
                if (access != NULL) { access->lpVtbl->Release(access); }
                if (surface != NULL) { IDirect3DSurface_Release(surface); }
                IDirect3D11CaptureFrame_Release(frame);
                if (FAILED(hr)) {
                    break;
                }

                /* The display changed size (a mode change): frames keep the old size until the pool is remade. */
                if (content.Width > 0 && content.Height > 0 &&
                    (content.Width != size.Width || content.Height != size.Height)) {
                    plog("[pane] the display is now %dx%d (was %dx%d)", (int) content.Width, (int) content.Height,
                         (int) size.Width, (int) size.Height);
                    size = content;
                    IDirect3D11CaptureFramePool_Recreate(pool, rt_dev, PANE_PIXEL_FORMAT_BGRA8, 2, size);
                    IDXGISwapChain1_ResizeBuffers(chain, 0, (UINT) size.Width, (UINT) size.Height, DXGI_FORMAT_UNKNOWN,
                                                  0);
                    InterlockedExchange(&vw.cap_w, size.Width);
                    InterlockedExchange(&vw.cap_h, size.Height);
                    PostMessageW(vw.child, PANE_WM_REFIT, 0, 0);
                }
            }

            const DWORD now = GetTickCount();
            if (now - stat_t0 >= 10000) {
                const LONG f = vw.frames, p = vw.presents;
                plog("[pane] view %ldx%ld: %.1f frames/s captured, %.1f shown, %ld superseded in total", vw.cap_w,
                     vw.cap_h, (f - stat_frames0) * 1000.0 / (now - stat_t0),
                     (p - stat_presents0) * 1000.0 / (now - stat_t0), vw.dropped);
                stat_t0 = now;
                stat_frames0 = f;
                stat_presents0 = p;
            }
        }
    } while (0);

    if (FAILED(hr)) {
        plog("[pane] view of %ls stopped at \"%s\": 0x%08lx", vw.gdi, step, (unsigned long) hr);
    }
    InterlockedExchange(&vw.alive, 0);
    if (have_handler && pool != NULL) {
        IDirect3D11CaptureFramePool_remove_FrameArrived(pool, token);
    }
    if (session != NULL) {
        IClosable *closable = NULL;
        if (SUCCEEDED(IGraphicsCaptureSession_QueryInterface(session, &IID_IClosable, (void **) &closable))) {
            IClosable_Close(closable);
            IClosable_Release(closable);
        }
    }
    if (pool != NULL) {
        IClosable *closable = NULL;
        if (SUCCEEDED(IDirect3D11CaptureFramePool_QueryInterface(pool, &IID_IClosable, (void **) &closable))) {
            IClosable_Close(closable);
            IClosable_Release(closable);
        }
    }
    RELEASE(IGraphicsCaptureSession, session);
    RELEASE(IDirect3D11CaptureFramePool, pool);
    RELEASE(IDirect3D11CaptureFramePoolStatics2, pool_statics);
    RELEASE(IGraphicsCaptureItem, item);
    RELEASE(IGraphicsCaptureItemInterop, interop);
    RELEASE(IDirect3DDevice, rt_dev);
    RELEASE(IInspectable, rt_dev_insp);
    RELEASE(IDXGISwapChain1, chain);
    RELEASE(IDXGIFactory2, factory);
    RELEASE(IDXGIAdapter, adapter);
    RELEASE(IDXGIDevice, dxgi_dev);
    if (ctx != NULL) {
        ID3D11DeviceContext_ClearState(ctx);
        ID3D11DeviceContext_Flush(ctx);
    }
    RELEASE(ID3D11DeviceContext, ctx);
    RELEASE(ID3D11Device, dev);
    if (ro_inited) {
        rt.RoUninitialize();
    }
    return 0;
}

/** Aspect-fit the captured display into the area and place the child there. UI thread only. */
static void view_fit(void) {
    if (vw.child == NULL) {
        return;
    }
    const int aw = vw.area.right - vw.area.left, ah = vw.area.bottom - vw.area.top;
    int x = vw.area.left, y = vw.area.top, w = aw, h = ah;
    const LONG cw = vw.cap_w, ch = vw.cap_h;
    if (cw > 0 && ch > 0 && aw > 0 && ah > 0) {
        const int fit_h = (int) ((long long) ch * aw / cw);
        if (fit_h <= ah) {
            h = fit_h;
            y += (ah - h) / 2;
        } else {
            w = (int) ((long long) cw * ah / ch);
            x += (aw - w) / 2;
        }
    }
    const bool show = vw.want_visible && w > 0 && h > 0;
    SetWindowPos(vw.child, HWND_TOP, x, y, w > 0 ? w : 1, h > 0 ? h : 1,
                 SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    InterlockedExchange(&vw.visible, show ? 1 : 0);
}

static LRESULT CALLBACK view_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST:
            /* Not ours: every click and move belongs to the client window underneath, which routes the pointer
             * (coop_pointer.c). Works because parent and child share a thread. */
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            ValidateRect(hwnd, NULL);
            return 0;
        case PANE_WM_REFIT:
            view_fit();
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

bool local_pane_view_start(void *parent_hwnd, const char *gdi_name, int x, int y, int w, int h) {
    local_pane_view_stop();
    if (parent_hwnd == NULL || !rt_load()) {
        plog("[pane] view: this Windows has no Windows Graphics Capture (or no parent window)");
        return false;
    }
    vw.gdi[0] = L'\0';
    if (gdi_name != NULL) {
        MultiByteToWideChar(CP_UTF8, 0, gdi_name, -1, vw.gdi, CCHDEVICENAME);
        vw.gdi[CCHDEVICENAME - 1] = L'\0';
    } else {
        AcquireSRWLockShared(&vd.lock);
        if (vd.added) {
            wcsncpy(vw.gdi, vd.gdi, CCHDEVICENAME - 1);
        }
        ReleaseSRWLockShared(&vd.lock);
    }
    if (vw.gdi[0] == L'\0') {
        plog("[pane] view: no display to show");
        return false;
    }

    static bool class_registered = false;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!class_registered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = view_wndproc;
        wc.hInstance = inst;
        wc.hbrBackground = (HBRUSH) GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = PANE_CLASS;
        class_registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    vw.parent = (HWND) parent_hwnd;
    vw.area = (RECT) {x, y, x + w, y + h};
    vw.want_visible = true;
    vw.cap_w = vw.cap_h = 0;
    vw.frames = vw.presents = vw.dropped = 0;
    vw.child = CreateWindowExW(WS_EX_NOPARENTNOTIFY | WS_EX_NOACTIVATE, PANE_CLASS, L"", WS_CHILD | WS_CLIPSIBLINGS,
                               x, y, w > 0 ? w : 1, h > 0 ? h : 1, vw.parent, NULL, inst, NULL);
    if (vw.child == NULL) {
        plog("[pane] view: the child window could not be created, error %lu", (unsigned long) GetLastError());
        return false;
    }
    vw.stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    vw.frame_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    view_fit();
    vw.thread = CreateThread(NULL, 0, view_thread_main, NULL, 0, NULL);
    if (vw.thread == NULL) {
        local_pane_view_stop();
        return false;
    }
    return true;
}

void local_pane_view_set_area(int x, int y, int w, int h) {
    vw.area = (RECT) {x, y, x + w, y + h};
    view_fit();
}

void local_pane_view_show(bool show) {
    if (vw.want_visible != show) {
        vw.want_visible = show;
        view_fit();
    }
}

void local_pane_view_stop(void) {
    if (vw.thread != NULL) {
        SetEvent(vw.stop_event);
        SetEvent(vw.frame_event);
        /* The thread never sends a message to the child (it only posts), so waiting here cannot deadlock. */
        WaitForSingleObject(vw.thread, 5000);
        CloseHandle(vw.thread);
        vw.thread = NULL;
    }
    if (vw.child != NULL) {
        DestroyWindow(vw.child);
        vw.child = NULL;
    }
    if (vw.stop_event != NULL) {
        CloseHandle(vw.stop_event);
        vw.stop_event = NULL;
    }
    if (vw.frame_event != NULL) {
        CloseHandle(vw.frame_event);
        vw.frame_event = NULL;
    }
    vw.alive = 0;
    vw.visible = 0;
    vw.parent = NULL;
}

void local_pane_view_stats(local_pane_stats_t *out) {
    if (out == NULL) {
        return;
    }
    out->running = vw.alive != 0;
    out->cap_w = (int) vw.cap_w;
    out->cap_h = (int) vw.cap_h;
    out->frames = (unsigned long) vw.frames;
    out->presents = (unsigned long) vw.presents;
    out->dropped = (unsigned long) vw.dropped;
}

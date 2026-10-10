/*
 * The mouse and keyboard's OWN names, for the co-op controller routing (platform/input_names.h).
 *
 * Raw input lists every mouse and keyboard Windows reads from; each HID one can be opened for
 * QUERY ONLY (no access rights -- Windows holds mice and keyboards exclusively for itself, so a
 * read or write open would fail) and asked its product string. One device usually shows up as
 * several collections, so entries are merged by vendor/product id.
 *
 * WHICH ONE, when there are several. A gaming mouse commonly ALSO reports a keyboard collection
 * (its macro keys) and a keyboard may report a mouse one (mouse keys), so "the first keyboard" can
 * be a mouse. MEASURED 2026-09-28 on SKYNET: the Logitech mouse receiver ("USB Receiver") and the
 * Keychron keyboard receiver ("Keychron Link") BOTH report a keyboard and a mouse, and the co-op
 * dialog named both rows "USB Receiver". So the keyboard is the one last TYPED ON (a background
 * raw-input watcher, keyboard class only), and the mouse is picked from the OTHER devices. Without
 * any typing yet (or injected keys, which carry no device), each pick prefers a device that is only
 * a mouse (only a keyboard), then one whose name says so; the keyboard also prefers a device other
 * than the mouse just named; then the first seen. The co-op dialog re-reads the names while it is
 * open, so its rows correct themselves at the first real key press.
 */
#include "platform/input_names.h"
#include "local_host_win.h"

#include <windows.h>
#include <hidsdi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MAX_DEVICES 32

typedef struct {
    USHORT vid, pid;
    bool mouse, keyboard;
    char name[128];
} input_dev_t;

/* The raw input handle of the keyboard that sent the last key, set by the watcher thread. */
static HANDLE volatile last_keyboard;

static LRESULT CALLBACK watch_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        RAWINPUTHEADER head;
        UINT size = sizeof(head);
        if (GetRawInputData((HRAWINPUT) lp, RID_HEADER, &head, &size, sizeof(RAWINPUTHEADER)) == sizeof(head) &&
            head.dwType == RIM_TYPEKEYBOARD && head.hDevice != NULL &&
            InterlockedExchangePointer((PVOID volatile *) &last_keyboard, head.hDevice) != head.hDevice) {
            /* A different keyboard than last time (or the first): say which, so the naming can
             * be checked -- on a desk where the fallback already names it right, this line is the
             * only sign the watcher works. The path carries the vendor and product ids. */
            char path[256] = "?";
            UINT len = sizeof(path);
            if (GetRawInputDeviceInfoA(head.hDevice, RIDI_DEVICENAME, path, &len) == (UINT) -1) {
                strcpy(path, "?");
            }
            local_host_log("input-names: typed on %s", path);
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);   /* WM_INPUT needs it for the system's cleanup */
}

static DWORD WINAPI watch_thread(LPVOID unused) {
    (void) unused;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = watch_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"EclipseKeyboardWatch";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (hwnd == NULL) {
        local_host_log("input-names: keyboard watcher NOT started (window: error %lu)", GetLastError());
        return 1;
    }
    /* Keyboard class only: SDL2 (2.32.10) registers the mouse class for relative mode and the pad
     * classes for its raw input joysticks -- a second registration of either class would take
     * those messages away from SDL. INPUTSINK: note keys typed while another window has focus. */
    RAWINPUTDEVICE rid = {0x01, 0x06, RIDEV_INPUTSINK, hwnd};
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        local_host_log("input-names: keyboard watcher NOT started (raw input: error %lu)", GetLastError());
        DestroyWindow(hwnd);
        return 1;
    }
    local_host_log("input-names: keyboard watcher started (raw input, keyboard class)");
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        DispatchMessageW(&m);
    }
    return 0;
}

void input_names_start(void) {
    static LONG started;
    if (InterlockedExchange(&started, 1) != 0) {
        return;
    }
    HANDLE t = CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
    if (t != NULL) {
        CloseHandle(t);
    }
}

/** A HID device's product string and ids, from its raw input device path. */
static bool query_hid(const wchar_t *path, input_dev_t *dev) {
    if (wcsncmp(path, L"\\\\?\\HID", 7) != 0) {
        return false;   /* PS/2, RDP and other non-HID sources have no product string */
    }
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    HIDD_ATTRIBUTES attr;
    attr.Size = sizeof(attr);
    wchar_t product[126] = L"";
    const bool ok = HidD_GetAttributes(h, &attr) && HidD_GetProductString(h, product, sizeof(product)) &&
                    product[0] != L'\0';
    CloseHandle(h);
    if (!ok) {
        return false;
    }
    dev->vid = attr.VendorID;
    dev->pid = attr.ProductID;
    WideCharToMultiByte(CP_UTF8, 0, product, -1, dev->name, (int) sizeof(dev->name), NULL, NULL);
    dev->name[sizeof(dev->name) - 1] = '\0';
    /* Some devices pad the string: Keychron's is "Keychron Link " (a double space before
     * "(Keyboard)" in the co-op dialog, 2026-09-28). */
    size_t n = strlen(dev->name);
    while (n > 0 && (dev->name[n - 1] == ' ' || dev->name[n - 1] == '\t')) {
        dev->name[--n] = '\0';
    }
    return n > 0;
}

/* Product strings never change, and asking for one is a request to the device, so each device
 * path is asked ONCE: the co-op dialog re-reads the names every couple of seconds. UI thread only. */
#define NAME_CACHE_MAX 32
static struct {
    wchar_t path[512];
    bool ok;
    input_dev_t dev;
} name_cache[NAME_CACHE_MAX];
static int name_cache_count;

static bool query_hid_cached(const wchar_t *path, input_dev_t *dev) {
    for (int i = 0; i < name_cache_count; i++) {
        if (wcscmp(name_cache[i].path, path) == 0) {
            *dev = name_cache[i].dev;
            return name_cache[i].ok;
        }
    }
    const bool ok = query_hid(path, dev);
    /* Only answers are cached: a device still being set up by Windows can fail its first
     * query, and a cached failure would leave it unnamed for the life of the process. */
    if (ok && name_cache_count < NAME_CACHE_MAX && wcslen(path) < 512) {
        wcscpy(name_cache[name_cache_count].path, path);
        name_cache[name_cache_count].ok = ok;
        name_cache[name_cache_count].dev = *dev;
        name_cache_count++;
    }
    return ok;
}

static bool name_says(const char *name, const char *word) {
    char lower[128];
    size_t i = 0;
    for (; name[i] != '\0' && i < sizeof(lower) - 1; i++) {
        lower[i] = (char) ((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]);
    }
    lower[i] = '\0';
    return strstr(lower, word) != NULL;
}

bool input_device_name(bool keyboard, char *out, size_t len) {
    if (out == NULL || len == 0) {
        return false;
    }
    input_names_start();   /* no-op after the first call; covers the setting being turned on live */
    UINT count = 0;
    if (GetRawInputDeviceList(NULL, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0) {
        return false;
    }
    RAWINPUTDEVICELIST *list = calloc(count, sizeof(*list));
    if (list == NULL) {
        return false;
    }
    const UINT got = GetRawInputDeviceList(list, &count, sizeof(RAWINPUTDEVICELIST));
    input_dev_t devs[MAX_DEVICES];
    int ndev = 0;
    for (UINT i = 0; got != (UINT) -1 && i < got && ndev < MAX_DEVICES; i++) {
        if (list[i].dwType != RIM_TYPEMOUSE && list[i].dwType != RIM_TYPEKEYBOARD) {
            continue;
        }
        wchar_t path[512];
        UINT size = sizeof(path) / sizeof(path[0]);
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, path, &size) == (UINT) -1) {
            continue;
        }
        input_dev_t dev;
        memset(&dev, 0, sizeof(dev));
        if (!query_hid_cached(path, &dev)) {
            continue;
        }
        int at = -1;
        for (int k = 0; k < ndev; k++) {
            if (devs[k].vid == dev.vid && devs[k].pid == dev.pid) {
                at = k;
                break;
            }
        }
        if (at < 0) {
            at = ndev++;
            devs[at] = dev;
        }
        if (list[i].dwType == RIM_TYPEMOUSE) {
            devs[at].mouse = true;
        } else {
            devs[at].keyboard = true;
        }
    }
    free(list);

    /* The keyboard last typed on: named as THE keyboard, and passed over for the mouse. */
    input_dev_t typed;
    memset(&typed, 0, sizeof(typed));
    bool have_typed = false;
    HANDLE last = InterlockedCompareExchangePointer((PVOID volatile *) &last_keyboard, NULL, NULL);
    if (last != NULL) {
        wchar_t path[512];
        UINT size = sizeof(path) / sizeof(path[0]);
        have_typed = GetRawInputDeviceInfoW(last, RIDI_DEVICENAME, path, &size) != (UINT) -1 && query_hid_cached(path, &typed);
        if (!have_typed && keyboard) {
            /* Typed on a keyboard that has no name to give (a laptop's built-in PS/2 one): say
             * "Keyboard" rather than name a mouse's keyboard collection instead. */
            return false;
        }
    }
    int others = 0;   /* mice that are not the typed-on keyboard */
    for (int k = 0; have_typed && k < ndev; k++) {
        if (devs[k].mouse && !(devs[k].vid == typed.vid && devs[k].pid == typed.pid)) {
            others++;
        }
    }

    /* The mouse first: never the typed-on keyboard while another mouse exists. */
    int mouse = -1, best_score = -1;
    for (int k = 0; k < ndev; k++) {
        if (!devs[k].mouse) {
            continue;
        }
        const bool is_typed = have_typed && devs[k].vid == typed.vid && devs[k].pid == typed.pid;
        if (is_typed && others > 0) {
            continue;
        }
        const int score = (!devs[k].keyboard ? 2 : 0) + (name_says(devs[k].name, "mouse") ? 1 : 0);
        if (score > best_score) {
            best_score = score;
            mouse = k;
        }
    }
    int best = mouse;
    if (keyboard) {
        /* The typed-on one; else a device that is only a keyboard, says so, or at least is not the
         * mouse just named -- one mouse and one keyboard is the usual desk, and two rows naming the
         * same receiver told the user nothing (2026-09-28). */
        best = -1;
        best_score = -1;
        for (int k = 0; k < ndev; k++) {
            if (!devs[k].keyboard) {
                continue;
            }
            const bool is_typed = have_typed && devs[k].vid == typed.vid && devs[k].pid == typed.pid;
            const int score = (is_typed ? 8 : 0) + (!devs[k].mouse ? 2 : 0) +
                              (name_says(devs[k].name, "keyboard") ? 1 : 0) + (k != mouse ? 1 : 0);
            if (score > best_score) {
                best_score = score;
                best = k;
            }
        }
    }
    if (best < 0) {
        return false;
    }
    snprintf(out, len, "%s", devs[best].name);
    return true;
}

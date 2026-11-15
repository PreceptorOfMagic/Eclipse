/*
 * pad_ownership_win.c -- keep a controller assigned to the OTHER co-op PC away from this PC.
 * Contract and reasons: src/app/platform/pad_ownership.h, docs/device-passthrough-design.md.
 *
 * MEASURED FACTS THIS RESTS ON (SKYNET, 2026-10-02 .. 11-03; scripts/padtest/):
 *  - HidHide hides a device only from programs that open it AFTER the hide; a handle opened before
 *    keeps working (901 reports straight through a cloak, virtual DS4).
 *  - A program closed and reopened while the pad is hidden comes back WITHOUT it (Steam).
 *  - After un-hiding, a program that was reopened blind does NOT pick the pad up again by itself.
 *  - NEVER restart the device of an XInput (Xbox) pad that is held: Windows refuses ("System reboot
 *    is needed") and the pad drops off XInput until it is power-cycled. So this file never restarts
 *    devices at all; it closes and reopens the programs instead.
 *  - HidHide's control device \\.\HidHide opens, and answers, for a NON-elevated process.
 *  - HidHide's crash-safe session blacklist (IOCTL 2056) is NOT in the signed v1.5.230 driver
 *    (error 87), so the persistent blacklist is used and a state file undoes it after a crash.
 *
 * HidHide's control interface (MIT, github.com/nefarius/HidHide Shared/HidHideIoctlContract.h):
 * CTL_CODE(32769, 2048.., METHOD_BUFFERED, FILE_READ_DATA); lists are MULTI_SZ of WCHAR; whitelist
 * entries are full image names (\Device\HarddiskVolumeN\...), blacklist entries device instance ids.
 */
#include "platform/pad_ownership.h"

#include <windows.h>
#include <aclapi.h>
#include <cfgmgr32.h>
#include <sddl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "input/steam_triton.h"
#include "local_host_win.h"
#include "logging.h"

#define HH_TYPE 32769
#define HH_CODE(fn) CTL_CODE(HH_TYPE, (fn), METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_HH_GET_WHITELIST HH_CODE(2048)
#define IOCTL_HH_SET_WHITELIST HH_CODE(2049)
#define IOCTL_HH_GET_BLACKLIST HH_CODE(2050)
#define IOCTL_HH_SET_BLACKLIST HH_CODE(2051)
#define IOCTL_HH_GET_ACTIVE HH_CODE(2052)
#define IOCTL_HH_SET_ACTIVE HH_CODE(2053)
#define IOCTL_HH_GET_WLINVERSE HH_CODE(2054)

#define HH_LIST_MAX 32768   /* WCHARs; the lists are short in practice */

static void plog(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    commons_log_info("PadOwn", "%s", line);
    local_host_log("pad-ownership: %s", line);
}

/* ------------------------------------------------------------------ HidHide control device */

static HANDLE hh_open(void) {
    return CreateFileW(L"\\\\.\\HidHide", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

static bool hh_get_bool(HANDLE h, DWORD code, bool *out) {
    BOOLEAN v = FALSE;
    DWORD got = 0;
    if (!DeviceIoControl(h, code, NULL, 0, &v, sizeof(v), &got, NULL) || got != sizeof(v)) {
        return false;
    }
    *out = v != FALSE;
    return true;
}

static bool hh_set_bool(HANDLE h, DWORD code, bool value) {
    BOOLEAN v = value ? TRUE : FALSE;
    DWORD got = 0;
    return DeviceIoControl(h, code, &v, sizeof(v), NULL, 0, &got, NULL) != FALSE;
}

/* MULTI_SZ in/out. @p list holds HH_LIST_MAX WCHARs; @p len_out = WCHARs used incl. the final NUL. */
static bool hh_get_list(HANDLE h, DWORD code, WCHAR *list, size_t *len_out) {
    DWORD needed = 0;
    if (!DeviceIoControl(h, code, NULL, 0, NULL, 0, &needed, NULL)) {
        return false;
    }
    if (needed > HH_LIST_MAX * sizeof(WCHAR)) {
        return false;
    }
    if (needed < 2 * sizeof(WCHAR)) {   /* empty list */
        list[0] = list[1] = 0;
        *len_out = 2;
        return true;
    }
    if (!DeviceIoControl(h, code, NULL, 0, list, needed, &needed, NULL)) {
        return false;
    }
    /* THE LIST ENDS AT ITS FIRST EMPTY STRING, NOT AT `needed`. MEASURED 2026-11-04 on SKYNET (HidHide
     * 1.5.230): the whitelist came back as 438 WCHARs of which 221 were NUL -- three names, their
     * terminator, then ~218 NULs of padding. Taking `needed` as the length made multisz_add append AFTER
     * the padding, past the list's end, where HidHide never reads: "whitelisted this exe: ok" was logged
     * every launch and the entry was never there, so Eclipse lost a hidden pad for good once it
     * dropped (it could not reopen it). Same for the blacklist whenever it was not empty. */
    const size_t n = needed / sizeof(WCHAR);
    size_t end = 0;
    while (end < n && list[end] != 0) {
        end += wcsnlen(list + end, n - end) + 1;
    }
    if (end >= HH_LIST_MAX - 1) {
        return false;
    }
    list[end] = 0;
    if (end == 0) {
        list[1] = 0;                    /* empty: two NULs, as multisz_add expects */
        *len_out = 2;
    } else {
        *len_out = end + 1;             /* the names, each NUL-ended, plus the final NUL */
    }
    return true;
}

static bool hh_set_list(HANDLE h, DWORD code, const WCHAR *list, size_t len) {
    DWORD got = 0;
    return DeviceIoControl(h, code, (LPVOID) list, (DWORD) (len * sizeof(WCHAR)), NULL, 0, &got, NULL) != FALSE;
}

static bool multisz_contains(const WCHAR *list, const WCHAR *item) {
    for (const WCHAR *p = list; *p != 0; p += wcslen(p) + 1) {
        if (_wcsicmp(p, item) == 0) {
            return true;
        }
    }
    return false;
}

/* Append @p item (if absent). @p len counts WCHARs incl. the terminating extra NUL. */
static bool multisz_add(WCHAR *list, size_t *len, const WCHAR *item) {
    if (multisz_contains(list, item)) {
        return true;
    }
    const size_t n = wcslen(item);
    size_t used = *len >= 2 ? *len - 1 : 0;      /* drop the final NUL, append "item\0\0" */
    if (used == 1 && list[0] == 0) {
        used = 0;                                /* "\0\0" is the empty list */
    }
    if (used + n + 2 > HH_LIST_MAX) {
        return false;
    }
    wmemcpy(list + used, item, n);
    list[used + n] = 0;
    list[used + n + 1] = 0;
    *len = used + n + 2;
    return true;
}

static void multisz_remove(WCHAR *list, size_t *len, const WCHAR *item) {
    WCHAR out[HH_LIST_MAX];
    size_t o = 0;
    for (const WCHAR *p = list; *p != 0; p += wcslen(p) + 1) {
        if (_wcsicmp(p, item) != 0) {
            const size_t n = wcslen(p) + 1;
            wmemcpy(out + o, p, n);
            o += n;
        }
    }
    out[o++] = 0;
    if (o == 1) {
        out[o++] = 0;
    }
    wmemcpy(list, out, o);
    *len = o;
}

/* This exe as HidHide's whitelist wants it: \Device\HarddiskVolumeN\path\to\moonlight-tv.exe */
static bool self_image_name(WCHAR *out, size_t out_len) {
    WCHAR path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n < 3 || n >= MAX_PATH || path[1] != L':') {
        return false;
    }
    WCHAR drive[3] = {path[0], L':', 0};
    WCHAR dos[MAX_PATH];
    if (QueryDosDeviceW(drive, dos, MAX_PATH) == 0) {
        return false;
    }
    return _snwprintf(out, out_len, L"%ls%ls", dos, path + 2) > 0;
}

/* ------------------------------------------------------------------ state (crash recovery) */

#define STATE_MAX_ADDED PAD_OWN_MAX_PADS

typedef struct own_state_t {
    bool active;                         /* we have something hidden */
    bool turned_cloak_on;                /* HidHide was off before us */
    int added_count;
    WCHAR added[STATE_MAX_ADDED][320];   /* blacklist entries WE added */
} own_state_t;

static own_state_t g_state;
static SDL_mutex *g_lock;

static SDL_mutex *lock_get(void) {
    if (g_lock == NULL) {
        g_lock = SDL_CreateMutex();
    }
    return g_lock;
}

static bool state_path(WCHAR *out, size_t len) {
    WCHAR base[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return false;
    }
    WCHAR dir[MAX_PATH];
    _snwprintf(dir, MAX_PATH, L"%ls\\Eclipse", base);
    CreateDirectoryW(dir, NULL);
    return _snwprintf(out, len, L"%ls\\pad-ownership.txt", dir) > 0;
}

static void state_save(void) {
    WCHAR path[MAX_PATH];
    if (!state_path(path, MAX_PATH)) {
        return;
    }
    if (!g_state.active) {
        DeleteFileW(path);
        return;
    }
    FILE *f = _wfopen(path, L"w, ccs=UTF-8");
    if (f == NULL) {
        return;
    }
    fwprintf(f, L"pid=%lu\ncloak_was_off=%d\n", GetCurrentProcessId(), g_state.turned_cloak_on ? 1 : 0);
    for (int i = 0; i < g_state.added_count; i++) {
        fwprintf(f, L"added=%ls\n", g_state.added[i]);
    }
    fclose(f);
}

static bool state_load(own_state_t *out, DWORD *pid) {
    WCHAR path[MAX_PATH];
    if (!state_path(path, MAX_PATH)) {
        return false;
    }
    FILE *f = _wfopen(path, L"r, ccs=UTF-8");
    if (f == NULL) {
        return false;
    }
    SDL_zerop(out);
    *pid = 0;
    WCHAR line[400];
    while (fgetws(line, 400, f) != NULL) {
        line[wcscspn(line, L"\r\n")] = 0;
        if (wcsncmp(line, L"pid=", 4) == 0) {
            *pid = (DWORD) wcstoul(line + 4, NULL, 10);
        } else if (wcsncmp(line, L"cloak_was_off=", 14) == 0) {
            out->turned_cloak_on = line[14] == L'1';
        } else if (wcsncmp(line, L"added=", 6) == 0 && out->added_count < STATE_MAX_ADDED) {
            wcsncpy(out->added[out->added_count++], line + 6, 319);
        }
    }
    fclose(f);
    out->active = out->added_count > 0 || out->turned_cloak_on;
    return true;
}

/* Undo @p st in HidHide: remove our entries, put the cloak back the way we found it. */
static bool revert(const own_state_t *st) {
    HANDLE h = hh_open();
    if (h == INVALID_HANDLE_VALUE) {
        plog("revert: HidHide control device not available (%lu)", GetLastError());
        return false;
    }
    static WCHAR list[HH_LIST_MAX];
    size_t len = 0;
    bool ok = true;
    if (st->added_count > 0) {
        if (hh_get_list(h, IOCTL_HH_GET_BLACKLIST, list, &len)) {
            for (int i = 0; i < st->added_count; i++) {
                multisz_remove(list, &len, st->added[i]);
            }
            ok = hh_set_list(h, IOCTL_HH_SET_BLACKLIST, list, len) && ok;
        } else {
            ok = false;
        }
    }
    if (st->turned_cloak_on) {
        ok = hh_set_bool(h, IOCTL_HH_SET_ACTIVE, false) && ok;
    }
    CloseHandle(h);
    plog("revert: %d device(s) un-hidden, cloak %s -> %s", st->added_count,
         st->turned_cloak_on ? "restored to OFF" : "left as it was", ok ? "ok" : "FAILED");
    return ok;
}

/* ------------------------------------------------------------------ public: support, resolve, hide */

bool pad_own_supported(char *why, size_t why_len) {
    HANDLE h = hh_open();
    if (h == INVALID_HANDLE_VALUE) {
        if (why != NULL) {
            SDL_strlcpy(why, "HidHide is not installed, so this PC's own games and Steam will also see the "
                             "controllers assigned to the other PC.", why_len);
        }
        return false;
    }
    bool inverse = false;
    const bool ok = hh_get_bool(h, IOCTL_HH_GET_WLINVERSE, &inverse);
    CloseHandle(h);
    if (!ok) {
        if (why != NULL) {
            SDL_strlcpy(why, "HidHide did not answer, so controllers cannot be kept away from this PC.", why_len);
        }
        return false;
    }
    if (inverse) {
        if (why != NULL) {
            SDL_strlcpy(why, "HidHide is set to its inverse application list, which Eclipse does not change. "
                             "Turn \"Inverse application list\" off in HidHide's settings.", why_len);
        }
        return false;
    }
    return true;
}

/* \\?\HID#VID_057E&PID_2009#8&...#{4d1e55b2-...} -> HID\VID_057E&PID_2009\8&... */
static bool interface_path_to_instance(const char *path, char *out, size_t len) {
    if (path == NULL) {
        return false;
    }
    const char *p = path;
    if (strncmp(p, "\\\\?\\", 4) == 0 || strncmp(p, "\\\\.\\", 4) == 0) {
        p += 4;
    }
    /* The LAST "#{" starts the interface class: a Bluetooth pad's id has one of its own
     * (HID#{00001124-...}_VID&0002057e_PID&2009#b&...#{4d1e55b2-...}). */
    const char *end = NULL;
    for (const char *c = strstr(p, "#{"); c != NULL; c = strstr(c + 2, "#{")) {
        end = c;
    }
    if (end == NULL || (size_t) (end - p) + 1 > len) {
        return false;
    }
    size_t n = 0;
    for (const char *c = p; c < end; c++) {
        out[n++] = *c == '#' ? '\\' : (char) toupper((unsigned char) *c);
    }
    out[n] = 0;
    DEVINST dev;
    return CM_Locate_DevNodeA(&dev, out, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS;
}

bool pad_own_resolve(SDL_JoystickID id, const char *name, pad_own_pad_t *out) {
    SDL_zerop(out);
    SDL_strlcpy(out->name, name != NULL ? name : "Controller", sizeof(out->name));
    char path[512];
    if (steam_triton_device_path(id, path, sizeof(path))) {
        out->kind = PAD_OWN_KIND_STEAM_SLOT;
        return interface_path_to_instance(path, out->instance, sizeof(out->instance));
    }
#if SDL_VERSION_ATLEAST(2, 24, 0)
    SDL_Joystick *joy = SDL_JoystickFromInstanceID(id);
    const char *jpath = joy != NULL ? SDL_JoystickPath(joy) : NULL;
    if (jpath != NULL && interface_path_to_instance(jpath, out->instance, sizeof(out->instance))) {
        out->kind = strstr(out->instance, "&IG_") != NULL ? PAD_OWN_KIND_XBOX : PAD_OWN_KIND_HID;
        return true;
    }
    plog("cannot identify '%s' (SDL path '%s'): it cannot be hidden", out->name, jpath != NULL ? jpath : "none");
#endif
    return false;
}

/* What the last pad_own_hide() was asked to hide (see pad_own_last_hidden). Under lock_get(). */
static pad_own_pad_t last_hidden[PAD_OWN_MAX_PADS];
static int last_hidden_count;

int pad_own_last_hidden(pad_own_pad_t *out, int max) {
    SDL_LockMutex(lock_get());
    const int n = last_hidden_count < max ? last_hidden_count : max;
    for (int i = 0; i < n; i++) {
        out[i] = last_hidden[i];
    }
    SDL_UnlockMutex(lock_get());
    return n;
}

bool pad_own_hide(const pad_own_pad_t *pads, int count, char *err, size_t err_len) {
    SDL_LockMutex(lock_get());
    last_hidden_count = 0;
    for (int i = 0; i < count && i < PAD_OWN_MAX_PADS; i++) {
        last_hidden[last_hidden_count++] = pads[i];
    }
    HANDLE h = hh_open();
    bool ok = h != INVALID_HANDLE_VALUE;
    static WCHAR list[HH_LIST_MAX];
    size_t len = 0;
    if (ok) {
        /* Eclipse itself must keep seeing the pads it forwards: whitelist this exe (persistent and
         * harmless -- HidHide's own client does the same for itself). */
        WCHAR self[MAX_PATH * 2];
        if (self_image_name(self, MAX_PATH * 2) && hh_get_list(h, IOCTL_HH_GET_WHITELIST, list, &len)) {
            if (!multisz_contains(list, self)) {
                ok = multisz_add(list, &len, self) && hh_set_list(h, IOCTL_HH_SET_WHITELIST, list, len);
                plog("whitelisted this exe for HidHide: %s", ok ? "ok" : "FAILED");
            }
        } else {
            ok = false;
        }
    }
    if (ok && hh_get_list(h, IOCTL_HH_GET_BLACKLIST, list, &len)) {
        for (int i = 0; i < count && ok; i++) {
            WCHAR inst[320];
            MultiByteToWideChar(CP_UTF8, 0, pads[i].instance, -1, inst, 320);
            if (multisz_contains(list, inst)) {
                continue;                        /* already hidden (by us earlier, or by the user) */
            }
            ok = multisz_add(list, &len, inst);
            if (ok && g_state.added_count < STATE_MAX_ADDED) {
                wcsncpy(g_state.added[g_state.added_count++], inst, 319);
            }
        }
        ok = ok && hh_set_list(h, IOCTL_HH_SET_BLACKLIST, list, len);
    } else {
        ok = false;
    }
    bool active = false;
    if (ok && hh_get_bool(h, IOCTL_HH_GET_ACTIVE, &active) && !active) {
        ok = hh_set_bool(h, IOCTL_HH_SET_ACTIVE, true);
        g_state.turned_cloak_on = ok || g_state.turned_cloak_on;
    }
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
    }
    g_state.active = g_state.added_count > 0 || g_state.turned_cloak_on;
    state_save();
    SDL_UnlockMutex(lock_get());
    for (int i = 0; i < count; i++) {
        plog("hide %s '%s' %s", pads[i].kind == PAD_OWN_KIND_XBOX ? "xbox" :
                                pads[i].kind == PAD_OWN_KIND_STEAM_SLOT ? "steam-slot" : "hid",
             pads[i].name, pads[i].instance);
    }
    plog("hide: %s", ok ? "ok" : "FAILED");
    if (!ok && err != NULL) {
        SDL_strlcpy(err, "HidHide refused the change.", err_len);
    }
    return ok;
}

bool pad_own_active(void) {
    return g_state.active;
}

/* ------------------------------------------------------------------ scan: who already holds them */

typedef struct {
    PVOID Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
} sys_handle_t;
typedef struct {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    sys_handle_t Handles[1];
} sys_handles_t;
typedef NTSTATUS(NTAPI *nt_qsi_t)(ULONG, PVOID, ULONG, PULONG);
typedef NTSTATUS(NTAPI *nt_qo_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
#define SYS_EXTENDED_HANDLE_INFORMATION 64
#define OBJECT_NAME_INFORMATION_CLASS 1
#define STATUS_INFO_LENGTH_MISMATCH_ ((NTSTATUS) 0xC0000004L)

static nt_qo_t p_nt_query_object;

typedef struct {
    HANDLE h;
    WCHAR name[512];
    bool ok;
} name_query_t;

static DWORD WINAPI name_thread(LPVOID arg) {
    name_query_t *q = arg;
    BYTE buf[2048];
    ULONG len = 0;
    if (p_nt_query_object(q->h, OBJECT_NAME_INFORMATION_CLASS, buf, sizeof(buf), &len) >= 0) {
        const UNICODE_STRING *u = (const UNICODE_STRING *) buf;
        const size_t n = u->Buffer != NULL ? SDL_min((size_t) u->Length / 2, (size_t) 511) : 0;
        wmemcpy(q->name, u->Buffer != NULL ? u->Buffer : L"", n);
        q->name[n] = 0;
        q->ok = true;
    }
    return 0;
}

/* The kernel object names of the hidden pads (their PDOs, e.g. \Device\000000f1). */
static int pad_pdo_names(WCHAR names[][128], int max) {
    int n = 0;
    for (int i = 0; i < g_state.added_count && n < max; i++) {
        DEVINST dev;
        if (CM_Locate_DevNodeW(&dev, g_state.added[i], CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
            continue;
        }
        ULONG size = sizeof(names[n]);
        if (CM_Get_DevNode_Registry_PropertyW(dev, CM_DRP_PHYSICAL_DEVICE_OBJECT_NAME, NULL, names[n], &size, 0)
            == CR_SUCCESS) {
            n++;
        }
    }
    return n;
}

static const struct {
    const char *exe;
    const char *display;
    int how;          /* 1 = Steam (-shutdown / -silent), 2 = terminate (Windows restarts it), 3 = WM_CLOSE + reopen */
} known_apps[] = {
    {"steam.exe", "Steam", 1},
    {"GameBar.exe", "Xbox Game Bar", 2},
    {"GameBarFTServer.exe", "Xbox Game Bar", 2},
    {"DS4Windows.exe", "DS4Windows", 3},
    {"DSX.exe", "DSX", 3},
    {"x360ce.exe", "x360ce", 3},
    {"JoyToKey.exe", "JoyToKey", 3},
    {"antimicrox.exe", "AntiMicroX", 3},
};

/* Shell pieces that hold every pad and must never be closed. They are not listed: none of them turns a button
 * press into an action in a game. SERVICES ARE NOT HERE: a service holding a pad is found and named by the scan
 * (services_of), whatever it is called -- the old entries for svchost, XboxService and the GameInput services
 * hid exactly the holder that kept passing the Xbox pad to SKYNET's games. */
static const char *ignored_apps[] = {"dwm.exe", "explorer.exe", "csrss.exe", "System", "moonlight-tv.exe",
                                     "TextInputHost.exe", "ShellExperienceHost.exe"};

static int known_index(const char *exe) {
    for (size_t i = 0; i < SDL_arraysize(known_apps); i++) {
        if (SDL_strcasecmp(known_apps[i].exe, exe) == 0) {
            return (int) i;
        }
    }
    return -1;
}

static bool is_ignored(const char *exe) {
    for (size_t i = 0; i < SDL_arraysize(ignored_apps); i++) {
        if (SDL_strcasecmp(ignored_apps[i], exe) == 0) {
            return true;
        }
    }
    return false;
}

/* SERVICES ARE FOUND FROM WHAT HOLDS THE PAD, NOT BY NAME (user 2026-11-12: "we also can't be just hardcoding
 * each services we need to be finding them correctly dynamically"). The scan names the PROCESS holding a hidden
 * pad; this maps a process id to the Windows services it hosts. MEASURED 2026-11-04 on SKYNET (elevated holder
 * scan): GameInputRedistService held the Xbox pad and kept passing it to SKYNET's games after the hide -- one
 * instance of the general case this covers. Listing services and their process ids needs no administrator. */
typedef struct svc_info_t {
    DWORD pid;
    WCHAR name[128];
    WCHAR display[128];
    bool stoppable;
} svc_info_t;

#define SVC_MAX 512
static svc_info_t g_svcs[SVC_MAX];
static int g_svc_count;

static void services_load(void) {
    g_svc_count = 0;
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
    if (scm == NULL) {
        return;
    }
    DWORD needed = 0, count = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE, NULL, 0, &needed, &count,
                          &resume, NULL);
    BYTE *buf = needed > 0 ? SDL_malloc(needed) : NULL;
    resume = 0;
    if (buf != NULL && EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE, buf, needed,
                                             &needed, &count, &resume, NULL)) {
        const ENUM_SERVICE_STATUS_PROCESSW *svc = (const ENUM_SERVICE_STATUS_PROCESSW *) buf;
        for (DWORD i = 0; i < count && g_svc_count < SVC_MAX; i++) {
            if (svc[i].ServiceStatusProcess.dwProcessId == 0) {
                continue;
            }
            svc_info_t *o = &g_svcs[g_svc_count++];
            o->pid = svc[i].ServiceStatusProcess.dwProcessId;
            wcsncpy(o->name, svc[i].lpServiceName, 127);
            o->name[127] = 0;
            wcsncpy(o->display, svc[i].lpDisplayName != NULL ? svc[i].lpDisplayName : svc[i].lpServiceName, 127);
            o->display[127] = 0;
            o->stoppable = (svc[i].ServiceStatusProcess.dwControlsAccepted & SERVICE_ACCEPT_STOP) != 0;
        }
    }
    SDL_free(buf);
    CloseServiceHandle(scm);
}

/* How many services process @p pid hosts; @p first gets the first of them. */
static int services_of(DWORD pid, const svc_info_t **first) {
    int n = 0;
    *first = NULL;
    for (int i = 0; i < g_svc_count; i++) {
        if (g_svcs[i].pid == pid) {
            if (n++ == 0) {
                *first = &g_svcs[i];
            }
        }
    }
    return n;
}

static bool token_elevated(HANDLE process) {
    HANDLE tok = NULL;
    if (!OpenProcessToken(process, TOKEN_QUERY, &tok)) {
        return false;
    }
    TOKEN_ELEVATION e;
    DWORD got = 0;
    const bool elevated = GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &got) && e.TokenIsElevated != 0;
    CloseHandle(tok);
    return elevated;
}

bool pad_own_elevated(void) {
    return token_elevated(GetCurrentProcess());
}

static bool pid_elevated(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == NULL) {
        return true;                     /* not even its basic information: another user, or protected */
    }
    const bool e = token_elevated(h);
    CloseHandle(h);
    return e;
}

/* An elevated scan may open every process but the protected ones: switch the debug privilege on (it is in
 * an administrator's token, off by default). */
static void enable_debug_privilege(void) {
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        return;
    }
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(NULL, L"SeDebugPrivilege", &tp.Privileges[0].Luid)) {
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL);
    }
    CloseHandle(tok);
}

/* ------------------------------------------------------------------ driver routes past HidHide */

/* MEASURED 2026-11-12 on SKYNET: Steam kept reading the Xbox pad given to Vega with NO handle to any of the pad's
 * devices. Its kernel driver steamxbox.sys is a LOWER filter on the HID, XboxComposite and XnaComposite classes
 * (HidHide is an UPPER filter there), and hands the pad's data to Steam through its own control device,
 * \Device\nvagipmanControl, whose name is in the driver's image. With that device locked to SYSTEM and
 * Administrators and Steam reopened, Steam read the pad the ordinary way (HID handles) -- which HidHide blocks
 * -- and the pad reached Vega only. Found WITHOUT names (user 2026-11-12: "we need to be finding them correctly
 * dynamically"): the filter drivers on each hidden pad's stack, not Microsoft's (company field) and not HidHide
 * (which Eclipse itself uses), whose image names a \Device\ that exists. */
#define ROUTE_MAX 4
typedef struct route_t {
    WCHAR dev[160];
    char display[96];
} route_t;
static route_t g_routes[ROUTE_MAX];
static int g_route_count;
static WCHAR g_extra[ROUTE_MAX][160];   /* locked control devices: their holders are pad holders (pad_own_scan) */
static int g_extra_count;

typedef struct lock_t {
    WCHAR dev[160];
    WCHAR orig[1024];                   /* the device's own security descriptor, to put back */
} lock_t;
static lock_t g_locks[ROUTE_MAX];
static int g_lock_count;

#define LOCK_SDDL L"D:P(A;;FA;;;SY)(A;;FA;;;BA)"

static void report_add(char *report, size_t len, const char *fmt, ...);

static void name_add(WCHAR list[][64], int *n, int max, const WCHAR *s) {
    for (int i = 0; i < *n; i++) {
        if (_wcsicmp(list[i], s) == 0) {
            return;
        }
    }
    if (*n < max) {
        wcsncpy(list[*n], s, 63);
        list[(*n)++][63] = 0;
    }
}

static void multisz_names(const WCHAR *buf, WCHAR list[][64], int *n, int max) {
    for (const WCHAR *p = buf; *p != 0; p += wcslen(p) + 1) {
        name_add(list, n, max, p);
    }
}

/* The filter drivers of one device node: its own and its class's, upper and lower. */
static void filters_of(DEVINST d, WCHAR list[][64], int *n, int max) {
    static WCHAR buf[2048];
    const ULONG props[2] = {CM_DRP_UPPERFILTERS, CM_DRP_LOWERFILTERS};
    for (int i = 0; i < 2; i++) {
        ULONG sz = sizeof(buf) - 4;
        SDL_memset(buf, 0, sizeof(buf));
        if (CM_Get_DevNode_Registry_PropertyW(d, props[i], NULL, buf, &sz, 0) == CR_SUCCESS) {
            multisz_names(buf, list, n, max);
        }
    }
    WCHAR guid[64];
    ULONG gsz = sizeof(guid);
    if (CM_Get_DevNode_Registry_PropertyW(d, CM_DRP_CLASSGUID, NULL, guid, &gsz, 0) != CR_SUCCESS) {
        return;
    }
    WCHAR key[160];
    _snwprintf(key, 160, L"SYSTEM\\CurrentControlSet\\Control\\Class\\%ls", guid);
    const WCHAR *vals[2] = {L"UpperFilters", L"LowerFilters"};
    for (int i = 0; i < 2; i++) {
        DWORD sz = sizeof(buf) - 4;
        SDL_memset(buf, 0, sizeof(buf));
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, vals[i], RRF_RT_REG_MULTI_SZ, NULL, buf, &sz) == ERROR_SUCCESS) {
            multisz_names(buf, list, n, max);
        }
    }
}

/* A driver service's image file, as a normal path. */
static bool driver_file(const WCHAR *svc, WCHAR *out, size_t len) {
    WCHAR key[200], ip[MAX_PATH] = L"", win[MAX_PATH];
    _snwprintf(key, 200, L"SYSTEM\\CurrentControlSet\\Services\\%ls", svc);
    DWORD sz = sizeof(ip);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"ImagePath", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, NULL,
                     ip, &sz) != ERROR_SUCCESS) {
        _snwprintf(ip, MAX_PATH, L"System32\\drivers\\%ls.sys", svc);
    }
    if (GetWindowsDirectoryW(win, MAX_PATH) == 0) {
        return false;
    }
    if (_wcsnicmp(ip, L"\\SystemRoot\\", 12) == 0) {
        _snwprintf(out, len, L"%ls\\%ls", win, ip + 12);
    } else if (wcsncmp(ip, L"\\??\\", 4) == 0) {
        _snwprintf(out, len, L"%ls", ip + 4);
    } else if (ip[0] != 0 && ip[1] == L':') {
        _snwprintf(out, len, L"%ls", ip);
    } else {
        _snwprintf(out, len, L"%ls\\%ls", win, ip);
    }
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

typedef DWORD(WINAPI *gfvis_t)(LPCWSTR, LPDWORD);
typedef BOOL(WINAPI *gfvi_t)(LPCWSTR, DWORD, DWORD, LPVOID);
typedef BOOL(WINAPI *vqv_t)(LPCVOID, LPCWSTR, LPVOID *, PUINT);

/* One string of a file's version resource ("CompanyName", "FileDescription"). version.dll is loaded on demand. */
static bool version_string(const WCHAR *path, const WCHAR *what, WCHAR *out, size_t len) {
    static HMODULE ver;
    if (ver == NULL) {
        ver = LoadLibraryW(L"version.dll");
    }
    gfvis_t size_of = ver ? (gfvis_t) (void *) GetProcAddress(ver, "GetFileVersionInfoSizeW") : NULL;
    gfvi_t info_of = ver ? (gfvi_t) (void *) GetProcAddress(ver, "GetFileVersionInfoW") : NULL;
    vqv_t query = ver ? (vqv_t) (void *) GetProcAddress(ver, "VerQueryValueW") : NULL;
    out[0] = 0;
    if (size_of == NULL || info_of == NULL || query == NULL) {
        return false;
    }
    DWORD dummy = 0;
    const DWORD n = size_of(path, &dummy);
    BYTE *buf = n > 0 ? SDL_malloc(n) : NULL;
    bool ok = false;
    if (buf != NULL && info_of(path, 0, n, buf)) {
        WORD *tr = NULL;
        UINT trlen = 0;
        if (query(buf, L"\\VarFileInfo\\Translation", (LPVOID *) &tr, &trlen) && trlen >= 4) {
            WCHAR sub[96];
            _snwprintf(sub, 96, L"\\StringFileInfo\\%04x%04x\\%ls", tr[0], tr[1], what);
            WCHAR *val = NULL;
            UINT vlen = 0;
            if (query(buf, sub, (LPVOID *) &val, &vlen) && val != NULL) {
                wcsncpy(out, val, len - 1);
                out[len - 1] = 0;
                ok = true;
            }
        }
    }
    SDL_free(buf);
    return ok;
}

typedef struct {
    UNICODE_STRING Name;
    UNICODE_STRING TypeName;
} obj_dir_info_t;
typedef NTSTATUS(NTAPI *nt_odo_t)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
typedef NTSTATUS(NTAPI *nt_qdo_t)(HANDLE, PVOID, ULONG, BOOLEAN, BOOLEAN, PULONG, PULONG);

/* Every \Device\ object whose full name appears (UTF-16) in @p image: candidates for @p display's route. */
static void devices_named_in(const BYTE *image, size_t n, const char *display) {
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    nt_odo_t open_dir = (nt_odo_t) (void *) GetProcAddress(nt, "NtOpenDirectoryObject");
    nt_qdo_t query_dir = (nt_qdo_t) (void *) GetProcAddress(nt, "NtQueryDirectoryObject");
    if (open_dir == NULL || query_dir == NULL) {
        return;
    }
    WCHAR root[] = L"\\Device";
    UNICODE_STRING us = {(USHORT) (wcslen(root) * 2), (USHORT) sizeof(root), root};
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &us, 0, NULL, NULL);
    HANDLE dir = NULL;
    if (open_dir(&dir, 0x0001 /* DIRECTORY_QUERY */, &oa) < 0) {
        return;
    }
    static BYTE buf[4096];
    ULONG ctx = 0, got = 0;
    BOOLEAN first = TRUE;
    while (g_route_count < ROUTE_MAX && query_dir(dir, buf, sizeof(buf), TRUE, first, &ctx, &got) >= 0) {
        first = FALSE;
        const obj_dir_info_t *e = (const obj_dir_info_t *) buf;
        if (e->Name.Buffer == NULL || e->TypeName.Buffer == NULL || e->Name.Length < 8 ||
            _wcsnicmp(e->TypeName.Buffer, L"Device", 6) != 0) {
            continue;
        }
        WCHAR full[160];
        _snwprintf(full, 160, L"\\Device\\%.*ls", (int) (e->Name.Length / 2), e->Name.Buffer);
        const size_t k = wcslen(full) * 2;
        for (size_t i = 0; i + k <= n; i += 2) {
            if (image[i] == (BYTE) full[0] && memcmp(image + i, full, k) == 0) {
                route_t *r = &g_routes[g_route_count++];
                wcsncpy(r->dev, full, 159);
                r->dev[159] = 0;
                SDL_strlcpy(r->display, display, sizeof(r->display));
                break;
            }
        }
    }
    CloseHandle(dir);
}

static void routes_discover(void) {
    g_route_count = 0;
    WCHAR names[32][64];
    int nn = 0;
    for (int i = 0; i < g_state.added_count; i++) {
        DEVINST d;
        if (CM_Locate_DevNodeW(&d, g_state.added[i], CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
            continue;
        }
        for (int up = 0; up < 3; up++) {          /* the pad's node and two parents (a USB interface, a receiver) */
            filters_of(d, names, &nn, 32);
            DEVINST parent;
            if (CM_Get_Parent(&parent, d, 0) != CR_SUCCESS) {
                break;
            }
            d = parent;
        }
    }
    for (int i = 0; i < nn && g_route_count < ROUTE_MAX; i++) {
        if (_wcsicmp(names[i], L"HidHide") == 0) {
            continue;                            /* Eclipse's own tool: its control device must stay open */
        }
        WCHAR path[MAX_PATH], company[128], desc[128];
        if (!driver_file(names[i], path, MAX_PATH)) {
            continue;
        }
        version_string(path, L"CompanyName", company, 128);
        if (wcsstr(company, L"Microsoft") != NULL) {
            continue;                            /* Windows' own stack (xinputhid, xboxgip, ...) */
        }
        if (!version_string(path, L"FileDescription", desc, 128) || desc[0] == 0) {
            wcsncpy(desc, names[i], 127);
            desc[127] = 0;
        }
        char display[96];
        WideCharToMultiByte(CP_UTF8, 0, desc, -1, display, sizeof(display), NULL, NULL);
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            continue;
        }
        LARGE_INTEGER size;
        BYTE *image = NULL;
        DWORD read = 0;
        if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (16 << 20) &&
            (image = SDL_malloc((size_t) size.QuadPart)) != NULL &&
            ReadFile(f, image, (DWORD) size.QuadPart, &read, NULL)) {
            const int before = g_route_count;
            devices_named_in(image, read, display);
            for (int k = before; k < g_route_count; k++) {
                plog("route past HidHide: filter driver %ls (\"%s\") has control device %ls", names[i], display,
                     g_routes[k].dev);
            }
        }
        SDL_free(image);
        CloseHandle(f);
    }
}

typedef NTSTATUS(NTAPI *nt_of_t)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);

static HANDLE device_object_open(const WCHAR *name, ACCESS_MASK access) {
    nt_of_t op = (nt_of_t) (void *) GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtOpenFile");
    WCHAR w[160];
    wcsncpy(w, name, 159);
    w[159] = 0;
    UNICODE_STRING us = {(USHORT) (wcslen(w) * 2), (USHORT) (wcslen(w) * 2 + 2), w};
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, NULL, NULL);
    IO_STATUS_BLOCK io;
    HANDLE h = NULL;
    if (op == NULL || op(&h, access, &oa, &io, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0) < 0) {
        return NULL;
    }
    return h;
}

static bool device_sddl(const WCHAR *name, WCHAR *out, size_t len) {
    HANDLE h = device_object_open(name, READ_CONTROL);
    if (h == NULL) {
        return false;
    }
    PSECURITY_DESCRIPTOR sd = NULL;
    LPWSTR s = NULL;
    const SECURITY_INFORMATION si = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    bool ok = GetSecurityInfo(h, SE_KERNEL_OBJECT, si, NULL, NULL, NULL, NULL, &sd) == ERROR_SUCCESS &&
              ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1, si, &s, NULL);
    if (ok) {
        wcsncpy(out, s, len - 1);
        out[len - 1] = 0;
    }
    LocalFree(s);
    LocalFree(sd);
    CloseHandle(h);
    return ok;
}

static bool device_set_dacl(const WCHAR *name, const WCHAR *sddl) {
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, NULL)) {
        return false;
    }
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = NULL;
    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
    HANDLE h = device_object_open(name, READ_CONTROL | WRITE_DAC);
    const bool ok = h != NULL && present &&
                    SetSecurityInfo(h, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                    NULL, NULL, dacl, NULL) == ERROR_SUCCESS;
    if (h != NULL) CloseHandle(h);
    LocalFree(sd);
    return ok;
}

/* The locks, on disk while they stand: an elevated Eclipse that dies leaves the device locked until a reboot
 * (device security is not stored) or the next admin step, which takes the ORIGINAL from here. */
static bool locks_path(WCHAR *out, size_t len) {
    WCHAR state[MAX_PATH];
    if (!state_path(state, MAX_PATH)) {
        return false;
    }
    WCHAR *slash = wcsrchr(state, L'\\');
    if (slash == NULL) {
        return false;
    }
    *slash = 0;
    return _snwprintf(out, len, L"%ls\\pad-ownership-locks.txt", state) > 0;
}

static void locks_save(void) {
    WCHAR path[MAX_PATH];
    if (!locks_path(path, MAX_PATH)) {
        return;
    }
    if (g_lock_count == 0) {
        DeleteFileW(path);
        return;
    }
    FILE *f = _wfopen(path, L"w, ccs=UTF-8");
    if (f == NULL) {
        return;
    }
    for (int i = 0; i < g_lock_count; i++) {
        fwprintf(f, L"%ls\t%ls\n", g_locks[i].dev, g_locks[i].orig);
    }
    fclose(f);
}

/* The original recorded by an earlier run that never restored it (the device may still be locked). */
static bool locks_saved_original(const WCHAR *dev, WCHAR *out, size_t len) {
    WCHAR path[MAX_PATH];
    if (!locks_path(path, MAX_PATH)) {
        return false;
    }
    FILE *f = _wfopen(path, L"r, ccs=UTF-8");
    if (f == NULL) {
        return false;
    }
    WCHAR line[1300];
    bool found = false;
    while (!found && fgetws(line, 1300, f) != NULL) {
        line[wcscspn(line, L"\r\n")] = 0;
        WCHAR *tab = wcschr(line, L'\t');
        if (tab != NULL) {
            *tab = 0;
            if (_wcsicmp(line, dev) == 0) {
                wcsncpy(out, tab + 1, len - 1);
                out[len - 1] = 0;
                found = true;
            }
        }
    }
    fclose(f);
    return found;
}

/* Elevated: lock every discovered route to SYSTEM and Administrators. Its users then read the pad the ordinary
 * way once reopened -- measured for Steam 2026-11-12. */
static void routes_lock(char *report, size_t len) {
    g_lock_count = 0;
    g_extra_count = 0;
    for (int i = 0; i < g_route_count && g_lock_count < ROUTE_MAX; i++) {
        lock_t *l = &g_locks[g_lock_count];
        wcsncpy(l->dev, g_routes[i].dev, 159);
        l->dev[159] = 0;
        if (!locks_saved_original(l->dev, l->orig, 1024) && !device_sddl(l->dev, l->orig, 1024)) {
            plog("admin: route %ls: cannot read its security (%lu) -- left open", l->dev, GetLastError());
            continue;
        }
        const bool ok = device_set_dacl(l->dev, LOCK_SDDL);
        plog("admin: route %ls (\"%s\"): %s; was %ls", l->dev, g_routes[i].display,
             ok ? "locked to SYSTEM and Administrators for the session" : "COULD NOT BE LOCKED", l->orig);
        if (ok) {
            g_lock_count++;
            wcsncpy(g_extra[g_extra_count++], l->dev, 159);
        }
        report_add(report, len, ok ? "%s blocked for this session" : "%s could not be blocked", g_routes[i].display);
    }
    locks_save();
}

static void routes_unlock(const char *who) {
    for (int i = 0; i < g_lock_count; i++) {
        const bool ok = device_set_dacl(g_locks[i].dev, g_locks[i].orig);
        plog("%s: route %ls restored: %s", who, g_locks[i].dev, ok ? "ok" : "FAILED (it resets at the next reboot)");
    }
    g_lock_count = 0;
    locks_save();
}

void pad_own_scan(pad_own_scan_t *out) {
    SDL_zerop(out);
    out->complete = true;
    out->elevated = pad_own_elevated();
    if (out->elevated) {
        enable_debug_privilege();
    }
    services_load();
    routes_discover();
    out->routes = g_route_count;
    for (int i = 0; i < g_route_count && i < 4; i++) {
        SDL_strlcpy(out->route[i], g_routes[i].display, sizeof(out->route[i]));
    }
    WCHAR pdo[PAD_OWN_MAX_PADS][128];
    SDL_LockMutex(lock_get());
    const int npdo = pad_pdo_names(pdo, PAD_OWN_MAX_PADS);
    SDL_UnlockMutex(lock_get());
    if (npdo == 0) {
        return;
    }
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    nt_qsi_t qsi = (nt_qsi_t) (void *) GetProcAddress(nt, "NtQuerySystemInformation");
    p_nt_query_object = (nt_qo_t) (void *) GetProcAddress(nt, "NtQueryObject");
    if (qsi == NULL || p_nt_query_object == NULL) {
        out->complete = false;
        return;
    }
    /* Learn the File object type's index from a handle of our own, so only file handles are
     * duplicated and named (the table holds every handle in the system). */
    WCHAR self_path[MAX_PATH];
    GetModuleFileNameW(NULL, self_path, MAX_PATH);
    HANDLE probe = CreateFileW(self_path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, 0, NULL);
    ULONG size = 1 << 22;
    sys_handles_t *table = NULL;
    NTSTATUS st;
    for (int tries = 0; tries < 6; tries++) {
        SDL_free(table);
        table = SDL_malloc(size);
        if (table == NULL) {
            break;
        }
        ULONG need = 0;
        st = qsi(SYS_EXTENDED_HANDLE_INFORMATION, table, size, &need);
        if (st != STATUS_INFO_LENGTH_MISMATCH_) {
            break;
        }
        size = need > size ? need + (1 << 20) : size * 2;
    }
    if (table == NULL || st < 0) {
        SDL_free(table);
        if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
        out->complete = false;
        return;
    }
    const DWORD self = GetCurrentProcessId();
    USHORT file_type = 0;
    for (ULONG_PTR i = 0; i < table->NumberOfHandles && probe != INVALID_HANDLE_VALUE; i++) {
        if (table->Handles[i].UniqueProcessId == self && (HANDLE) table->Handles[i].HandleValue == probe) {
            file_type = table->Handles[i].ObjectTypeIndex;
            break;
        }
    }
    if (probe != INVALID_HANDLE_VALUE) {
        CloseHandle(probe);
    }

    /* Process names. */
    typedef struct { DWORD pid; char exe[64]; HANDLE h; bool tried; bool hit; bool stalled; } proc_t;
    static proc_t procs[2048];
    int nprocs = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    for (BOOL more = Process32First(snap, &pe); more && nprocs < 2048; more = Process32Next(snap, &pe)) {
        procs[nprocs].pid = pe.th32ProcessID;
        SDL_strlcpy(procs[nprocs].exe, pe.szExeFile, sizeof(procs[nprocs].exe));
        procs[nprocs].h = NULL;
        procs[nprocs].tried = procs[nprocs].hit = procs[nprocs].stalled = false;
        nprocs++;
    }
    CloseHandle(snap);

    int stalls = 0;
    for (ULONG_PTR i = 0; i < table->NumberOfHandles; i++) {
        const sys_handle_t *e = &table->Handles[i];
        const DWORD pid = (DWORD) e->UniqueProcessId;
        if (pid == self || pid <= 4 || (file_type != 0 && e->ObjectTypeIndex != file_type)) {
            continue;
        }
        proc_t *p = NULL;
        for (int k = 0; k < nprocs; k++) {
            if (procs[k].pid == pid) {
                p = &procs[k];
                break;
            }
        }
        if (p == NULL || p->hit || p->stalled || is_ignored(p->exe)) {
            continue;
        }
        if (!p->tried) {
            p->tried = true;
            p->h = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
            if (p->h == NULL) {
                out->unscanned++;
            }
        }
        if (p->h == NULL) {
            continue;
        }
        HANDLE dup = NULL;
        if (!DuplicateHandle(p->h, (HANDLE) e->HandleValue, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            continue;
        }
        if (GetFileType(dup) == FILE_TYPE_PIPE) {   /* a synchronous pipe can block NtQueryObject forever */
            CloseHandle(dup);
            continue;
        }
        /* On the heap: if the query gets stuck the thread outlives this function, and it must
         * not be left writing into a dead stack frame. */
        name_query_t *q = SDL_calloc(1, sizeof(*q));
        if (q == NULL) {
            CloseHandle(dup);
            break;
        }
        q->h = dup;
        HANDLE t = CreateThread(NULL, 0, name_thread, q, 0, NULL);
        if (t == NULL) {
            CloseHandle(dup);
            SDL_free(q);
            out->complete = false;
            break;
        }
        if (WaitForSingleObject(t, 250) == WAIT_TIMEOUT) {
            /* Stuck in the kernel on an odd handle (measured 11-03 on SKYNET: one stall stopped the
             * whole scan after ~0.6 s). Leave that thread, its handle and its buffer alone (never
             * TerminateThread), skip the REST OF THAT PROCESS only, and carry on with the others.
             * Each stall leaves a thread behind, so give up after a few. */
            CloseHandle(t);
            p->stalled = true;
            out->complete = false;
            plog("scan: naming a handle of %s (pid %lu) stalled: that process is skipped", p->exe, pid);
            if (++stalls >= 4) {
                plog("scan: %d stalls, stopping", stalls);
                break;
            }
            continue;
        }
        CloseHandle(t);
        CloseHandle(dup);
        const bool named = q->ok;
        WCHAR qname[512];
        wcsncpy(qname, q->name, 511);
        qname[511] = 0;
        SDL_free(q);
        if (!named) {
            continue;
        }
        for (int k = 0; k < npdo; k++) {
            const size_t pl = wcslen(pdo[k]);
            if (_wcsnicmp(qname, pdo[k], pl) == 0 && (qname[pl] == 0 || qname[pl] == L'\\')) {
                p->hit = true;
                break;
            }
        }
        /* A program holding a locked driver control device read the pad through that driver: it is a holder
         * to close and reopen, so it comes back without the route (routes_lock). */
        for (int k = 0; !p->hit && k < g_extra_count; k++) {
            p->hit = _wcsicmp(qname, g_extra[k]) == 0;
        }
        if (p->hit && out->count < PAD_OWN_MAX_HOLDERS) {
            pad_own_holder_t *hd = &out->holders[out->count++];
            SDL_zerop(hd);
            hd->pid = pid;
            SDL_strlcpy(hd->exe, p->exe, sizeof(hd->exe));
            const int ki = known_index(p->exe);
            const svc_info_t *svc = NULL;
            const int nsvc = services_of(pid, &svc);
            if (nsvc > 0) {
                /* A SERVICE. Restarted only when it is the one service in its process and accepts a stop:
                 * restarting a shared host would stop services that have nothing to do with the pad. */
                hd->admin = true;
                if (nsvc == 1) {
                    WideCharToMultiByte(CP_UTF8, 0, svc->display, -1, hd->display, sizeof(hd->display), NULL, NULL);
                    if (svc->stoppable) {
                        WideCharToMultiByte(CP_UTF8, 0, svc->name, -1, hd->service, sizeof(hd->service), NULL,
                                            NULL);
                        hd->closable = true;
                    }
                } else {
                    snprintf(hd->display, sizeof(hd->display), "%s (%d Windows services)", p->exe, nsvc);
                }
            } else {
                SDL_strlcpy(hd->display, ki >= 0 ? known_apps[ki].display : p->exe, sizeof(hd->display));
                hd->admin = pid_elevated(pid);
                hd->closable = ki >= 0 && !hd->admin;
            }
        }
    }
    for (int k = 0; k < nprocs; k++) {
        if (procs[k].h != NULL) {
            CloseHandle(procs[k].h);
        }
    }
    SDL_free(table);
    plog("scan%s: %d holder(s), %d process(es) not inspectable, %s", out->elevated ? " (administrator)" : "",
         out->count, out->unscanned, out->complete ? "complete" : "INCOMPLETE");
    for (int i = 0; i < out->count; i++) {
        const pad_own_holder_t *hd = &out->holders[i];
        plog("  holder %s (pid %lu) \"%s\"%s%s%s%s", hd->exe, hd->pid, hd->display, hd->admin ? " [admin]" : "",
             hd->service[0] ? " [service " : "", hd->service, hd->service[0] ? ", restartable]" :
                                                               (hd->closable ? " [closable]" : ""));
    }
}

/* ------------------------------------------------------------------ evict: close and reopen */

typedef struct reopened_t {
    int how;
    WCHAR image[MAX_PATH];
    WCHAR cmdline[2048];
} reopened_t;

static reopened_t g_reopened[PAD_OWN_MAX_HOLDERS];
static int g_reopened_count;

static bool wait_exit(DWORD pid, DWORD ms) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (h == NULL) {
        return true;       /* already gone */
    }
    const bool gone = WaitForSingleObject(h, ms) == WAIT_OBJECT_0;
    CloseHandle(h);
    return gone;
}

static bool run(const WCHAR *cmdline) {
    WCHAR buf[2048];
    wcsncpy(buf, cmdline, 2047);
    buf[2047] = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SDL_zero(si);
    si.cb = sizeof(si);
    if (!CreateProcessW(NULL, buf, NULL, NULL, FALSE, CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS, NULL, NULL,
                        &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static BOOL CALLBACK close_windows_of(HWND w, LPARAM pid) {
    DWORD owner = 0;
    GetWindowThreadProcessId(w, &owner);
    if (owner == (DWORD) pid) {
        PostMessageW(w, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

typedef NTSTATUS(NTAPI *nt_qip_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);

static void command_line_of(DWORD pid, const WCHAR *image, WCHAR *out, size_t len) {
    _snwprintf(out, len, L"\"%ls\"", image);
    nt_qip_t qip = (nt_qip_t) (void *) GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (qip == NULL || h == NULL) {
        if (h != NULL) CloseHandle(h);
        return;
    }
    BYTE buf[4096];
    ULONG got = 0;
    if (qip(h, 60 /* ProcessCommandLineInformation */, buf, sizeof(buf), &got) >= 0) {
        const UNICODE_STRING *u = (const UNICODE_STRING *) buf;
        if (u->Buffer != NULL && u->Length > 0) {
            const size_t n = SDL_min((size_t) u->Length / 2, len - 1);
            wmemcpy(out, u->Buffer, n);
            out[n] = 0;
        }
    }
    CloseHandle(h);
}

/* Steam records the game it is running in HKCU\Software\Valve\Steam\RunningAppID (0 = none).
 * Shutting Steam down closes that game, so Steam is never restarted while it is non-zero. */
static bool steam_game_running(void) {
    DWORD id = 0, size = sizeof(id);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"RunningAppID", RRF_RT_REG_DWORD, NULL, &id,
                     &size) != ERROR_SUCCESS) {
        return false;
    }
    if (id != 0) {
        plog("Steam is running a game (app %lu): Steam is not restarted", id);
    }
    return id != 0;
}

/* Close one program and bring it back. @return true when it is running again (or, for Game Bar,
 * gone and left for Windows to restart on demand). */
static bool close_and_reopen(int how, DWORD pid, const WCHAR *image, const WCHAR *cmdline) {
    if (how == 1) {
        if (steam_game_running()) {
            return false;
        }
        WCHAR c[MAX_PATH + 16];
        _snwprintf(c, MAX_PATH + 16, L"\"%ls\" -shutdown", image);
        run(c);
        if (!wait_exit(pid, 30000)) {
            plog("Steam did not exit within 30 s: left running");
            return false;
        }
        Sleep(1000);
        _snwprintf(c, MAX_PATH + 16, L"\"%ls\" -silent", image);
        return run(c);
    }
    if (how == 2) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        const bool ok = h != NULL && TerminateProcess(h, 0);
        if (h != NULL) CloseHandle(h);
        return ok;
    }
    EnumWindows(close_windows_of, (LPARAM) pid);
    if (!wait_exit(pid, 10000)) {
        plog("pid %lu did not close within 10 s: left running", pid);
        return false;
    }
    Sleep(500);
    return run(cmdline);
}

int pad_own_evict(const pad_own_scan_t *scan, char *report, size_t report_len) {
    int done = 0;
    if (report != NULL && report_len > 0) {
        report[0] = 0;
    }
    bool steam_done = false;
    for (int i = 0; i < scan->count; i++) {
        const pad_own_holder_t *hd = &scan->holders[i];
        const int ki = known_index(hd->exe);
        if (!hd->closable || hd->admin || ki < 0) {
            continue;                    /* services and admin programs: the admin step (pad_own_evict_admin) */
        }
        const int how = known_apps[ki].how;
        if (how == 1 && steam_done) {
            continue;                    /* Steam runs several steam.exe; one shutdown covers them */
        }
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, hd->pid);
        WCHAR image[MAX_PATH] = {0};
        DWORD n = MAX_PATH;
        if (h == NULL || !QueryFullProcessImageNameW(h, 0, image, &n)) {
            if (h != NULL) CloseHandle(h);
            continue;
        }
        CloseHandle(h);
        WCHAR cmd[2048];
        command_line_of(hd->pid, image, cmd, 2048);
        const bool ok = close_and_reopen(how, hd->pid, image, cmd);
        plog("close and reopen %s (pid %lu): %s", hd->exe, hd->pid, ok ? "ok" : "FAILED");
        if (how == 1) {
            steam_done = true;
        }
        if (ok) {
            done++;
            if (how != 2 && g_reopened_count < PAD_OWN_MAX_HOLDERS) {
                reopened_t *r = &g_reopened[g_reopened_count++];
                r->how = how;
                wcsncpy(r->image, image, MAX_PATH - 1);
                wcsncpy(r->cmdline, cmd, 2047);
            }
        }
        if (report != NULL) {
            const size_t used = strlen(report);
            snprintf(report + used, report_len - used, "%s%s %s", used ? ", " : "", hd->display,
                     ok ? "reopened" : "could not be closed");
        }
    }
    return done;
}

/* ------------------------------------------------------------------ the admin step: services */

/* Stop and start one service, waiting for each state. A stop that would also stop OTHER services (dependents
 * running) is refused by Windows, and is then left alone. */
static bool service_wait(SC_HANDLE svc, DWORD state, DWORD ms) {
    SERVICE_STATUS st;
    for (DWORD waited = 0; waited <= ms; waited += 200) {
        if (!QueryServiceStatus(svc, &st)) {
            return false;
        }
        if (st.dwCurrentState == state) {
            return true;
        }
        Sleep(200);
    }
    return false;
}

static bool service_restart(const char *name, char *why, size_t why_len) {
    WCHAR w[128];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 128);
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm != NULL ? OpenServiceW(scm, w, SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS) : NULL;
    bool ok = false;
    if (svc == NULL) {
        snprintf(why, why_len, "cannot open it (%lu)", GetLastError());
    } else {
        SERVICE_STATUS st;
        if (!ControlService(svc, SERVICE_CONTROL_STOP, &st) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
            snprintf(why, why_len, "it refused to stop (%lu)", GetLastError());
        } else if (!service_wait(svc, SERVICE_STOPPED, 20000)) {
            snprintf(why, why_len, "it did not stop within 20 s");
        } else if (!StartServiceW(svc, 0, NULL) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
            snprintf(why, why_len, "it stopped but did not start again (%lu)", GetLastError());
        } else {
            ok = service_wait(svc, SERVICE_RUNNING, 20000);
            if (!ok) {
                snprintf(why, why_len, "it did not report running within 20 s");
            }
        }
    }
    if (svc != NULL) CloseServiceHandle(svc);
    if (scm != NULL) CloseServiceHandle(scm);
    return ok;
}

/* The services restarted at the start, restarted once more at session end so this PC gets the pads back
 * (a program reopened blind does not pick a pad up again by itself -- measured for Steam). */
#define ADMIN_MAX_RESTARTED 8
static char g_admin_restarted[ADMIN_MAX_RESTARTED][128];
static int g_admin_restarted_count;

static void report_add(char *report, size_t len, const char *fmt, ...) {
    const size_t used = strlen(report);
    if (used + 3 >= len) {
        return;
    }
    if (used > 0) {
        strcat(report, "; ");
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(report + strlen(report), len - strlen(report), fmt, ap);
    va_end(ap);
}

/* Elevated: scan everything, restart the restartable service holders, say what else still holds them. */
static void admin_step(char *report, size_t len) {
    report[0] = 0;
    static pad_own_scan_t scan;
    pad_own_scan(&scan);
    routes_lock(report, len);
    g_admin_restarted_count = 0;
    int admin_holders = 0;
    for (int i = 0; i < scan.count; i++) {
        const pad_own_holder_t *hd = &scan.holders[i];
        if (!hd->admin) {
            continue;
        }
        admin_holders++;
        if (hd->service[0] == 0) {
            report_add(report, len, "%s still has them (cannot be restarted safely)", hd->display);
            continue;
        }
        bool done = false;
        for (int k = 0; k < g_admin_restarted_count && !done; k++) {
            done = strcmp(g_admin_restarted[k], hd->service) == 0;
        }
        if (done) {
            continue;
        }
        char why[128] = "";
        const bool ok = service_restart(hd->service, why, sizeof(why));
        plog("admin: restart service %s (\"%s\", pid %lu): %s%s%s", hd->service, hd->display, hd->pid,
             ok ? "ok" : "FAILED -- ", ok ? "" : why, "");
        report_add(report, len, ok ? "%s restarted" : "%s could not be restarted", hd->display);
        if (ok && g_admin_restarted_count < ADMIN_MAX_RESTARTED) {
            SDL_strlcpy(g_admin_restarted[g_admin_restarted_count++], hd->service, 128);
        }
    }
    if (admin_holders == 0 && g_lock_count == 0) {
        report_add(report, len, "no Windows service, driver or administrator program had them open");
    }
}

/* The report's machine part, after the text: one "LOCKED <device>" line per locked route, so the asking Eclipse
 * can find (and reopen) the programs that held those devices. */
static void report_locks(char *report, size_t len) {
    for (int i = 0; i < g_lock_count; i++) {
        const size_t used = strlen(report);
        char dev[200];
        WideCharToMultiByte(CP_UTF8, 0, g_locks[i].dev, -1, dev, sizeof(dev), NULL, NULL);
        snprintf(report + used, len - used, "\nLOCKED %s", dev);
    }
}

static void report_take_locks(char *report) {
    g_extra_count = 0;
    char *nl = strchr(report, '\n');
    if (nl == NULL) {
        return;
    }
    *nl = 0;
    for (char *line = nl + 1; line != NULL && *line != 0;) {
        char *next = strchr(line, '\n');
        if (next != NULL) {
            *next++ = 0;
        }
        if (strncmp(line, "LOCKED ", 7) == 0 && g_extra_count < ROUTE_MAX) {
            MultiByteToWideChar(CP_UTF8, 0, line + 7, -1, g_extra[g_extra_count++], 160);
        }
        line = next;
    }
}

static void admin_restart_again(const char *who) {
    for (int k = 0; k < g_admin_restarted_count; k++) {
        char why[128] = "";
        const bool ok = service_restart(g_admin_restarted[k], why, sizeof(why));
        plog("%s: session end: restart service %s again: %s%s", who, g_admin_restarted[k], ok ? "ok" : "FAILED -- ",
             ok ? "" : why);
    }
    g_admin_restarted_count = 0;
}

/* The elevated copy started by a non-elevated Eclipse: its process and the event that ends it. */
static HANDLE g_admin_proc;
static HANDLE g_admin_end;

#define ADMIN_ARG "--pad-own-admin="

static bool admin_paths(DWORD owner, WCHAR *report, size_t rlen, WCHAR *ready, WCHAR *end, size_t elen) {
    WCHAR state[MAX_PATH];
    if (!state_path(state, MAX_PATH)) {
        return false;
    }
    WCHAR *slash = wcsrchr(state, L'\\');
    if (slash == NULL) {
        return false;
    }
    *slash = 0;
    _snwprintf(report, rlen, L"%ls\\pad-ownership-admin-%lu.txt", state, owner);
    _snwprintf(ready, elen, L"Local\\EclipsePadOwnReady-%lu", owner);
    _snwprintf(end, elen, L"Local\\EclipsePadOwnEnd-%lu", owner);
    return true;
}

static void pad_event_name(DWORD owner, WCHAR *out, size_t len) {
    _snwprintf(out, len, L"Local\\EclipsePadOwnPads-%lu", owner);
}

/* ------------------------------------------------------------------ admin prompts in the pane (session) */

/* ADMIN PROMPTS ON THE NORMAL DESKTOP, for a local-pane session only (user 2026-11-12). MEASURED 2026-11-12 on
 * Vega (local_pane_probe, pane display primary, notepad -Verb RunAs): with PromptOnSecureDesktop=1 the monitor
 * went dark and the prompt was on the invisible pane display -- the RDR2 symptom; with 0 the prompt was a normal
 * window on the pane display and the pane's view showed it, where the user can click it. Windows' own setting
 * ("Switch to the secure desktop when prompting for elevation"); turned off for the session, the old value kept
 * in a file so an elevated copy that dies is undone by the next one. */
#define UAC_KEY L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System"
static bool g_uac_changed;
static DWORD g_uac_orig = 1;

static bool uac_path(WCHAR *out, size_t len) {
    WCHAR state[MAX_PATH];
    if (!state_path(state, MAX_PATH)) {
        return false;
    }
    WCHAR *slash = wcsrchr(state, L'\\');
    if (slash == NULL) {
        return false;
    }
    *slash = 0;
    return _snwprintf(out, len, L"%ls\\pad-ownership-uac.txt", state) > 0;
}

static void uac_session_off(char *report, size_t len) {
    WCHAR path[MAX_PATH];
    if (!uac_path(path, MAX_PATH)) {
        return;
    }
    DWORD cur = 1, sz = sizeof(cur);
    RegGetValueW(HKEY_LOCAL_MACHINE, UAC_KEY, L"PromptOnSecureDesktop", RRF_RT_REG_DWORD, NULL, &cur, &sz);
    DWORD orig = cur;
    FILE *f = _wfopen(path, L"r");
    if (f != NULL) {                     /* an earlier session never put it back: that file has the real value */
        unsigned v = 1;
        if (fscanf(f, "%u", &v) == 1) {
            orig = v;
        }
        fclose(f);
    }
    if (cur == 0 && orig == 0) {
        plog("admin: admin prompts already appear on the normal desktop (the user's own setting): left as it is");
        return;
    }
    f = _wfopen(path, L"w");
    if (f != NULL) {
        fprintf(f, "%lu\n", (unsigned long) orig);
        fclose(f);
    }
    DWORD zero = 0;
    const bool ok = RegSetKeyValueW(HKEY_LOCAL_MACHINE, UAC_KEY, L"PromptOnSecureDesktop", REG_DWORD, &zero,
                                    sizeof(zero)) == ERROR_SUCCESS;
    g_uac_changed = ok;
    g_uac_orig = orig;
    plog("admin: admin prompts on the normal desktop for this session (PromptOnSecureDesktop %lu -> 0): %s",
         (unsigned long) orig, ok ? "ok" : "FAILED");
    report_add(report, len, ok ? "admin prompts will appear in this PC's half" : "admin prompts could not be moved");
}

static void uac_restore(const char *who) {
    if (!g_uac_changed) {
        return;
    }
    const bool ok = RegSetKeyValueW(HKEY_LOCAL_MACHINE, UAC_KEY, L"PromptOnSecureDesktop", REG_DWORD, &g_uac_orig,
                                    sizeof(g_uac_orig)) == ERROR_SUCCESS;
    plog("%s: admin prompts back to the secure desktop setting (PromptOnSecureDesktop -> %lu): %s", who,
         (unsigned long) g_uac_orig, ok ? "ok" : "FAILED");
    if (ok) {
        WCHAR path[MAX_PATH];
        if (uac_path(path, MAX_PATH)) {
            DeleteFileW(path);
        }
        g_uac_changed = false;
    }
}

/* ------------------------------------------------------------------ the elevated copy, from this side */

static HANDLE g_admin_ready;             /* the elevated copy finished a step (manual reset) */
static HANDLE g_admin_pads;              /* ask it for the controller step (auto reset) */
static bool g_session_admin;             /* a local-pane session's admin, in force (here or in the copy) */

bool pad_own_admin_ready(void) {
    return pad_own_elevated() || g_admin_proc != NULL;
}

static void read_report(const WCHAR *rpath, char *report, size_t report_len) {
    report[0] = 0;
    FILE *f = _wfopen(rpath, L"rb");
    if (f != NULL) {
        const size_t n = fread(report, 1, report_len - 1, f);
        report[n] = 0;
        fclose(f);
        DeleteFileW(rpath);
    }
}

/* Wait for the copy's "ready" after a step: false when it died or did not answer. */
static bool wait_ready(char *report, size_t report_len, DWORD ms) {
    HANDLE waits[2] = {g_admin_ready, g_admin_proc};
    const DWORD w = WaitForMultipleObjects(2, waits, FALSE, ms);
    WCHAR rpath[MAX_PATH], rn[64], en[64];
    admin_paths(GetCurrentProcessId(), rpath, MAX_PATH, rn, en, 64);
    if (w != WAIT_OBJECT_0) {
        snprintf(report, report_len, "the elevated copy %s", w == WAIT_OBJECT_0 + 1 ? "exited without a report"
                                                                                   : "did not answer within 90 s");
        return false;
    }
    read_report(rpath, report, report_len);
    return true;
}

/* Start the elevated copy (Windows' prompt). @p session: a local-pane session's copy -- it moves admin prompts
 * to the normal desktop and waits to be asked for the controller step; otherwise it does that step at once. */
static pad_own_admin_result_t admin_launch(bool session, char *report, size_t report_len) {
    report[0] = 0;
    const DWORD self = GetCurrentProcessId();
    WCHAR rpath[MAX_PATH], ready_name[64], end_name[64], pads_name[64], exe[MAX_PATH];
    if (!admin_paths(self, rpath, MAX_PATH, ready_name, end_name, 64) || GetModuleFileNameW(NULL, exe, MAX_PATH) == 0) {
        SDL_strlcpy(report, "no place for its report", report_len);
        return PAD_OWN_ADMIN_FAILED;
    }
    pad_event_name(self, pads_name, 64);
    DeleteFileW(rpath);
    if (g_admin_ready == NULL) g_admin_ready = CreateEventW(NULL, TRUE, FALSE, ready_name);
    if (g_admin_pads == NULL) g_admin_pads = CreateEventW(NULL, FALSE, FALSE, pads_name);
    g_admin_end = CreateEventW(NULL, TRUE, FALSE, end_name);
    ResetEvent(g_admin_ready);
    ResetEvent(g_admin_end);
    WCHAR params[96];
    _snwprintf(params, 96, L"%hs%lu%ls", ADMIN_ARG, self, session ? L" --session" : L"");
    SHELLEXECUTEINFOW sei;
    SDL_zero(sei);
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = params;
    sei.nShow = SW_HIDE;
    plog("admin: asking Windows for administrator rights (%s)", session ? "this local-pane session" : "controllers");
    if (!ShellExecuteExW(&sei) || sei.hProcess == NULL) {
        const DWORD err = GetLastError();
        if (g_admin_end != NULL) CloseHandle(g_admin_end);
        g_admin_end = NULL;
        plog("admin: %s (%lu)", err == ERROR_CANCELLED ? "the user declined the administrator prompt" : "could not start",
             err);
        if (err == ERROR_CANCELLED) {
            return PAD_OWN_ADMIN_DECLINED;
        }
        snprintf(report, report_len, "the elevated copy could not start (%lu)", err);
        return PAD_OWN_ADMIN_FAILED;
    }
    g_admin_proc = sei.hProcess;
    if (!wait_ready(report, report_len, 90000)) {
        plog("admin: %s", report);
        return PAD_OWN_ADMIN_FAILED;
    }
    return PAD_OWN_ADMIN_DONE;
}

pad_own_admin_result_t pad_own_session_admin_begin(char *report, size_t report_len) {
    report[0] = 0;
    if (g_session_admin) {
        SDL_strlcpy(report, "already in force", report_len);
        return PAD_OWN_ADMIN_DONE;
    }
    if (pad_own_elevated()) {
        uac_session_off(report, report_len);
        g_session_admin = true;
        return PAD_OWN_ADMIN_DONE;
    }
    const pad_own_admin_result_t r = admin_launch(true, report, report_len);
    g_session_admin = r == PAD_OWN_ADMIN_DONE;
    plog("admin: session copy: %s -- %s", r == PAD_OWN_ADMIN_DONE ? "running" : "NOT running", report);
    return r;
}

pad_own_admin_result_t pad_own_evict_admin(char *report, size_t report_len) {
    report[0] = 0;
    if (pad_own_elevated()) {
        admin_step(report, report_len);   /* g_extra is set by routes_lock */
        return PAD_OWN_ADMIN_DONE;
    }
    if (g_admin_proc != NULL) {
        /* The session's copy is running: ask it, no second prompt. */
        ResetEvent(g_admin_ready);
        SetEvent(g_admin_pads);
        if (!wait_ready(report, report_len, 90000)) {
            plog("admin: %s", report);
            return PAD_OWN_ADMIN_FAILED;
        }
    } else {
        const pad_own_admin_result_t r = admin_launch(false, report, report_len);
        if (r != PAD_OWN_ADMIN_DONE) {
            return r;
        }
    }
    report_take_locks(report);
    plog("admin: elevated copy reports: %s (%d driver route(s) locked)", report, g_extra_count);
    return PAD_OWN_ADMIN_DONE;
}

bool pad_own_admin_needed(const pad_own_scan_t *scan) {
    if (scan->routes > 0) {
        return true;
    }
    if (!scan->elevated) {
        return scan->unscanned > 0;
    }
    for (int i = 0; i < scan->count; i++) {
        if (scan->holders[i].admin && scan->holders[i].closable) {
            return true;
        }
    }
    return false;
}

/* The elevated copy, after the session end was signalled: the refresh (reopening Steam) waits for it, so Steam
 * comes back AFTER its driver route is open again. */
static HANDLE g_admin_wait;

/* Session end: the pads are visible again -- open the driver routes and restart the same services once more, and
 * put admin prompts back where the user had them. */
static void admin_session_end(void) {
    if (g_admin_end != NULL) {
        SetEvent(g_admin_end);           /* the elevated copy does it, then exits */
        CloseHandle(g_admin_end);
        g_admin_end = NULL;
    }
    if (g_admin_proc != NULL) {
        if (g_admin_wait != NULL) {
            CloseHandle(g_admin_wait);
        }
        g_admin_wait = g_admin_proc;
        g_admin_proc = NULL;
    }
    if (g_lock_count > 0) {
        routes_unlock("admin");          /* this Eclipse is elevated and did it itself */
    }
    if (g_admin_restarted_count > 0) {
        admin_restart_again("admin");
    }
    uac_restore("admin");
    g_extra_count = 0;
    g_session_admin = false;
}

/* THE ELEVATED COPY: moonlight-tv.exe --pad-own-admin=<pid of the Eclipse that asked> [--session]. With --session
 * (a local-pane session) it moves admin prompts to the normal desktop at once and waits to be asked for the
 * controller step; without, it does the controller step at once. Either way it then waits for the session end
 * (or the asking Eclipse's exit) and undoes everything: driver routes, admin prompts, services restarted again.
 * Starts nothing else: no window, no SDL, no stream. */
static void copy_report(const WCHAR *rpath, const char *report, HANDLE ready) {
    FILE *f = _wfopen(rpath, L"wb");
    if (f != NULL) {
        fwrite(report, 1, strlen(report), f);
        fclose(f);
    }
    if (ready != NULL) {
        SetEvent(ready);
    }
}

static void copy_pad_step(char *report, size_t len) {
    DWORD state_pid = 0;
    report[0] = 0;
    if (!state_load(&g_state, &state_pid) || !g_state.active) {
        SDL_strlcpy(report, "nothing is hidden", len);
    } else {
        admin_step(report, len);
        report_locks(report, len);
    }
    SDL_zero(g_state);                   /* the asking Eclipse owns the hide; this copy never reverts it */
}

bool pad_own_admin_main(int argc, char *argv[], int *exit_code) {
    DWORD owner = 0;
    bool session = false;
    for (int i = 1; i < argc; i++) {
        if (argv[i] != NULL && strncmp(argv[i], ADMIN_ARG, strlen(ADMIN_ARG)) == 0) {
            owner = (DWORD) strtoul(argv[i] + strlen(ADMIN_ARG), NULL, 10);
        } else if (argv[i] != NULL && strcmp(argv[i], "--session") == 0) {
            session = true;
        }
    }
    if (owner == 0) {
        return false;
    }
    *exit_code = 1;
    WCHAR rpath[MAX_PATH], ready_name[64], end_name[64], pads_name[64];
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, owner);
    if (parent == NULL || !admin_paths(owner, rpath, MAX_PATH, ready_name, end_name, 64) || !pad_own_elevated()) {
        plog("admin copy: cannot serve Eclipse pid %lu", owner);
        return true;
    }
    pad_event_name(owner, pads_name, 64);
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name);
    HANDLE end = OpenEventW(SYNCHRONIZE, FALSE, end_name);
    HANDLE pads = OpenEventW(SYNCHRONIZE, FALSE, pads_name);
    static char report[1024];
    report[0] = 0;
    if (session) {
        uac_session_off(report, sizeof(report));
    } else {
        copy_pad_step(report, sizeof(report));
    }
    copy_report(rpath, report, ready);
    plog("admin copy for pid %lu%s: %s", owner, session ? " (session)" : "", report);
    bool ended = false;
    while (!ended && end != NULL) {
        HANDLE waits[3] = {end, parent, pads};
        const DWORD w = WaitForMultipleObjects(pads != NULL ? 3 : 2, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0 + 2) {
            copy_pad_step(report, sizeof(report));
            copy_report(rpath, report, ready);
            plog("admin copy for pid %lu: controller step: %s", owner, report);
            continue;
        }
        /* The routes and admin prompts are put back either way: an Eclipse that died must not leave Steam's
         * driver locked or admin prompts off the secure desktop. */
        routes_unlock("admin copy");
        uac_restore("admin copy");
        if (w == WAIT_OBJECT_0) {
            Sleep(500);
            admin_restart_again("admin copy");
        } else {
            plog("admin copy: Eclipse pid %lu ended without a session end; restarted services are left as they are",
                 owner);
        }
        ended = true;
    }
    if (ready != NULL) CloseHandle(ready);
    if (pads != NULL) CloseHandle(pads);
    if (end != NULL) CloseHandle(end);
    CloseHandle(parent);
    *exit_code = 0;
    return true;
}

/* ------------------------------------------------------------------ session end and recovery */

static int SDLCALL refresh_thread(void *arg) {
    (void) arg;
    if (g_admin_wait != NULL) {
        /* The elevated copy opens the driver routes and restarts services first (it exits when done). */
        WaitForSingleObject(g_admin_wait, 20000);
        CloseHandle(g_admin_wait);
        g_admin_wait = NULL;
    }
    /* The programs reopened blind at the start do not see the pads come back by themselves
     * (measured): close and reopen them once more now that the pads are visible again. */
    for (int i = 0; i < g_reopened_count; i++) {
        reopened_t *r = &g_reopened[i];
        DWORD pid = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(pe);
        const WCHAR *base = wcsrchr(r->image, L'\\');
        base = base != NULL ? base + 1 : r->image;
        for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
            if (_wcsicmp(pe.szExeFile, base) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        }
        CloseHandle(snap);
        if (pid != 0) {
            const bool ok = close_and_reopen(r->how, pid, r->image, r->cmdline);
            plog("session end: refreshed %ls: %s", base, ok ? "ok" : "FAILED");
        }
    }
    g_reopened_count = 0;
    return 0;
}

void pad_own_session_end(void) {
    SDL_LockMutex(lock_get());
    const bool active = g_state.active;
    if (active) {
        revert(&g_state);
        SDL_zero(g_state);
        state_save();
    }
    SDL_UnlockMutex(lock_get());
    /* Also with nothing hidden: a local-pane session's admin (admin prompts in the pane) ends with the session. */
    admin_session_end();
    if (active && g_reopened_count > 0) {
        SDL_Thread *t = SDL_CreateThread(refresh_thread, "PadOwnRefresh", NULL);
        if (t != NULL) {
            SDL_DetachThread(t);
        }
    }
}

void pad_own_startup_recover(void) {
    own_state_t st;
    DWORD pid = 0;
    if (!state_load(&st, &pid) || !st.active) {
        return;
    }
    if (pid != 0 && pid != GetCurrentProcessId()) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h != NULL) {
            WCHAR image[MAX_PATH];
            DWORD n = MAX_PATH;
            const bool ours = QueryFullProcessImageNameW(h, 0, image, &n) && wcsstr(image, L"moonlight-tv.exe") != NULL;
            DWORD code = 0;
            const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
            CloseHandle(h);
            if (ours && alive) {
                return;            /* another Eclipse (e.g. the co-op secondary) still owns it */
            }
        }
    }
    plog("startup: a previous run left %d controller(s) hidden: restoring", st.added_count);
    if (revert(&st)) {
        WCHAR path[MAX_PATH];
        if (state_path(path, MAX_PATH)) {
            DeleteFileW(path);
        }
    }
}

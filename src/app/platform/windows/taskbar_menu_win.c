/*
 * taskbar_menu_win.c -- the taskbar icon's right-click menu (jump list). See platform/taskbar_menu.h.
 */
#define COBJMACROS
#include "platform/taskbar_menu.h"

#include <windows.h>
#include <shobjidl.h>
#include <propsys.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <SDL.h>

#include "logging.h"
#include "util/bus.h"
#include "util/i18n.h"
#include "util/user_event.h"

#define CMD_WINDOW_CLASS L"EclipseTaskbarCommand"
#define CMD_COPYDATA_TAG 0xEC11u     /* COPYDATASTRUCT.dwData: "this is a taskbar command" */
#define FORWARD_TIMEOUT_MS 5000

/* PKEY_Title -- a jump-list task's visible text (propkey.h: {F29F85E0-4FF9-1068-AB91-08002B27B3D9}, 2).
 * Defined here rather than pulled from propkey.h + initguid, which would define every key. */
static const PROPERTYKEY KEY_TITLE = {
        {0xF29F85E0, 0x4FF9, 0x1068, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}}, 2};

static const struct {
    taskbar_cmd_t cmd;
    const char *arg;          /* after --taskbar= */
} commands[] = {
        {TASKBAR_CMD_DISCONNECT, "disconnect"},
        {TASKBAR_CMD_QUIT, "quit"},
        {TASKBAR_CMD_GIVE_BACK_MOUSE, "mouse"},
        {TASKBAR_CMD_RESUME_COOP, "resume-coop"},
};

static HWND cmd_window;
static taskbar_cmd_t pending_cmd;   /* carried by a start that found no running Eclipse */
static bool coop_pair;
static CRITICAL_SECTION list_lock;
static bool list_lock_ready;

/* ------------------------------------------------------------------ the carrier process */

static taskbar_cmd_t parse_command(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (argv[i] == NULL || strncmp(argv[i], "--taskbar=", 10) != 0) {
            continue;
        }
        for (size_t k = 0; k < sizeof(commands) / sizeof(commands[0]); k++) {
            if (strcmp(argv[i] + 10, commands[k].arg) == 0) {
                return commands[k].cmd;
            }
        }
    }
    return TASKBAR_CMD_NONE;
}

bool taskbar_menu_forward(int argc, char *argv[], int *exit_code) {
    const taskbar_cmd_t cmd = parse_command(argc, argv);
    if (cmd == TASKBAR_CMD_NONE) {
        return false;
    }
    *exit_code = 0;
    HWND target = FindWindowExW(HWND_MESSAGE, NULL, CMD_WINDOW_CLASS, NULL);
    if (target == NULL) {
        /* No Eclipse running. Resume starts one and resumes from there; the stream entries have
         * nothing to act on (a menu left over from an Eclipse that was killed mid-stream). */
        if (cmd == TASKBAR_CMD_RESUME_COOP) {
            pending_cmd = cmd;
            return false;
        }
        return true;
    }
    /* The running Eclipse puts its own window in front; allow it to. */
    DWORD target_pid = 0;
    GetWindowThreadProcessId(target, &target_pid);
    AllowSetForegroundWindow(target_pid);
    int payload = (int) cmd;
    COPYDATASTRUCT cds = {.dwData = CMD_COPYDATA_TAG, .cbData = sizeof(payload), .lpData = &payload};
    DWORD_PTR answer = 0;
    const LRESULT sent = SendMessageTimeoutW(target, WM_COPYDATA, 0, (LPARAM) &cds,
                                             SMTO_ABORTIFHUNG | SMTO_BLOCK, FORWARD_TIMEOUT_MS, &answer);
    if (sent != 0) {
        return true;
    }
    /* NOT ANSWERING: the stuck stream this menu is a backup for. Quit/Disconnect end that process;
     * in co-op its secondary then ends both panes (coop_secondary.c). Nothing else is forced. */
    if (cmd == TASKBAR_CMD_QUIT || cmd == TASKBAR_CMD_DISCONNECT) {
        HANDLE h = target_pid != 0 ? OpenProcess(PROCESS_TERMINATE, FALSE, target_pid) : NULL;
        if (h != NULL) {
            TerminateProcess(h, 1);
            CloseHandle(h);
        }
    } else if (cmd == TASKBAR_CMD_GIVE_BACK_MOUSE) {
        ClipCursor(NULL);         /* at least free a cursor the frozen stream had fenced in */
    }
    return true;
}

/* ------------------------------------------------------------------ the running Eclipse */

static LRESULT CALLBACK cmd_window_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COPYDATA) {
        const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *) lp;
        if (cds != NULL && cds->dwData == CMD_COPYDATA_TAG && cds->cbData == sizeof(int)) {
            const int cmd = *(const int *) cds->lpData;
            commons_log_info("Taskbar", "command %d from the taskbar menu", cmd);
            bus_pushevent(USER_TASKBAR_COMMAND, (void *) (intptr_t) cmd, NULL);
            return TRUE;
        }
        return FALSE;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

typedef struct {
    bool streaming;
    bool coop_pair;
    wchar_t exe[MAX_PATH];
    wchar_t titles[4][96];
} list_job_t;

static void to_wide(const char *utf8, wchar_t *out, int n) {
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, n) == 0) {
        out[0] = 0;
    }
}

static IShellLinkW *make_task(const wchar_t *exe, const char *arg, const wchar_t *title) {
    IShellLinkW *link = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **) &link))) {
        return NULL;
    }
    wchar_t args[64];
    _snwprintf(args, 64, L"--taskbar=%hs", arg);
    IShellLinkW_SetPath(link, exe);
    IShellLinkW_SetArguments(link, args);
    IShellLinkW_SetIconLocation(link, exe, 0);
    IShellLinkW_SetDescription(link, title);
    IPropertyStore *props = NULL;
    if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPropertyStore, (void **) &props))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        const size_t bytes = (wcslen(title) + 1) * sizeof(wchar_t);
        pv.vt = VT_LPWSTR;
        pv.pwszVal = CoTaskMemAlloc(bytes);
        if (pv.pwszVal != NULL) {
            memcpy(pv.pwszVal, title, bytes);
            IPropertyStore_SetValue(props, &KEY_TITLE, &pv);
            IPropertyStore_Commit(props);
        }
        PropVariantClear(&pv);
        IPropertyStore_Release(props);
    }
    return link;
}

/* COM on a thread of its own: the UI thread's apartment is SDL's business, and the shell calls can
 * take tens of milliseconds. One rebuild at a time; the last one committed is what the menu shows. */
static DWORD WINAPI list_thread(LPVOID arg) {
    list_job_t *job = arg;
    EnterCriticalSection(&list_lock);
    const HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    ICustomDestinationList *list = NULL;
    IObjectCollection *tasks = NULL;
    IObjectArray *removed = NULL;
    UINT slots = 0;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(&CLSID_DestinationList, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_ICustomDestinationList, (void **) &list)) &&
        SUCCEEDED(ICustomDestinationList_BeginList(list, &slots, &IID_IObjectArray, (void **) &removed)) &&
        SUCCEEDED(CoCreateInstance(&CLSID_EnumerableObjectCollection, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IObjectCollection, (void **) &tasks))) {
        int added = 0;
        for (size_t k = 0; k < sizeof(commands) / sizeof(commands[0]); k++) {
            const bool stream_entry = commands[k].cmd != TASKBAR_CMD_RESUME_COOP;
            if (stream_entry != job->streaming || (!stream_entry && !job->coop_pair)) {
                continue;
            }
            IShellLinkW *link = make_task(job->exe, commands[k].arg, job->titles[k]);
            if (link != NULL) {
                IObjectCollection_AddObject(tasks, (IUnknown *) link);
                IShellLinkW_Release(link);
                added++;
            }
        }
        IObjectArray *array = NULL;
        if (SUCCEEDED(IObjectCollection_QueryInterface(tasks, &IID_IObjectArray, (void **) &array))) {
            ok = (added == 0 || SUCCEEDED(ICustomDestinationList_AddUserTasks(list, array))) &&
                 SUCCEEDED(ICustomDestinationList_CommitList(list));
            IObjectArray_Release(array);
        }
    }
    if (tasks != NULL) IObjectCollection_Release(tasks);
    if (removed != NULL) IObjectArray_Release(removed);
    if (list != NULL) ICustomDestinationList_Release(list);
    if (SUCCEEDED(init)) CoUninitialize();
    LeaveCriticalSection(&list_lock);
    commons_log_info("Taskbar", "menu %s: %s", job->streaming ? "stream entries" : "launcher entry",
                     ok ? "ok" : "FAILED");
    SDL_free(job);
    return 0;
}

static void rebuild(bool streaming, bool wait) {
    list_job_t *job = SDL_calloc(1, sizeof(*job));
    if (job == NULL || !list_lock_ready) {
        SDL_free(job);
        return;
    }
    job->streaming = streaming;
    job->coop_pair = coop_pair;
    GetModuleFileNameW(NULL, job->exe, MAX_PATH);
    /* Localised on the UI thread: locstr is the UI's. Same order as commands[]. */
    to_wide(locstr("Disconnect"), job->titles[0], 96);
    to_wide(locstr("Quit stream"), job->titles[1], 96);
    to_wide(locstr("Give back the mouse"), job->titles[2], 96);
    to_wide(locstr("Resume last co-op session"), job->titles[3], 96);
    HANDLE t = CreateThread(NULL, 0, list_thread, job, 0, NULL);
    if (t == NULL) {
        SDL_free(job);
        return;
    }
    if (wait) {
        WaitForSingleObject(t, 3000);     /* at exit: the process must not end mid-commit */
    }
    CloseHandle(t);
}

void taskbar_menu_init(bool has_coop_pair) {
    coop_pair = has_coop_pair;
    if (!list_lock_ready) {
        InitializeCriticalSection(&list_lock);
        list_lock_ready = true;
    }
    WNDCLASSEXW wc = {.cbSize = sizeof(wc), .lpfnWndProc = cmd_window_proc,
                      .hInstance = GetModuleHandleW(NULL), .lpszClassName = CMD_WINDOW_CLASS};
    RegisterClassExW(&wc);
    /* Message-only, on the UI thread: SDL's own message pump delivers WM_COPYDATA to it. */
    cmd_window = CreateWindowExW(0, CMD_WINDOW_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                                 GetModuleHandleW(NULL), NULL);
    if (cmd_window == NULL) {
        commons_log_warn("Taskbar", "command window not created (%lu): the menu entries will do nothing",
                         GetLastError());
    }
    rebuild(false, false);
    if (pending_cmd != TASKBAR_CMD_NONE) {
        bus_pushevent(USER_TASKBAR_COMMAND, (void *) (intptr_t) pending_cmd, NULL);
        pending_cmd = TASKBAR_CMD_NONE;
    }
}

void taskbar_menu_set_streaming(bool streaming) {
    rebuild(streaming, false);
}

void taskbar_menu_deinit(void) {
    rebuild(false, true);
    if (cmd_window != NULL) {
        DestroyWindow(cmd_window);
        cmd_window = NULL;
    }
}

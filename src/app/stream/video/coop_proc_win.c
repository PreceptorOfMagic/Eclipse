/**
 * Windows bodies for the only non-portable part of coop_coord.c: spawning, reaping and
 * sweeping the secondary process. The POSIX bodies stay in coop_coord.c behind
 * `#ifndef _WIN32`; this file is compiled only on Windows. Keep the two in step -- the
 * contracts below are copied from the comments on the POSIX side, and where this platform
 * cannot honour one, that is stated rather than papered over.
 */
#include "coop_coord.h"

#ifdef _WIN32

#include "coop_win_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef AURORA_DIAG_LOG
#define AURORA_DIAG_LOG "aurora-diag.log"
#endif

static HANDLE coord_child_handle = NULL;
static DWORD  coord_child_pid    = 0;

static wchar_t *coop_utf8_to_wide(const char *src) {
    if (src == NULL) {
        return NULL;
    }
    int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, NULL, 0);
    if (chars <= 0) {
        return NULL;
    }
    wchar_t *wide = calloc((size_t) chars, sizeof(*wide));
    if (wide == NULL || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             src, -1, wide, chars) != chars) {
        free(wide);
        return NULL;
    }
    return wide;
}

/**
 * THE JOB OBJECT IS THIS PLATFORM'S PR_SET_PDEATHSIG.
 *
 * On Linux the child sets PR_SET_PDEATHSIG so an abnormal primary exit -- where normal
 * session cleanup never runs -- still takes the secondary with it. Windows has no such
 * per-process flag, but a job object with KILL_ON_JOB_CLOSE is stronger: the kernel
 * terminates every process in the job when the last handle to it closes, and the primary's
 * handle closes however the primary dies, including a crash. The handle is deliberately
 * never closed while we run.
 *
 * This is also why coop_coord_reap_orphan_secondaries() has nothing to do here.
 */
static HANDLE coord_job(void) {
    static HANDLE job = NULL;
    if (job != NULL) {
        return job;
    }
    job = CreateJobObjectW(NULL, NULL);
    if (job == NULL) {
        return NULL;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    memset(&li, 0, sizeof(li));
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li))) {
        CloseHandle(job);
        job = NULL;
    }
    return job;
}

/**
 * Nothing to sweep: the job object above makes an orphaned secondary impossible, so there
 * is no equivalent of the /proc walk the POSIX side does. Returning 0 is the truthful
 * answer ("none were killed"), not a stub standing in for unwritten code.
 *
 * If that ever stops being true -- someone removes the job object, or a secondary is
 * started outside it -- this must grow a real implementation. Note that a Windows process
 * does NOT expose its command line to another process without reading the remote PEB, so
 * the POSIX version's "same exe AND same socket path" test cannot be reproduced cheaply;
 * the job object is what avoids needing it.
 */
int coop_coord_reap_orphan_secondaries(const char *socket_path) {
    (void) socket_path;
    return 0;
}

int coop_coord_spawn_secondary_for(const char *exe_path, const char *socket_path,
                                   const char *host_uuid, int app_id,
                                   int stream_width, int stream_height) {
    /* Same contract as POSIX: the host and app travel in the environment so argv stays
     * the single --coop-secondary flag. CreateProcess gives the child a copy of ours. */
    unsetenv("COOP_HOST_UUID");
    unsetenv("COOP_APP_ID");
    unsetenv("COOP_STREAM_WIDTH");
    unsetenv("COOP_STREAM_HEIGHT");
    if (stream_width <= 0 || stream_height <= 0) {
        return -1;
    }
    if (host_uuid != NULL && setenv("COOP_HOST_UUID", host_uuid, 1) != 0) {
        return -1;
    }
    if (app_id > 0) {
        char idbuf[32];
        snprintf(idbuf, sizeof(idbuf), "%d", app_id);
        if (setenv("COOP_APP_ID", idbuf, 1) != 0) {
            unsetenv("COOP_HOST_UUID");
            return -1;
        }
    }
    char width_buf[32];
    char height_buf[32];
    snprintf(width_buf, sizeof(width_buf), "%d", stream_width);
    snprintf(height_buf, sizeof(height_buf), "%d", stream_height);
    if (setenv("COOP_STREAM_WIDTH", width_buf, 1) != 0 ||
        setenv("COOP_STREAM_HEIGHT", height_buf, 1) != 0) {
        unsetenv("COOP_HOST_UUID");
        unsetenv("COOP_APP_ID");
        unsetenv("COOP_STREAM_WIDTH");
        unsetenv("COOP_STREAM_HEIGHT");
        return -1;
    }
    const int pid = coop_coord_spawn_secondary(exe_path, socket_path);
    unsetenv("COOP_HOST_UUID");
    unsetenv("COOP_APP_ID");
    unsetenv("COOP_STREAM_WIDTH");
    unsetenv("COOP_STREAM_HEIGHT");
    return pid;
}

int coop_coord_spawn_secondary(const char *exe_path, const char *socket_path) {
    if (exe_path == NULL || socket_path == NULL) {
        return -1;
    }
    /* Wide APIs are required here, not polish. A portable folder under a user profile may
     * contain characters outside the active ANSI code page; CreateProcessA would then fail
     * even though the same executable was already running from that path. */
    wchar_t *wide_exe = coop_utf8_to_wide(exe_path);
    wchar_t *wide_socket = coop_utf8_to_wide(socket_path);
    if (wide_exe == NULL || wide_socket == NULL) {
        free(wide_exe);
        free(wide_socket);
        return -1;
    }

    /* CreateProcess parses argv out of ONE mutable string. Build it in UTF-16 so the
     * allocating self-path API is not immediately narrowed again by a fixed 2 KiB buffer.
     * Windows paths cannot contain a quote, so ordinary command-line quoting is sufficient.
     * CreateProcessW itself caps the complete command line at 32,767 UTF-16 code units. */
    static const wchar_t middle[] = L"\" --coop-secondary \"";
    const size_t cmd_chars = 1u + wcslen(wide_exe) + (sizeof(middle) / sizeof(middle[0]) - 1u) +
                             wcslen(wide_socket) + 1u;
    if (cmd_chars + 1u > 32767u) {
        free(wide_exe);
        free(wide_socket);
        return -1;
    }
    wchar_t *wide_cmd = calloc(cmd_chars + 1u, sizeof(*wide_cmd));
    if (wide_cmd == NULL ||
        swprintf(wide_cmd, cmd_chars + 1u, L"\"%ls\" --coop-secondary \"%ls\"",
                 wide_exe, wide_socket) < 0) {
        free(wide_exe);
        free(wide_socket);
        free(wide_cmd);
        return -1;
    }
    free(wide_socket);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    /* CREATE_SUSPENDED so the child is in the job BEFORE it runs: a process that spawned
     * and exited between CreateProcess and AssignProcessToJobObject would escape the
     * kill-on-close guarantee entirely. */
    if (!CreateProcessW(wide_exe, wide_cmd, NULL, NULL, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        free(wide_exe);
        free(wide_cmd);
        return -1;
    }
    free(wide_exe);
    free(wide_cmd);
    HANDLE job = coord_job();
    if (job == NULL || !AssignProcessToJobObject(job, pi.hProcess)) {
        /* Never resume a helper that escaped the kill-on-parent guarantee. */
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return -1;
    }
    if (ResumeThread(pi.hThread) == (DWORD) -1) {
        /* A suspended helper that never ran would otherwise look like a valid
         * spawn and hold startup for the full readiness timeout. */
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return -1;
    }
    CloseHandle(pi.hThread);

    coord_child_handle = pi.hProcess;
    coord_child_pid = pi.dwProcessId;
    return (int) pi.dwProcessId;
}

/* Wait up to timeout_ms for the child. True if it exited. */
static bool coord_wait_child(unsigned timeout_ms) {
    if (coord_child_handle == NULL) {
        return true;
    }
    return WaitForSingleObject(coord_child_handle, timeout_ms) == WAIT_OBJECT_0;
}

static void coord_forget_child(void) {
    if (coord_child_handle != NULL) {
        CloseHandle(coord_child_handle);
        coord_child_handle = NULL;
    }
    coord_child_pid = 0;
}

/**
 * BOUNDED, like the POSIX side, and for the same reason: this is the first blocking call
 * in teardown, so anything that stalls here leaves the UI on "Disconnecting...".
 *
 * session_video posts a cooperative shutdown over the existing control channel before it
 * calls us. Give that path long enough to destroy the Moonlight session and send Apollo's
 * quit request; TerminateProcess remains the bounded fallback for a wedged child.
 */
void coop_coord_reap_secondary(void) {
    if (coord_child_handle == NULL) {
        return;
    }
    if (coord_wait_child(2500)) {
        coord_forget_child();
        return;
    }
    TerminateProcess(coord_child_handle, 1);
    if (!coord_wait_child(2000)) {
        /* Same policy as POSIX: never block teardown on a child that will not die. */
        FILE *f = fopen(AURORA_DIAG_LOG, "a");
        if (f != NULL) {
            fprintf(f, "[COOP-COORD] secondary pid=%lu survived TerminateProcess; abandoning it\n",
                    (unsigned long) coord_child_pid);
            fclose(f);
        }
    }
    coord_forget_child();
}

bool coop_coord_child_exited(uint32_t *status_out) {
    /* Non-blocking, like POSIX. Keep the DWORD intact: Windows exceptions such
     * as 0xC0000005 have their high bit set and cannot share an int -1 sentinel. */
    if (coord_child_handle == NULL) {
        return false;
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(coord_child_handle, &code)) {
        return false;
    }
    if (code == STILL_ACTIVE) {
        return false;
    }
    coord_forget_child();
    if (status_out != NULL) {
        *status_out = (uint32_t) code;
    }
    return true;
}

#endif /* _WIN32 */

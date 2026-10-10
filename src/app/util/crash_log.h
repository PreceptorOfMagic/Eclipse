#pragma once
/*
 * A CRASH INTO THE KEPT LOG (support logs, 2026-09-30). Without it a crash just ends the log mid-line: no signal, no
 * address, no stack. Installed by diag_logs_start() once stderr is the kept log file; writes straight to fd 2 /
 * stderr, then lets the crash continue (default action / Windows error reporting), so nothing else changes.
 *
 *   Linux desktop: signal + fault address (or, for a signal another process or abort() sent, who sent it) + PC,
 *                  glibc backtrace, and /proc/self/maps (ASLR: frames are resolved offline against the map, then
 *                  the build's own binary).
 *   Windows:       exception code + address + module+offset, access-violation detail, a StackWalk64 of the
 *                  faulting context as module+offset frames. The shipped moonlight-tv.exe keeps its symbol table,
 *                  so the exact exe (its sha256 is in the report) resolves them: scripts/support/crash_frames.py.
 *   webOS:         the same as Linux, plus lr/sp and raw stack words (32-bit ARM backtraces come out EMPTY -- the
 *                  test crash of 2026-09-30 -- so those words are what a frame walk works from). Only a
 *                  DIAGNOSTIC build (AURORA_VENDOR_DIAG) leaves it to platform/webos/vendor_diag.c instead -- that
 *                  file is not in normal builds, so a release TV had NO crash record at all (measured 2026-09-30).
 *
 * A FROZEN UI INTO THE KEPT LOG. The other report a user gives with nothing to show for it: "it froze". The UI
 * thread beats once per main-loop pass (crash_log_ui_alive); a watchdog thread notices 5 s without a beat and
 * writes a "UI THREAD STUCK" record with that thread's stack (Windows: suspend, copy the stack, resume, then walk
 * the copy; Linux/webOS: SIGRTMIN+5 snapshot, the way vendor_diag.c samples threads on the TV), and a line when
 * the thread runs again. A second record at 30 s shows whether it moved. The same shape as a crash record, so
 * crash_frames.py reads both.
 */

/** Install the handlers (once). No-op only in a webOS AURORA_VENDOR_DIAG build (its own handler runs). */
void crash_log_install(void);

/** Watch the CALLING thread -- the UI thread -- for stalls, from now on. Only while a kept log is active. No-op in a
 *  webOS AURORA_VENDOR_DIAG build (vendor_diag.c has its own hang watch). */
void crash_log_watch_ui(void);

/** The UI thread's heartbeat, once per main-loop pass: one atomic increment. */
void crash_log_ui_alive(void);

/** The app is shutting down: the main loop has stopped beating on purpose. Stalls are still recorded (an app that
 *  will not close is a report too), after 15 s instead of 5 s. */
void crash_log_ui_shutdown(void);

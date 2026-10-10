#pragma once
/*
 * LOGS A USER CAN HAND OVER (2026-09-29).
 *
 * KEEP: with the `diag_logging` setting on, everything the app logs (stderr, and stdout where webOS's
 * logger also prints) goes to <settings dir>/logs/eclipse-YYYYMMDD-HHMMSS.log, the newest DIAG_LOGS_KEEP
 * kept. Before this the TV's only log was in /tmp, wiped by every reboot, and reachable only with
 * developer-mode ssh; a desktop build without a console kept nothing it could show.
 *
 * SHARE: while the "Share logs" screen is open, a small read-only web page on this device serves those
 * logs, the co-op diagnostics, the settings (secrets redacted) and a system summary, at an address with a
 * random token -- so anyone can download them from a browser on another computer or phone on the same
 * network, including off a TV. Closing the screen stops it. Not on Windows: listening there raised the
 * firewall prompt over the fullscreen app (2026-09-29); desktops get the one-file bundle and its folder.
 */
#include <stdbool.h>
#include <stddef.h>

#define DIAG_LOGS_KEEP 5
#define DIAG_LOGS_SHARE_PORT 47900
#define DIAG_LOGS_BUNDLE_NAME "eclipse-logs.txt"

/**
 * DID THE LAST RUN END CLEANLY? (2026-09-30) A run leaves <conf_dir>/logs/running.txt from start to clean exit, so
 * finding it at the next start means the last run crashed, froze and was closed by force, failed to start, or the
 * device lost power. The app then keeps THIS run's log even with the setting off: an app that dies before its UI
 * is up leaves the user no way to turn the setting on, and a random user will not have turned it on beforehand.
 * Returns true when the last run did not end cleanly; @p previous gets that run's own line ("started <time>,
 * Eclipse <version>"). Call once at start, after the settings are read; the primary process only.
 */
bool diag_logs_run_begin(const char *conf_dir, char *previous, size_t previous_len);

/** This run is ending cleanly: remove the marker. The very last thing before the process exits. */
void diag_logs_run_end(void);

/** Start keeping logs under @p conf_dir/logs (call once, after the settings are read). */
void diag_logs_start(const char *conf_dir);

/** Is this process writing a kept log? */
bool diag_logs_active(void);

/** The folder the kept logs live in (under @p conf_dir), written to @p dest. */
void diag_logs_dir(const char *conf_dir, char *dest, size_t maxlen);

/**
 * Start sharing. @p conf_dir is the settings directory; @p summary is a few lines about the app and
 * system, shown at the top of the page and of the combined download. On success writes the page's
 * address (one URL per usable network address, newline-separated) to @p urls and returns true.
 */
bool diag_logs_share_start(const char *conf_dir, const char *summary, char *urls, size_t urls_len);

/**
 * Write the same combined text the share page offers ("everything as one file": @p summary, the kept logs,
 * the co-op diagnostics, the settings with secrets removed) to <settings dir>/logs/DIAG_LOGS_BUNDLE_NAME,
 * replacing any earlier one. The desktop handover: the user sends that one file. Its path goes to @p path.
 */
bool diag_logs_write_bundle(const char *conf_dir, const char *summary, char *path, size_t path_len);

/** Stop sharing (no-op when not sharing). */
void diag_logs_share_stop(void);

#pragma once
/*
 * ONE REPORT FOR BLIND SUPPORT (user, 2026-09-30: "assume that you will only ever get the one file from someone,
 * so it has to contain absolutely everything you would possibly need").
 *
 * Plain text, NO SECRETS (no PIN, no keys, no passwords): which build (commit + the running binary's SHA-256, since
 * the version number is not bumped per build), OS / firmware / kernel, CPU and memory, displays and panel, the video
 * and audio modules with their capabilities, controllers AND the input devices not used as controllers, every
 * network interface with its addresses and the default route, every known computer with what it last reported, and
 * the settings that shape a stream.
 *
 * Where it goes: into every kept log once the app is up (diag_report_log), and at the top of the Share-logs bundle
 * and page (diag_report_build) -- so each of the last 5 starts carries its own environment, and the file a user
 * sends always starts with the current one.
 */
#include <stdbool.h>

struct app_t;
struct pclist_t;

/** The whole report as one malloc'd string (caller frees). Main thread. */
char *diag_report_build(struct app_t *app);

/** The part of the report that needs nothing started -- which build, OS / firmware, CPU, memory, GPUs and their
 *  drivers, storage, network, settings -- as one malloc'd string (caller frees). For the very start of a kept log,
 *  so a run that dies while starting (in video, audio or driver set-up: before diag_report_log) still says what it
 *  ran on. Main thread. */
char *diag_report_build_start(struct app_t *app);

/** diag_report_build_start into the log, one "Report" line per line, and start hashing the binary. */
void diag_report_log_start(struct app_t *app);

/** The whole report into the log, one "Report" line per line of text, then the binary's hash when it is known
 *  (hashed on a worker thread). Main thread; only while a kept log is active. */
void diag_report_log(struct app_t *app);

/** One line describing a computer, into the log -- only when it differs from the last line logged for that
 *  computer (a status poll every few seconds must not flood the log). Main thread, pcmanager locked. */
void diag_report_log_host(const struct pclist_t *node);

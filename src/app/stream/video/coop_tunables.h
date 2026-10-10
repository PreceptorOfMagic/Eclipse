/*
 * CO-OP DIALS -> the /tmp sentinels the runtime already reads.
 *
 * WHY NOT READ THE SETTINGS DIRECTLY. Every one of these knobs exists because it was
 * measured by touching a file and re-running: `/tmp/coop-nocatchup`, `/tmp/coop-slices`,
 * `/tmp/coop-fps` and the rest are read in the video, audio and session paths, each at
 * the exact moment that value is latched. Teaching those paths to read app_configuration
 * instead would mean editing the streaming code in a dozen delicate places to change
 * nothing about what it does. Writing the files leaves all of it untouched.
 *
 * PRECEDENCE, AND IT MATTERS: these are applied at APP START and whenever the settings
 * screen changes one -- never at stream start. scripts/coop/run_coop.sh writes the whole
 * sentinel set immediately before a test run (docs/coop-modes.md: every run writes every
 * switch), so a test still overrides the UI, which is what keeps the measured procedure
 * working. The settings are the baseline the app comes up with.
 *
 * /tmp/coop-playfile IS DELIBERATELY NOT HERE and must never be. It replays a recording
 * while every counter looks healthy; it once stayed armed for two hours and silently
 * voided two findings (CLAUDE.md). It is not a setting, it is a trap.
 */
#pragma once

typedef struct CONFIGURATION app_settings_t_fwd;

/**
 * Materialise the co-op dials for the CURRENT orientation's column. Safe to call
 * repeatedly; it only ever writes or removes the files it owns.
 *
 * THE DIALS ONLY, NOT THE MODE. Everything written here is read inside the co-op
 * composite and means nothing to an ordinary stream, so it is safe to leave on disk.
 */
/** Write the dial sentinels for ONE orientation's column.
 *
 *  TAKES THE ORIENTATION EXPLICITLY because the right answer differs per caller and the
 *  two used to be conflated: the settings screen means "the column being edited", a
 *  session start means "the column this run will use", and app start in the SECONDARY
 *  means "whatever the primary published" (coop_orientation_resolved()). Deriving it
 *  inside here made a stale session's column win over a fresh dialog choice. */
void coop_tunables_apply(int orientation);

/**
 * ARM A MODE. Writes the dials AND the mode selectors for @p orientation.
 *
 * THE ORIENTATION IS A SENTINEL, NOT JUST A NUMBER: `/tmp/coop-vmirror` selects tile
 * COLUMNS and `/tmp/coop-hh-tiles` selects tile ROWS (docs/coop-modes.md). A co-op
 * session that writes neither runs the retired stacked-halves path, and one that writes
 * both is undefined -- so every launch writes the one it wants AND clears the other,
 * which is the rule the whole three-modes document rests on.
 */
void coop_tunables_arm(int orientation);

/**
 * ARM THE MODE SELECTORS ONLY -- `/tmp/coop-vmirror` and `/tmp/coop-hh-tiles` -- and touch
 * nothing else.
 *
 * WHY THIS EXISTS SEPARATELY FROM coop_tunables_arm(). The `ares-launch "coop":true` path
 * that every script here uses never armed anything: coop_tunables_arm() has exactly one
 * caller, the co-op dialog. That was survivable only while the SHELL could write the
 * sentinels, and it cannot -- measured 2026-08-15, `prisoner` (6122) gets EACCES/EPERM on
 * touch, write AND unlink for every sentinel the app (6584) created, because /tmp is sticky.
 * run_coop.sh hides all of it behind `2>/dev/null`, so selecting vertical from the shell
 * created /tmp/coop-vmirror while SILENTLY FAILING to remove /tmp/coop-hh-tiles: both
 * selectors set at once, which is undefined and reads on the panel as "co-op did not
 * activate". Vertical was unrunnable from a script for that reason alone.
 *
 * The selectors alone, deliberately: calling the full coop_tunables_arm() here would also
 * re-materialise ten other dials from the settings file at stream start, which is a different
 * change with a different risk, and the measured-good configuration on this rig is currently
 * written by run_coop.sh. This restores the one thing that was broken and nothing else.
 */
void coop_tunables_arm_mode(int orientation);

/**
 * Take the mode selectors off disk for an ORDINARY stream.
 *
 * `/tmp/coop-vmirror` is not co-op-only in its effect: session.c halves the requested
 * WIDTH whenever it exists, co-op or not, and `/tmp/coop-slices` rewrites the slice
 * count in the SDP for every stream. A leftover from a co-op run therefore silently
 * reshapes the next single-host stream, which is precisely the "a sentinel left over
 * from another mode made a broken run look like a working one" trap.
 */
void coop_tunables_disarm(void);

/**
 * Per-orientation picture overrides for a co-op session, or 0 to use the shared stream
 * setting. These are not sentinels -- nothing in the stream path reads a file for them --
 * so the session asks at connect time.
 */
int coop_tunables_bitrate(void);

int coop_tunables_fps(void);

int coop_tunables_height(void);

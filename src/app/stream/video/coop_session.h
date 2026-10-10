/*
 * The co-op session as the user configured it in the launch popup: which two hosts
 * take part, which application to run on them, and how the screen splits.
 *
 * WHY THIS EXISTS. Until now the secondary host was named by a DEV-TIME SENTINEL --
 * session_video.c read "<host-uuid> <app-id>" out of /tmp/coop-spawn, which
 * scripts/coop/run_coop.sh wrote over SSH. That is fine for a test rig and useless as
 * a feature: it needs a shell on the TV, it silently outlives the run that armed it
 * (see the cleanup in app_webos.c), and there is no way for a person holding a remote
 * to reach it. This module is the in-process replacement, set by the UI before the
 * stream starts.
 *
 * THE SENTINEL IS DELIBERATELY STILL SUPPORTED as a fallback when nothing has been set
 * here. run_coop.sh and the whole co-op test corpus drive through it, and those are the
 * tools that proved both modes work -- breaking them to land a UI would trade a working
 * harness for an unproven screen.
 *
 * Plain in-memory state rather than a header full of getters into app_configuration:
 * the video thread reads this at session start and must not touch settings storage or
 * the filesystem, and a co-op session is per-run anyway -- what persists (the last pair
 * used) lives in app_settings as coop_primary_uuid / coop_secondary_uuid.
 */
#pragma once

#include <stdbool.h>

#include "coop_stream_mode.h"
#include "uuidstr.h"

#ifdef __cplusplus
extern "C" {
#endif

struct APP_LIST;

/** Room for a uuidstr_t (36 chars + terminator) with slack for the dashless form. */
#define COOP_SESSION_UUID_MAX 64

/** The host-side launcher entry used for each pane of a co-op session. */
#define COOP_SESSION_APP_NAME "Co-op Pane"

typedef struct {
    /** Set by the launch popup or by the scripted pairing snapshot. */
    bool valid;
    /** The host THIS process streams from -- player 1's pane. */
    char primary_uuid[COOP_SESSION_UUID_MAX];
    /** The host the spawned secondary streams from -- player 2's pane. */
    char secondary_uuid[COOP_SESSION_UUID_MAX];
    /** Apollo application ids resolved independently from each host's current app list.
     *  IDs are host-local and must never be assumed to match merely because the entries
     *  have the same name. */
    int primary_app_id;
    int secondary_app_id;
    /** COOP_ORIENTATION_HORIZONTAL / _VERTICAL, snapshotted at launch. Orientation
     *  selects a whole composition path, so it must not change mid-session. */
    int orientation;
    /** COOP_STREAM_CORE or _COMPAT, RESOLVED at launch from both hosts' encoders and
     *  the user's setting -- never AUTO, and never re-derived mid-session.
     *
     *  Snapshotted for the same reason orientation is: it selects a codec, the codec is
     *  negotiated once in the RTSP handshake, and a value that changed underneath (a host
     *  re-probing its encoder, the user editing the setting) would leave the two panes
     *  disagreeing about what they are. Zero means "nothing decided" -- the sentinel
     *  launch path has no dialog to decide it -- and is read as AUTO. */
    int stream_mode;
    /** Tri-state, because "nobody decided" and "decided: no HDR" are different: 0
     *  undecided, 1 allowed, 2 denied. Snapshotted for the same reason the codec is --
     *  the two panes are ONE bitstream with ONE SPS, so they cannot disagree about bit
     *  depth any more than they can about the codec. */
    int hdr_allowed;
    bool gpu_hevc;
} coop_session_t;

/** Resolve the exact co-op launcher entry from one host's app list.
 *
 * @p preferred_id is accepted only when that entry is also named
 * COOP_SESSION_APP_NAME. If it is stale or points at another app, the list is searched
 * by exact name instead. Returns 0 when the host does not expose the co-op entry. */
int coop_session_resolve_app_id(const struct APP_LIST *apps, int preferred_id);

/** Record the session the user just configured. Copied, not referenced. */
void coop_session_set(const coop_session_t *session);

/** The configured session, or NULL when nothing has been set this run. */
const coop_session_t *coop_session_get(void);

/** Forget the configuration. Called when the stream ends so a later ordinary
 *  single-host launch cannot inherit a co-op pairing. */
void coop_session_clear(void);

bool coop_gpu_hevc_resolved(void);

/** Shared UI/script planning. primary_server may supply fresher serverinfo than
 * the host cache; NULL uses the cached report for both hosts. */
coop_stream_plan_t coop_session_plan_for_pair(const uuidstr_t *primary, const uuidstr_t *secondary,
                                             const struct _SERVER_DATA *primary_server);

/** Snapshot the scripted /tmp/coop-spawn pairing before codec/geometry negotiation.
 * Called only by the primary. An existing UI session takes precedence. */
bool coop_session_prepare_scripted(const struct _SERVER_DATA *primary_server, const char *spawn_path);

/**
 * The orientation a session can ACTUALLY run, given its codec.
 *
 * Returns COOP_ORIENTATION_HORIZONTAL for a COMPAT (H.264) session, and @p orientation
 * unchanged otherwise, because the H.264 composer only builds top/bottom today.
 *
 * **CORRECTION, 2026-09-12.** This clamp was first written with the reason "a left/right split
 * is NOT EXPRESSIBLE in H.264, structurally". That is WRONG and was retracted the next day by
 * measurement. What is true is narrower: the VERTICAL composer builds HEVC TILE COLUMNS, and
 * H.264 has no tiles -- so that MECHANISM cannot do it. A different mechanism can:
 *
 *   Each host encodes a HALF-WIDTH pane with ONE SLICE PER MACROBLOCK ROW, and the client
 *   interleaves them -- left row 0, right row 0, left row 1 ... -- rewriting each slice's
 *   first_mb_in_slice to its address in the wider picture. A slice is still a contiguous run in
 *   raster order; it is simply a run that covers half a row.
 *
 * MEASURED OFFLINE 2026-09-12 with the SHIPPING rewriter (tests/coop/avc_sbs_cli.c): a real
 * NVENC pane and a real AMF pane composed side by side, 900 slices rewritten with 0 refused,
 * decoded with ZERO errors both all-intra and with P-frames on the primary, and the composite's
 * left half is pixel-exact to its own solo decode except for the last 18 columns at the seam
 * (mean |diff| 0.001 away from the seam, 1.11 over the final two macroblock columns).
 *
 * So this clamp is now a statement about what is BUILT, not about what is possible, and it
 * stands only until the row-interleave composer exists. Full result and the open gates -- chiefly
 * whether the TV's decoder accepts ~270 slices per picture at real geometry -- are in
 * docs/avc-side-by-side.md.
 */
int coop_orientation_for_mode(int orientation, coop_stream_mode_t mode);

/** Hand the RESOLVED codec path AND the resolved HDR answer to the co-op secondary,
 *  which is spawned later and inherits this process's environment. Call before the
 *  secondary exists; calling it with AUTO is legal and means "nothing decided". */
void coop_stream_mode_publish(coop_stream_mode_t mode, bool hdr_allowed);

/** The codec path THIS process must use: the dialog's decision, else what the primary
 *  published to us, else the standing setting. Returns AUTO only when nothing has
 *  decided -- callers treat that as "leave the codec selection alone". */
coop_stream_mode_t coop_stream_mode_resolved(void);

/** Whether THIS process may advertise 10-bit formats for a co-op session: the dialog's
 *  decision, else what the primary published, else true.
 *
 *  DEFAULTS TO TRUE, which is the user's own `hdr` setting getting the final say when
 *  nothing has overruled it. The alternative -- defaulting to deny -- would turn every
 *  launch path that predates this (the sentinel harness, a relaunch after a PIN entry)
 *  into a silent HDR downgrade. */
bool coop_hdr_allowed_resolved(void);

/** The orientation THIS process must run: the dialog's decision, else what the primary
 *  published to us, else the standing `coop_orientation` setting.
 *
 *  WHY IT CANNOT JUST READ THE SETTING. The settings file PRE-FILLS the dialog; the
 *  dialog's choice is what runs. The dialog writes its choice to
 *  `app_configuration->coop_orientation` in the PRIMARY's memory only (coop.dialog.c),
 *  and the secondary is a `fork()`+`execl()` (coop_coord.c) whose fresh image reloads
 *  moonlight.ini from disk -- so the secondary read the SAVED orientation instead.
 *  MEASURED 2026-09-17 14:34: the user picked VERTICAL while the file still said
 *  horizontal, and the two halves ran different columns of the per-orientation dials --
 *  SKYNET opened at coop_bitrate_v (80000 -> 62988 kbps) and VEGA at coop_bitrate_h
 *  (50000 -> 38988). Geometry was unaffected (the secondary reads the primary-armed
 *  sentinels), so it presented as one pane being quietly encoded at the wrong bitrate. */
int coop_orientation_resolved(void);

#ifdef __cplusplus
}
#endif

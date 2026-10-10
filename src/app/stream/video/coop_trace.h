/*
 * Co-op per-picture trace -- passover 07 §3, "the smallest useful measurement".
 *
 * WHY A SEPARATE FILE AND NOT MORE COUNTERS. Every co-op gauge we had was an aggregate
 * printed once a second: counts, means, and since 2026-09-12 a few maxima. Passover 07 §3
 * asks for something aggregates structurally cannot give -- "keep maxima and event timing,
 * not just means", and a per-picture record of WHICH two source pictures a composite
 * actually represented. A mean cannot say that picture 41,207 carried a peer pane from
 * picture 40,950; only a row per picture can.
 *
 * BOUNDED, AND OFF BY DEFAULT. Passover 07 §3 says "a bounded per-picture trace" for a
 * reason: at a 115 fps composite this writes ~115 rows/s. It is gated on the sentinel
 * /tmp/aurora-agetrace, checked once at open, and stops itself at COOP_TRACE_MAX_ROWS --
 * after which it records that it stopped, so a truncated file can never be mistaken for a
 * session that went quiet.
 *
 * IT IS NOT THE DIAG LOG. The 1 s [COOP-AGE] line stays where it is; this is the raw
 * material behind it, written only when a run is being traced.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "coop_ipc.h"

/** What the composite did with the peer half for this picture. Passover 07 §3.2 wants the
 *  "fresh/held/broken-chain reason" on every accepted feed, not just a count of each. */
typedef enum {
    COOP_TRACE_FRESH = 0,   /**< a newly taken peer pane was spliced in */
    COOP_TRACE_HELD,        /**< the previous peer pane was repeated */
    COOP_TRACE_BROKEN,      /**< peer chain broken: pane synthesized/concealed */
    COOP_TRACE_CATCHUP,     /**< an extra composite emitted to drain the peer backlog */
    COOP_TRACE_BOOTSTRAP,   /**< paired with the sticky peer IDR */
    COOP_TRACE_MIRROR,      /**< diagnostic: both panes use the primary, no peer provenance */
    COOP_TRACE__COUNT
} coop_trace_kind_t;

/** One composed picture. Zero-initialise and fill what is known; unknown fields stay 0. */
typedef struct {
    coop_trace_kind_t kind;
    /* Primary source identity -- read from the primary's own decode unit. */
    uint32_t prim_frame;
    uint32_t prim_pts_ms;
    uint32_t prim_recv_ms;
    uint32_t prim_reasm_ms;
    uint16_t prim_host_10us;
    /* Peer source identity -- carried across the IPC link (coop_ipc_hdr_t). */
    coop_ipc_hdr_t peer;
    unsigned long peer_arrival_age_ms;  /**< complete arrival here -> this take */
    uint32_t ipc_dwell_ms;              /**< secondary send entry -> complete receipt here */
    uint32_t send_block_ms;             /**< secondary's last write_all() duration */
    /* What we then did with it. §3.2: "Distinguish attempting a feed from successfully
     * submitting it", and record feed entry AND exit, not just the result. */
    unsigned long feed_enter_mono_ms;
    unsigned long feed_exit_mono_ms;
    int feed_result;                    /**< SS4S_VideoFeedResult */
    uint32_t bytes;
    unsigned queued_after;              /**< peer panes still queued after this take */
} coop_trace_pic_t;

/** Live durations, sampled by the heartbeat as well as written on every picture. */
typedef struct {
    unsigned long hold_open_ms;
    unsigned long hold_max_ms;
    unsigned long broken_open_ms;
    unsigned long broken_max_ms;
    unsigned long recov_req_ms;
    unsigned long recov_anchor_ms;
    unsigned queued;
} coop_trace_state_t;

/** Supplied by the video path so the heartbeat can sample durations from its own thread. */
typedef void (*coop_trace_sampler_t)(coop_trace_state_t *out);

/**
 * Open the trace if /tmp/aurora-agetrace exists, and start the heartbeat thread.
 * Safe to call more than once; only the first call does anything. Returns true if tracing.
 */
bool coop_trace_open(coop_trace_sampler_t sampler);

/** Close the trace and stop the heartbeat. Safe if never opened. */
void coop_trace_close(void);

/** True if rows are actually being written -- check before doing work just for the trace. */
bool coop_trace_active(void);

/** Write one composed-picture row. No-op when inactive. */
void coop_trace_pic(const coop_trace_pic_t *pic);

/** Monotonic ms on the same clock every stamp in this trace uses. */
unsigned long coop_trace_now_ms(void);

/*
 * THE PANEL IDENTITY OVERLAY -- passover 07 §3.4.
 *
 * "Film the panel together with a TV-local diagnostic overlay showing latest received and
 * accepted identities. The overlay and trace need one monotonic timebase and matching session
 * epoch." A camera frame then carries, in one exposure, what the glass is showing AND what the
 * client believed it had just accepted -- which is the only way to separate "an old picture is
 * being held" from "a queue is playing old pictures". A 3 s screenshot sampler cannot do this.
 *
 * DELIBERATELY NOT GATED ON THE TRACE SENTINEL. The overlay is the half of the measurement a
 * human performs, and requiring a sentinel to be armed first is how that gets discovered after
 * the run rather than before it. It costs three word reads per stats refresh.
 */
void coop_trace_identity_set(uint32_t prim_frame, uint32_t peer_accepted_frame);

/** Latest primary frame fed, latest peer frame ACCEPTED into a composite, latest peer frame
 *  RECEIVED, and the monotonic clock the trace stamps with. Any may be NULL.
 *  Returns false if no co-op composite has been built this session. */
bool coop_trace_identity_get(uint32_t *prim_frame, uint32_t *peer_accepted,
                             uint32_t *peer_received, unsigned long *mono_ms);

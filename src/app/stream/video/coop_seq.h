/*
 * Co-op split-screen: the half-height pane SEQUENCING state machine.
 *
 * Which of the peer's pictures may be spliced into ours, and what to ask the peer for when
 * none may be. Extracted from session_video.c so it can be exercised offline: the defects
 * here are all ORDERING defects -- a livelock between the gap detector and the sticky IDR,
 * an anchor that could never be met -- and reproducing those live costs a two-host session
 * with the second PC free, which is exactly what was unavailable when they were found.
 *
 * PURE BY CONSTRUCTION. No sockets, no bitstream, no logging, no clock: the caller performs
 * every side effect the plan/verdict asks for. That is what makes a measured failure trace
 * replayable as a test -- see tests/coop/test_coop_seq.c, which replays the 401-re-anchor
 * livelock and the unachievable post-bootstrap anchor.
 *
 * WHY SEQUENCE AT ALL: each of the peer's inter pictures predicts from the peer's PREVIOUS
 * one, so feeding k+3 after k hands the decoder a reference the composite never held. The
 * bottom pane then looks perfect on the bootstrap IRAP and collapses to residual-only about
 * 30 pictures later.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** What the caller should try to take from the coordinator for THIS picture of ours. */
typedef enum {
    /** Our picture is an IRAP, so only an IRAP of the peer's can pair with it (every VCL
     *  NAL of one picture must carry the same nal_unit_type): take the sticky IDR. */
    COOP_SEQ_TAKE_IDR = 0,
    /** Our picture is inter: pop the oldest queued pane whose frame number is >= min_frame. */
    COOP_SEQ_TAKE_NEXT,
} coop_seq_kind_t;

typedef struct {
    coop_seq_kind_t kind;
    /** COOP_SEQ_TAKE_NEXT only. 0 means "not yet anchored" -- accept whatever comes. */
    uint32_t min_frame;
    /** Call the CLEARING peer-IDR request (coop_coord_request_peer_idr) before taking, to
     *  drop a sticky IDR that may be seconds old. Once per bootstrap attempt, never more:
     *  clearing on every request throws away each freshly arrived IDR just before our next
     *  IRAP wants it, and bootstrap never completes (measured: boot=0 for 721 pictures). */
    bool request_idr_clearing;
} coop_seq_plan_t;

typedef struct {
    /** The pane may be spliced. */
    bool accept;
    /** Call the KEEP peer-IDR request (coop_coord_request_peer_idr_keep) -- ask for a
     *  keyframe while retaining whatever sticky IDR arrives. Using the clearing variant
     *  here is the livelock: see coop_seq_verdict(). */
    bool request_idr_keep;
    /** Drop the retained last-peer AU: it is on the far side of a hole, so repeating it
     *  would conceal with a picture the composite's reference chain never contained. */
    bool drop_lastpeer;
} coop_seq_verdict_t;

typedef struct {
    /** The peer's own frame number we expect next; 0 = not yet anchored. */
    uint32_t peer_next;
    /** We currently hold a usable anchor: a paired IRAP has happened and no hole has been
     *  seen since. Cleared by a gap or a forced refresh, so it says "composing right now",
     *  NOT "composition has ever worked". */
    bool bootstrapped;
    /** A composite has been emitted AT SOME POINT this session. Latched: never cleared
     *  except by a session reset.
     *
     *  THE DIFFERENCE BETWEEN THESE TWO FLAGS IS A 29%-OF-PICTURES STUTTER. Dropping a
     *  picture entirely is only ever legitimate BEFORE the first composite, and for exactly
     *  one reason: our own parameter sets say pic_height 1088, and the webOS pipeline sizes
     *  its video plane from the FIRST SPS it sees and never resizes. Once any composite has
     *  gone out that geometry is already established, so suppressing achieves nothing and
     *  costs a dropped frame. Gating suppression on `bootstrapped` meant every gap and every
     *  forced refresh re-entered the drop-everything state mid-session: measured 5521
     *  pictures suppressed in one run (~29%), with `reparam=0` proving the intended fallback
     *  never once fired because it was gated on the same flag. The user saw it as BOTH panes
     *  stuttering -- including the primary's own mouse movement, which no pane-delivery fault
     *  could explain. */
    bool ever_bootstrapped;
    /** The once-per-bootstrap clearing request has been issued. */
    bool idr_cleared;
    /* Diagnostic counters, reported in the [COOP-HH] line. */
    unsigned peergap;       /**< holes refused */
    unsigned idr_unpaired;  /**< our IRAPs with no peer IDR to pair with */
    unsigned stale_dropped; /**< panes discarded as older than min_frame */
} coop_seq_t;

/** Per-session reset. Every field: a stale anchor or a stale `bootstrapped` both corrupt
 *  the first pictures of the next session. */
void coop_seq_reset(coop_seq_t *s);

/** Decide what to take for a picture of ours, and record that decision. */
void coop_seq_plan(coop_seq_t *s, bool mine_irap, coop_seq_plan_t *out);

/**
 * Judge what the coordinator actually returned.
 * @p got_pane / @p frame / @p dropped are the take's results (@p frame is meaningful only
 * for COOP_SEQ_TAKE_NEXT, @p dropped is its skipped_out).
 */
void coop_seq_verdict(coop_seq_t *s, const coop_seq_plan_t *plan, bool got_pane,
                      uint32_t frame, unsigned dropped, coop_seq_verdict_t *out);

/**
 * A composite was successfully built. Returns true if the caller must flush the pane queue
 * (queued panes predate the keyframe we just bootstrapped on).
 */
bool coop_seq_on_paired(coop_seq_t *s, bool mine_irap);

/**
 * True when the sticky peer IDR (the peer's frame @p idr_frame) is OLDER than a pane the
 * composite has already accepted, i.e. the peer's tile has moved past it. Pairing our IRAP
 * with it would reset that tile to the older picture while every later peer pane predicts from
 * the current one -- and nothing then asks for a fresh keyframe, because a pairing is not a
 * break. False when not anchored (peer_next == 0): no evidence either way.
 */
bool coop_seq_idr_superseded(const coop_seq_t *s, uint32_t idr_frame);

/**
 * Drop back to un-bootstrapped so the normal path -- suppress, ask both hosts for a
 * keyframe, resume on the next paired IRAP -- rebuilds from a fresh keyframe. Used when
 * repeat-concealment has decayed past its budget. The caller issues the CLEARING peer-IDR
 * request itself: the held IDR is stale by definition at that point.
 */
void coop_seq_force_refresh(coop_seq_t *s);

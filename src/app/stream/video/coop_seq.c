#include "coop_seq.h"

#include <string.h>

void coop_seq_reset(coop_seq_t *s) {
    memset(s, 0, sizeof(*s));
}

void coop_seq_plan(coop_seq_t *s, bool mine_irap, coop_seq_plan_t *out) {
    memset(out, 0, sizeof(*out));
    if (mine_irap) {
        out->kind = COOP_SEQ_TAKE_IDR;
        /* THE STICKY IDR MUST BE FRESH, AND EXACTLY ONCE-CLEARED. The coordinator keeps the
         * peer's most recent IDR indefinitely, so at bootstrap it can be seconds old, and
         * splicing a stale one puts a picture in the composite that the peer's SUBSEQUENT
         * slices were never coded against -- a bottom pane that only accumulates residual. */
        if (!s->idr_cleared) {
            s->idr_cleared = true;
            out->request_idr_clearing = true;
        }
        return;
    }
    out->kind = COOP_SEQ_TAKE_NEXT;
    out->min_frame = s->peer_next;
}

void coop_seq_verdict(coop_seq_t *s, const coop_seq_plan_t *plan, bool got_pane,
                      uint32_t frame, unsigned dropped, coop_seq_verdict_t *out) {
    memset(out, 0, sizeof(*out));
    if (plan->kind == COOP_SEQ_TAKE_IDR) {
        if (!got_pane) {
            s->idr_unpaired++;
            out->request_idr_keep = true;   /* waiting: keep what arrives */
            return;
        }
        out->accept = true;
        return;
    }

    s->stale_dropped += dropped;
    if (!got_pane) {
        /* EVERY QUEUED PANE WAS OLDER THAN THE ANCHOR. take_next only discards panes below
         * min_frame, so this is the peer's frame numbering having RESTARTED -- its session
         * reconnected and began counting from 0 again. The anchor can then never be met, and
         * every pane for the rest of the session is discarded as stale. Drop the anchor so
         * the next pane re-establishes it. */
        if (dropped > 0) {
            s->peer_next = 0;
            s->peergap++;
        }
        return;
    }
    if (s->peer_next != 0 && frame != s->peer_next) {
        /* A HOLE. RE-ANCHOR ON IT AND SPLICE IT ANYWAY -- refusing is strictly worse, and
         * measuring that is what this rule cost.
         *
         * The old rule refused the pane and waited for a fresh PAIRED IRAP. Nothing ever
         * delivered one: our host runs idr_refresh_interval_sec=0 and the only code that asks
         * it for a keyframe is gated on `!ever_bootstrapped`, so after the first composite
         * there is no path back. Measured live 2026-08-01, one 78s two-host run: a single
         * missed pane at frame 16 froze `expected` at 16 for the WHOLE SESSION while the peer
         * ran on to 2905 -- peergap=401, paired=12, fallback=1788, boot=0.
         *
         * It is not merely a frozen pane either; refusing feeds back into the pane supply.
         * Every refusal asks the peer for a keyframe, so PC2 received 550 IDR requests in 78s
         * (~7/s), encoded near-all-intra at 3840x1080, and the resulting bitrate collapsed its
         * own delivery: the secondary forwarded 5 access units a second against a peer frame
         * counter advancing at 48/s. The refusal caused the shortage it was reacting to.
         *
         * Splicing across the hole costs artefacts in the bottom pane until the peer's next
         * IDR, because its inter slices predict from a picture the composite never held. That
         * is bounded and self-healing now that a peer IDR no longer needs one of ours to pair
         * with -- coop_slice_idr_to_trail() folds it into whatever TRAIL picture we are
         * sending (confirmed on the TV, commit 88bf13a5). So: count the gap, ask for that
         * keyframe (the KEEP variant -- the clearing one livelocks, see the IDR path above),
         * drop the retained peer AU because it is on the far side of the hole, and carry on
         * composing from the pane in hand.
         *
         * `bootstrapped` deliberately stays SET. It means "composing right now", and we are. */
        s->peergap++;
        out->drop_lastpeer = true;
        out->request_idr_keep = true;
    }
    s->peer_next = frame + 1u;
    out->accept = true;
}

bool coop_seq_on_paired(coop_seq_t *s, bool mine_irap) {
    /* BOOTSTRAP MEANS A PAIRED *IRAP*, NOT MERELY A PAIRED PICTURE. Treating any successful
     * splice as bootstrap is how the composite ended up with no keyframe at all: the first
     * pairing that happened to succeed was an inter picture and we began feeding from there
     * (measured: 175 composed pictures, IRAP count ZERO, first picture TRAIL_R at POC 5). */
    if (!mine_irap) {
        return false;
    }
    s->bootstrapped = true;
    /* Latched for the rest of the session: from here on the decoder's plane geometry is
     * established, so a picture we cannot compose must still GO OUT (our slice with the
     * height-doubled parameter sets) rather than be dropped. See coop_seq.h. */
    s->ever_bootstrapped = true;
    s->idr_cleared = false;     /* re-arm for the next bootstrap */
    /* ANCHOR ON THE FIRST PANE WE ACTUALLY ACCEPT, NOT ON THE KEYFRAME.
     *
     * Anchoring to the peer's IDR frame + 1 is what it "should" be, and it is unachievable:
     * time passes between the peer's IDR landing in the sticky slot and our host producing
     * the IRAP to pair it with, and the flush the caller is about to perform drops the panes
     * from that gap. So the next pane is ALWAYS a few frames past the keyframe, the gap
     * detector fires on the very first picture after every bootstrap, and the composite can
     * never get going. 0 means "not yet anchored": the next pane is accepted whatever its
     * number and becomes the anchor. That costs ONE discontinuity immediately after the
     * keyframe, against a pane that otherwise never works at all. */
    s->peer_next = 0;
    return true;                /* queued panes predate this keyframe */
}

bool coop_seq_idr_superseded(const coop_seq_t *s, uint32_t idr_frame) {
    /* peer_next is the last ACCEPTED pane + 1, from the normal take and catch-up alike. Equal
     * is not superseded: that IDR is the tile's current picture. */
    return s->peer_next != 0 && idr_frame + 1u < s->peer_next;
}

void coop_seq_force_refresh(coop_seq_t *s) {
    s->bootstrapped = false;
}

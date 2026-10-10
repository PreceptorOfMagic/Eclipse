/*
 * Half-height pane SEQUENCING, replayed offline.
 *
 * The two defects this logic has had were both pure ordering -- a livelock between the gap
 * detector and the sticky IDR, and a post-bootstrap anchor that could never be met -- and
 * both were found by reading a live two-host session's counters. That test costs the second
 * PC being free, so it could not be re-run to confirm either fix. This replays the measured
 * traces instead: no sockets, no bitstream, no decoder.
 *
 * The fake coordinator below models coop_coord.c's queue EXACTLY (depth 3 of 4 slots,
 * oldest dropped on overflow, IDRs landing in both the sticky slot and the queue, take_next
 * discarding older panes) because the queue overflow IS the mechanism that produces the
 * holes: whenever our host outruns the peer's delivery, panes fall out of the queue and the
 * next one taken is newer than expected.
 *
 * Each test that asserts a fix also runs the PRE-FIX rule (legacy_* below) over the same
 * trace and asserts it fails, so a regression cannot pass by accident.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "coop_seq.h"

static int failures = 0;
static const char *current_test = "";

#define CHECK(cond, ...)                                                                   \
    do {                                                                                   \
        if (!(cond)) {                                                                      \
            printf("  FAIL [%s] %s:%d: ", current_test, __FILE__, __LINE__);                \
            printf(__VA_ARGS__);                                                            \
            printf("\n");                                                                   \
            failures++;                                                                     \
        }                                                                                   \
    } while (0)

/* ---------------------------------------------------------------- fake coordinator ---- */

#define FAKE_Q_SLOTS 4
#define FAKE_Q_DEPTH 3
/* coop_coord.c reads its depth once at start from /tmp/coop-queue, capped at COOP_Q_SLOTS.
 * Settable here so the harness can measure what that knob is actually worth. */
static unsigned fake_q_depth = FAKE_Q_DEPTH;

typedef struct {
    uint32_t frame[FAKE_Q_SLOTS];
    bool is_idr[FAKE_Q_SLOTS];
    unsigned head, count;
    bool have_idr;
    uint32_t idr_frame;
    unsigned idr_requests;      /* every request, clearing or keep -- the "840 sent" counter */
    bool idr_pending;           /* the peer owes us a keyframe */
} fake_coord_t;

/* The drain thread's arrival path: sticky slot for IDRs, then the bounded queue. */
static void fake_arrive(fake_coord_t *c, uint32_t frame, bool is_idr) {
    if (is_idr) {
        c->have_idr = true;
        c->idr_frame = frame;
    }
    if (c->count >= fake_q_depth) {
        c->head = (c->head + 1u) % FAKE_Q_SLOTS;   /* full: drop the OLDEST */
        c->count--;
    }
    unsigned slot = (c->head + c->count) % FAKE_Q_SLOTS;
    c->frame[slot] = frame;
    c->is_idr[slot] = is_idr;
    c->count++;
}

/* Not consumed: bootstrap retries until our own host produces an IRAP to pair it with. */
static bool fake_take_idr(fake_coord_t *c, uint32_t *frame_out) {
    if (!c->have_idr) { return false; }
    if (frame_out != NULL) { *frame_out = c->idr_frame; }
    return true;
}

static bool fake_take_next(fake_coord_t *c, uint32_t min_frame, uint32_t *frame_out,
                           unsigned *skipped_out) {
    unsigned skipped = 0;
    bool got = false;
    while (c->count > 0) {
        unsigned slot = c->head;
        uint32_t f = c->frame[slot];
        c->head = (c->head + 1u) % FAKE_Q_SLOTS;
        c->count--;
        if (f < min_frame) { skipped++; continue; }   /* stale leftover */
        if (frame_out != NULL) { *frame_out = f; }
        got = true;
        break;
    }
    if (skipped_out != NULL) { *skipped_out = skipped; }
    return got;
}

static void fake_request_clearing(fake_coord_t *c) {
    c->have_idr = false;        /* THE livelock: the answer is thrown away before it is used */
    c->idr_requests++;
    c->idr_pending = true;
}

static void fake_request_keep(fake_coord_t *c) {
    c->idr_requests++;
    c->idr_pending = true;
}

static void fake_flush(fake_coord_t *c) { c->count = 0; }

/* ------------------------------------------------------------------- two-host sim ---- */

typedef struct {
    /* Peer pictures delivered per picture of ours. Measured over one 119s run: our host
     * produced 10800 while the peer delivered 7117 -- SKYNET ~91fps against PC2 ~62fps. */
    double peer_rate;
    /* Pictures of ours between asking the peer for a keyframe and its arrival. */
    unsigned peer_idr_latency;
    /* Pictures of ours between returning DR_NEED_IDR and our host actually emitting one. */
    unsigned our_idr_latency;
    /* DELIVERY JITTER, and it is what actually creates the holes. A peer running SLOWER than
     * us can never overflow a queue we drain every picture -- yet holes were measured, and
     * coop_coord.c's own numbers say why: recv=7209 with replaced=2847 on a single slot,
     * i.e. the panes arrive in gulps, not evenly. Frames produced during a burst period are
     * delivered together; a burst deeper than the queue drops the oldest, and the sequence
     * the caller expects now has a hole in it. */
    unsigned burst;
} sim_cfg_t;

typedef struct {
    fake_coord_t coord;
    double peer_accum;
    uint32_t peer_frame;
    unsigned peer_idr_countdown;    /* 0 = not owed */
    unsigned our_idr_countdown;     /* 0 = not requesting */
    unsigned pictures;
    unsigned paired;
    unsigned bootstraps;
    uint32_t last_accepted;
    bool ever_accepted;
    /* Produced by the peer but not yet handed to the coordinator (in flight). */
    uint32_t pend_frame[64];
    bool pend_idr[64];
    unsigned pend_count;
} sim_t;

static void sim_reset(sim_t *s) {
    memset(s, 0, sizeof(*s));
    s->peer_frame = 1;
}

/* Advance the peer by one of our picture periods, delivering in bursts. */
static void sim_peer_tick(sim_t *s, const sim_cfg_t *cfg) {
    if (s->coord.idr_pending && s->peer_idr_countdown == 0) {
        s->peer_idr_countdown = cfg->peer_idr_latency > 0 ? cfg->peer_idr_latency : 1;
    }
    s->peer_accum += cfg->peer_rate;
    while (s->peer_accum >= 1.0) {
        s->peer_accum -= 1.0;
        bool idr = false;
        if (s->peer_idr_countdown > 0 && --s->peer_idr_countdown == 0) {
            idr = true;
            s->coord.idr_pending = false;
        }
        if (s->pend_count < 64) {
            s->pend_frame[s->pend_count] = s->peer_frame;
            s->pend_idr[s->pend_count] = idr;
            s->pend_count++;
        }
        s->peer_frame++;
    }
    const unsigned burst = cfg->burst > 0 ? cfg->burst : 1;
    if (s->pictures % burst == 0) {
        for (unsigned i = 0; i < s->pend_count; ++i) {
            fake_arrive(&s->coord, s->pend_frame[i], s->pend_idr[i]);
        }
        s->pend_count = 0;
    }
}

/* Our host: emits an IRAP only when asked, since idr_refresh_interval_sec=0. */
static bool sim_our_irap(sim_t *s, const sim_cfg_t *cfg, bool bootstrapped) {
    if (!bootstrapped && s->our_idr_countdown == 0) {
        s->our_idr_countdown = cfg->our_idr_latency > 0 ? cfg->our_idr_latency : 1;
    }
    if (s->our_idr_countdown > 0 && --s->our_idr_countdown == 0) {
        return true;
    }
    return false;
}

/* One picture of ours, through the REAL state machine. */
static void sim_step(sim_t *s, const sim_cfg_t *cfg, coop_seq_t *seq) {
    s->pictures++;
    sim_peer_tick(s, cfg);
    const bool mine_irap = sim_our_irap(s, cfg, seq->bootstrapped);

    coop_seq_plan_t plan;
    coop_seq_plan(seq, mine_irap, &plan);

    uint32_t frame = 0;
    unsigned dropped = 0;
    bool got;
    if (plan.kind == COOP_SEQ_TAKE_IDR) {
        if (plan.request_idr_clearing) { fake_request_clearing(&s->coord); }
        got = fake_take_idr(&s->coord, &frame);
    } else {
        got = fake_take_next(&s->coord, plan.min_frame, &frame, &dropped);
    }

    coop_seq_verdict_t v;
    coop_seq_verdict(seq, &plan, got, frame, dropped, &v);
    if (v.request_idr_keep) { fake_request_keep(&s->coord); }

    if (!v.accept) { return; }
    /* The splice succeeds: composition, geometry and POC discipline are separately proven
     * (test_coop_halves.c and the offline decode), so this test assumes them. */
    s->paired++;
    s->last_accepted = frame;
    s->ever_accepted = true;
    if (coop_seq_on_paired(seq, mine_irap)) {
        s->bootstraps++;
        fake_flush(&s->coord);
    }
}

/* ------------------------------------------------------------- the PRE-FIX rule ------ */

/* The state machine as it was before commit 717ef221, so each test can show the trace it
 * replays actually discriminates. Two differences, both in this function:
 *   - a gap called the CLEARING peer-IDR request rather than the keep variant;
 *   - bootstrap anchored the expected sequence to the peer's IDR frame + 1. */
typedef struct {
    uint32_t peer_next;
    bool bootstrapped;
    bool idr_cleared;
    unsigned peergap;
    unsigned reanchor_attempts;
} legacy_seq_t;

static void legacy_step(sim_t *s, const sim_cfg_t *cfg, legacy_seq_t *g) {
    s->pictures++;
    sim_peer_tick(s, cfg);
    const bool mine_irap = sim_our_irap(s, cfg, g->bootstrapped);

    uint32_t frame = 0;
    bool got;
    if (mine_irap) {
        if (!g->idr_cleared) {
            g->idr_cleared = true;
            fake_request_clearing(&s->coord);
        }
        got = fake_take_idr(&s->coord, &frame);
        if (!got) { fake_request_keep(&s->coord); }
    } else {
        unsigned dropped = 0;
        got = fake_take_next(&s->coord, g->peer_next, &frame, &dropped);
        if (got && g->peer_next != 0 && frame != g->peer_next) {
            g->peergap++;
            g->reanchor_attempts++;
            g->bootstrapped = false;
            fake_request_clearing(&s->coord);       /* <-- the livelock */
            return;
        }
        if (got) { g->peer_next = frame + 1u; }
    }
    if (!got) { return; }
    s->paired++;
    s->last_accepted = frame;
    if (mine_irap) {
        g->bootstrapped = true;
        g->idr_cleared = false;
        g->peer_next = s->coord.idr_frame + 1u;     /* <-- the unachievable anchor */
        fake_flush(&s->coord);
        s->bootstraps++;
    }
}

/* ------------------------------------------------------------------------ tests ------ */

/* SWEEP THE TIMING RATHER THAN PICK ONE OFFSET. How many of our pictures pass between the
 * peer's IDR landing and our IRAP pairing with it is not a knowable constant -- it depends
 * on two independent encoders -- and a single hand-chosen value would be fitting the test to
 * the answer. So the claim these tests make is stronger and honest: the current rule holds
 * across EVERY plausible timing, and the pre-fix rule fails somewhere inside it. */
#define SWEEP_PICTURES 2000u

/* TWO DIFFERENT FAILURES, and the first sweep run conflated them -- worth keeping separate,
 * because only one of them is what commit 717ef221 claims to fix:
 *   LIVELOCK   the anchor freezes and never recovers; the pane is built on a keyframe tens of
 *              seconds old and nothing pairs again for the rest of the session. Fatal.
 *   LOW YIELD  it recovers, but each hole costs a re-anchor, so few pictures get a FRESH pane
 *              and concealment carries the rest. Degraded, not broken -- and it shows as the
 *              ghosting this project has chased. */
#define LIVELOCK_FROZEN_PICTURES 300u

typedef struct {
    unsigned combos;
    unsigned new_livelocked;
    unsigned legacy_livelocked;
    unsigned new_worse_than_legacy;
    unsigned worst_paired_pct;              /* over combos with burst <= queue depth */
    sim_cfg_t worst;
    unsigned yield_by_burst[8];             /* mean fresh-pairing %, indexed by burst */
    unsigned yield_samples[8];
} sweep_t;

/* Our host ~91fps against the peer's ~62: measured over one 119s run (10800 vs 7117). */
#define MEASURED_RATE (62.0 / 91.0)

static void sweep(sweep_t *sw) {
    memset(sw, 0, sizeof(*sw));
    sw->worst_paired_pct = 100;
    for (unsigned our_lat = 1; our_lat <= 8; ++our_lat) {
        for (unsigned peer_lat = 1; peer_lat <= 10; ++peer_lat) {
            for (unsigned burst = 1; burst <= 5; ++burst) {
                sim_cfg_t cfg = { .peer_rate = MEASURED_RATE, .peer_idr_latency = peer_lat,
                                  .our_idr_latency = our_lat, .burst = burst };
                sw->combos++;

                sim_t s;
                coop_seq_t seq;
                sim_reset(&s);
                coop_seq_reset(&seq);
                unsigned frozen_at = 0, frozen_for = 0, worst_frozen = 0;
                for (unsigned i = 0; i < SWEEP_PICTURES; ++i) {
                    sim_step(&s, &cfg, &seq);
                    if (seq.peer_next == frozen_at) {
                        if (++frozen_for > worst_frozen) { worst_frozen = frozen_for; }
                    } else {
                        frozen_at = seq.peer_next;
                        frozen_for = 0;
                    }
                }
                /* The peer delivers ~68 panes per 100 pictures of ours, so ~68% is the
                 * ceiling on FRESH pairings. */
                unsigned pct = (s.paired * 100u) / SWEEP_PICTURES;
                if (worst_frozen > LIVELOCK_FROZEN_PICTURES || s.bootstraps == 0) {
                    sw->new_livelocked++;
                }
                sw->yield_by_burst[burst] += pct;
                sw->yield_samples[burst]++;
                /* The floor is only claimed where delivery jitter fits the queue; deeper
                 * bursts drop panes before we ever see them, which no sequencing rule can
                 * undo. Reported below rather than asserted. */
                if (burst <= FAKE_Q_DEPTH && pct < sw->worst_paired_pct) {
                    sw->worst_paired_pct = pct;
                    sw->worst = cfg;
                }

                sim_t ls;
                legacy_seq_t g;
                sim_reset(&ls);
                memset(&g, 0, sizeof(g));
                frozen_at = frozen_for = worst_frozen = 0;
                for (unsigned i = 0; i < SWEEP_PICTURES; ++i) {
                    legacy_step(&ls, &cfg, &g);
                    if (g.peer_next == frozen_at) {
                        if (++frozen_for > worst_frozen) { worst_frozen = frozen_for; }
                    } else {
                        frozen_at = g.peer_next;
                        frozen_for = 0;
                    }
                }
                unsigned lpct = (ls.paired * 100u) / SWEEP_PICTURES;
                if (worst_frozen > LIVELOCK_FROZEN_PICTURES || ls.bootstraps == 0) {
                    sw->legacy_livelocked++;
                }
                if (pct < lpct) { sw->new_worse_than_legacy++; }
            }
        }
    }
}

static void test_sequencing_holds_across_timings(void) {
    current_test = "sequencing holds across every plausible timing";
    sweep_t sw;
    sweep(&sw);
    printf("  %u timing combinations (our IDR latency 1-8 x peer 1-10 x burst 1-5)\n",
           sw.combos);
    printf("  livelocks: current rule %u, pre-fix rule %u\n",
           sw.new_livelocked, sw.legacy_livelocked);
    printf("  fresh-pane yield within the queue depth: worst %u%% (our_lat=%u peer_lat=%u "
           "burst=%u); ceiling is ~68%%\n", sw.worst_paired_pct, sw.worst.our_idr_latency,
           sw.worst.peer_idr_latency, sw.worst.burst);
    printf("  yield vs delivery burst (queue depth %u):", (unsigned) FAKE_Q_DEPTH);
    for (unsigned b = 1; b <= 5; ++b) {
        printf(" burst%u=%u%%", b, sw.yield_by_burst[b] / sw.yield_samples[b]);
    }
    printf("\n");

    CHECK(sw.new_livelocked == 0, "the current rule livelocks in %u of %u timings",
          sw.new_livelocked, sw.combos);
    /* Without this the sweep proves nothing: it would pass on a state machine that ignores
     * sequencing entirely. The pre-fix rule must fail the same traces, and fail the way the
     * live run did -- `expected` frozen while the peer runs on (measured: frozen at 1316,
     * peer at 5403, 401 re-anchor attempts, 840 IDR requests sent). */
    CHECK(sw.legacy_livelocked > 0, "the PRE-FIX rule never livelocked anywhere in this "
                                    "sweep, so the sweep is not evidence for the fix");
    CHECK(sw.new_worse_than_legacy == 0, "the current rule pairs FEWER pictures than the "
                                         "pre-fix rule in %u timings", sw.new_worse_than_legacy);
    /* Within the jitter the queue can absorb, the pane must actually be fed. */
    CHECK(sw.worst_paired_pct >= 34, "worst fresh-pane yield %u%% inside the queue depth -- "
                                     "concealment is carrying the picture", sw.worst_paired_pct);
}

static void test_queue_depth_sets_the_jitter_the_pane_survives(void) {
    current_test = "queue depth sets the jitter the pane survives";
    /* WHAT DEPTH IS WORTH, measured rather than guessed -- it is runtime-tunable via
     * /tmp/coop-queue (capped at 4 slots), so the knob deserves a number.
     *
     * THE CLIFF USED TO BE A CLIFF. Under the old refuse-on-gap rule a pane dropped by queue
     * overflow was not a late pane, it was a HOLE, and every hole cost a full re-anchor:
     * yield was flat until a delivery burst exceeded the depth and then collapsed (depth 3
     * measured 68% at burst 3 against 39% at burst 5). Re-anchoring ON the gap instead means
     * an overflow now costs only the panes it actually dropped, so the same sweep reads
     * 68% -> 59%. That flattening IS the fix; what remains is the unavoidable arithmetic of
     * panes that never reached the queue. */
    printf("  fresh-pane yield, delivery burst vs queue depth:\n");
    unsigned yield[FAKE_Q_SLOTS + 1][8];
    memset(yield, 0, sizeof(yield));
    for (unsigned depth = 1; depth <= FAKE_Q_SLOTS; ++depth) {
        fake_q_depth = depth;
        printf("    depth %u:", depth);
        for (unsigned burst = 1; burst <= 6; ++burst) {
            sim_cfg_t cfg = { .peer_rate = MEASURED_RATE, .peer_idr_latency = 6,
                              .our_idr_latency = 3, .burst = burst };
            sim_t s;
            coop_seq_t seq;
            sim_reset(&s);
            coop_seq_reset(&seq);
            for (unsigned i = 0; i < SWEEP_PICTURES; ++i) { sim_step(&s, &cfg, &seq); }
            yield[depth][burst] = (s.paired * 100u) / SWEEP_PICTURES;
            printf(" burst%u=%u%%", burst, yield[depth][burst]);
        }
        printf("\n");
    }
    fake_q_depth = FAKE_Q_DEPTH;

    /* A deeper queue must never yield LESS: if it did, depth would be trading pane freshness
     * for nothing and the /tmp/coop-queue advice would be backwards. */
    for (unsigned burst = 1; burst <= 6; ++burst) {
        for (unsigned depth = 2; depth <= FAKE_Q_SLOTS; ++depth) {
            CHECK(yield[depth][burst] + 2u >= yield[depth - 1][burst],
                  "depth %u yields %u%% at burst %u, worse than depth %u's %u%%",
                  depth, yield[depth][burst], burst, depth - 1, yield[depth - 1][burst]);
        }
    }
    /* Overflow must still COST something, or the model is not producing overflows at all and
     * this whole harness is measuring nothing. It just must no longer be a cliff. */
    CHECK(yield[3][5] + 4u < yield[3][3], "no yield loss at all at depth 3 (burst3=%u%% "
          "burst5=%u%%) -- overflow is not dropping panes, so this model is wrong",
          yield[3][3], yield[3][5]);
    CHECK(yield[3][3] - yield[3][5] < 20u, "overflow at depth 3 still costs %u points "
          "(burst3=%u%% burst5=%u%%) -- a dropped pane is being charged as a re-anchor again",
          yield[3][3] - yield[3][5], yield[3][3], yield[3][5]);
    CHECK(yield[4][5] > yield[3][5], "raising the depth from 3 to 4 did not help at burst 5 "
          "(%u%% vs %u%%)", yield[4][5], yield[3][5]);
}

static void test_a_hole_is_reported_and_re_anchored(void) {
    current_test = "a hole is reported, spliced and re-anchored on";
    /* A hole still has to be VISIBLE -- counted, the retained peer AU dropped, a keyframe
     * asked for -- because splicing across it does cost artefacts until the peer's next IDR.
     * What it must NOT do any more is refuse the pane: see the anchor-freeze test below. */
    coop_seq_t seq;
    coop_seq_reset(&seq);
    coop_seq_plan_t plan;
    coop_seq_verdict_t v;

    coop_seq_plan(&seq, false, &plan);
    CHECK(plan.min_frame == 0, "a fresh state machine must accept any frame, got min=%u",
          plan.min_frame);
    coop_seq_verdict(&seq, &plan, true, 10, 0, &v);
    CHECK(v.accept, "the anchoring pane must be accepted whatever its number");
    CHECK(seq.peer_next == 11, "anchor should now expect 11, got %u", seq.peer_next);

    coop_seq_plan(&seq, false, &plan);
    CHECK(plan.min_frame == 11, "min_frame should be 11, got %u", plan.min_frame);
    coop_seq_verdict(&seq, &plan, true, 14, 0, &v);       /* a hole: 11,12,13 missing */
    CHECK(v.accept, "frame 14 after 10 must be SPLICED -- refusing freezes the anchor");
    CHECK(seq.peergap == 1, "peergap should be 1, got %u", seq.peergap);
    CHECK(v.request_idr_keep, "a gap must ask for a keyframe with the KEEP variant");
    CHECK(v.drop_lastpeer, "the retained peer AU is on the far side of the hole");
    CHECK(seq.peer_next == 15, "the gap pane must become the new anchor, got %u",
          seq.peer_next);
}

static void test_a_hole_never_freezes_the_anchor(void) {
    current_test = "one hole must not freeze the anchor for the session";
    /* THE MEASURED FAILURE, 2026-08-01, one 78-second two-host run: a single missed pane at
     * frame 16 left `expected 16` in every gap line for the rest of the session while the
     * peer ran on to 2905. peergap=401, paired=12, fallback=1788, boot=0 -- and each refusal
     * asked PC2 for a keyframe, 550 of them, which drove it to near-all-intra and cut the
     * pane feed to 5 access units a second. Nothing recovered it: recovery needed a paired
     * IRAP, and after the first composite nothing ever asks our own host for one.
     *
     * Replay it: consecutive panes after a single hole must all be accepted. */
    coop_seq_t seq;
    coop_seq_reset(&seq);
    coop_seq_plan_t plan;
    coop_seq_verdict_t v;

    coop_seq_plan(&seq, false, &plan);
    coop_seq_verdict(&seq, &plan, true, 15, 0, &v);       /* anchor at 15 -> expect 16 */
    CHECK(v.accept, "anchor pane refused");

    unsigned accepted = 0;
    for (uint32_t f = 17; f <= 2905; ++f) {              /* 16 was lost; the rest arrive */
        coop_seq_plan(&seq, false, &plan);
        coop_seq_verdict(&seq, &plan, true, f, 0, &v);
        if (v.accept) { accepted++; }
    }
    CHECK(accepted == 2889, "only %u of 2889 panes after one hole were accepted -- the "
          "anchor froze at %u", accepted, seq.peer_next);
    CHECK(seq.peergap == 1, "one hole must count ONCE, got peergap=%u", seq.peergap);
}

static void test_peer_renumbering_releases_the_anchor(void) {
    current_test = "a peer that restarts its frame numbering releases the anchor";
    /* The peer's session can drop and reconnect, and its frame counter then restarts at 0.
     * take_next discards every pane below the anchor, so the caller sees "no pane" with a
     * non-zero skipped count, forever. Detect that and drop the anchor. */
    coop_seq_t seq;
    coop_seq_reset(&seq);
    coop_seq_plan_t plan;
    coop_seq_verdict_t v;

    coop_seq_plan(&seq, false, &plan);
    coop_seq_verdict(&seq, &plan, true, 900, 0, &v);
    CHECK(seq.peer_next == 901, "anchor should be 901, got %u", seq.peer_next);

    /* The peer reconnected: panes numbered 0.. are all discarded as stale. */
    coop_seq_plan(&seq, false, &plan);
    coop_seq_verdict(&seq, &plan, false, 0, 3, &v);
    CHECK(seq.peer_next == 0, "a queue emptied entirely by staleness must clear the anchor");

    coop_seq_plan(&seq, false, &plan);
    CHECK(plan.min_frame == 0, "min_frame must be 0 after the release, got %u", plan.min_frame);
    coop_seq_verdict(&seq, &plan, true, 4, 0, &v);
    CHECK(v.accept, "the restarted peer's first pane must be accepted");
    CHECK(seq.peer_next == 5, "re-anchor should expect 5, got %u", seq.peer_next);
}

static void test_stale_panes_are_dropped_not_refused(void) {
    current_test = "stale panes are dropped, not treated as holes";
    coop_seq_t seq;
    coop_seq_reset(&seq);
    coop_seq_plan_t plan;
    coop_seq_verdict_t v;
    coop_seq_plan(&seq, false, &plan);
    coop_seq_verdict(&seq, &plan, true, 40, 0, &v);      /* anchor at 40 */
    CHECK(v.accept, "anchor pane refused");

    /* take_next discarded two older panes on its way to the one we expected: that is normal
     * catch-up, not a hole -- the pane we got is exactly the one predicted from. */
    coop_seq_plan(&seq, false, &plan);
    coop_seq_verdict(&seq, &plan, true, 41, 2, &v);
    CHECK(v.accept, "the expected pane must be accepted even when stale ones preceded it");
    CHECK(seq.stale_dropped == 2, "stale_dropped should be 2, got %u", seq.stale_dropped);
    CHECK(seq.peergap == 0, "stale panes must not count as gaps (peergap=%u)", seq.peergap);
}

static void test_idr_clearing_is_once_per_bootstrap(void) {
    current_test = "the clearing IDR request fires once per bootstrap";
    /* Clearing on EVERY request throws away each freshly arrived IDR shortly before our next
     * IRAP wants it: measured boot=0 for 721 pictures. */
    coop_seq_t seq;
    coop_seq_reset(&seq);
    coop_seq_plan_t plan;
    coop_seq_verdict_t v;

    coop_seq_plan(&seq, true, &plan);
    CHECK(plan.kind == COOP_SEQ_TAKE_IDR, "our IRAP must pair with the peer's IDR");
    CHECK(plan.request_idr_clearing, "the first IRAP of a bootstrap must clear the stale IDR");
    coop_seq_verdict(&seq, &plan, false, 0, 0, &v);
    CHECK(!v.accept && v.request_idr_keep, "no IDR yet: wait with the KEEP variant");
    CHECK(seq.idr_unpaired == 1, "idr_unpaired should be 1, got %u", seq.idr_unpaired);

    for (unsigned i = 0; i < 20; ++i) {
        coop_seq_plan(&seq, true, &plan);
        CHECK(!plan.request_idr_clearing,
              "IRAP #%u of the same bootstrap cleared the IDR again", i + 2);
        coop_seq_verdict(&seq, &plan, false, 0, 0, &v);
    }

    /* Pair, then a forced refresh: the NEXT bootstrap re-arms the clearing request, because
     * the IDR held at that point is stale by definition. */
    coop_seq_plan(&seq, true, &plan);
    coop_seq_verdict(&seq, &plan, true, 500, 0, &v);
    CHECK(v.accept, "a present sticky IDR must pair with our IRAP");
    CHECK(coop_seq_on_paired(&seq, true), "a paired IRAP must flush the pane queue");
    CHECK(seq.bootstrapped, "a paired IRAP is the bootstrap");
    CHECK(seq.peer_next == 0, "bootstrap must leave the anchor unset, got %u", seq.peer_next);
    coop_seq_force_refresh(&seq);
    CHECK(!seq.bootstrapped, "a forced refresh must drop back to un-bootstrapped");
    coop_seq_plan(&seq, true, &plan);
    CHECK(plan.request_idr_clearing, "the next bootstrap must re-arm the clearing request");
}

static void test_superseded_sticky_idr_is_not_paired(void) {
    current_test = "a sticky IDR the composite has moved past is superseded";
    /* 2026-10-04 (VEGA primary, PC2 peer, vertical): the peer's IDR burst ended at its frame
     * 280, which the normal take converted and spliced; the panes after it kept coming for 36 s;
     * then OUR host IDR'd (its game going fullscreen). The sticky slot still held 280, and
     * pairing with it put a 36 s old picture under the peer's current inter panes. */
    coop_seq_t seq;
    coop_seq_reset(&seq);
    coop_seq_plan_t plan;
    coop_seq_verdict_t v;
    CHECK(!coop_seq_idr_superseded(&seq, 280), "unanchored: there is no evidence it is stale");

    for (uint32_t f = 270; f <= 2600; ++f) {           /* 280 among them, taken as a pane */
        coop_seq_plan(&seq, false, &plan);
        coop_seq_verdict(&seq, &plan, true, f, 0, &v);
    }
    CHECK(coop_seq_idr_superseded(&seq, 280), "IDR 280 after accepting 2600 must be superseded");
    CHECK(coop_seq_idr_superseded(&seq, 2599), "one behind the tile is still behind it");
    CHECK(!coop_seq_idr_superseded(&seq, 2600), "the tile's current picture is not superseded");
    CHECK(!coop_seq_idr_superseded(&seq, 2614), "an IDR still ahead in the queue is fresh");

    /* The rule that ran: plan/verdict never look at the sticky IDR's age, so the same trace
     * pairs with 280 -- this is what the caller now has to refuse. */
    coop_seq_plan(&seq, true, &plan);
    CHECK(plan.kind == COOP_SEQ_TAKE_IDR, "our IRAP must still plan to take the sticky IDR");
    coop_seq_verdict(&seq, &plan, true, 0, 0, &v);
    CHECK(v.accept && !v.request_idr_keep,
          "pre-fix: a stale sticky IDR is accepted and no keyframe is requested");

    /* A real pairing re-anchors, and a peer that renumbered releases the anchor: neither may
     * leave a fresh IDR judged stale against an old count. */
    (void) coop_seq_on_paired(&seq, true);
    CHECK(!coop_seq_idr_superseded(&seq, 5), "after a paired IRAP there is no anchor yet");
    coop_seq_plan(&seq, false, &plan);
    coop_seq_verdict(&seq, &plan, true, 3000, 0, &v);
    coop_seq_plan(&seq, false, &plan);
    coop_seq_verdict(&seq, &plan, false, 0, 4, &v);    /* queue emptied as stale: renumbered */
    CHECK(!coop_seq_idr_superseded(&seq, 2), "a renumbered peer's new IDR must not be stale");
}

static void test_peer_only_restart_is_taken_in_order(void) {
    current_test = "a peer-only encoder restart IDR is taken as the next pane";
    /* 2026-10-04 09:05-09:06 (Halo on PC2, VEGA primary static, no IRAP of ours for minutes):
     * PC2's encoder was torn down and reopened, emitting IDR 4285 between inter panes. The
     * suspicion was that it waits in the sticky slot for an IRAP of OURS. It cannot: the sticky
     * slot is read only when OUR picture is an IRAP; an inter picture of ours plans TAKE_NEXT,
     * which hands over the queued IDR in order like any pane (the caller then folds it into a
     * TRAIL picture). Only a numbering gap asks for another keyframe. */
    coop_seq_t seq;
    coop_seq_reset(&seq);
    coop_seq_plan_t plan;
    coop_seq_verdict_t v;
    for (uint32_t f = 4200; f <= 4284; ++f) {
        coop_seq_plan(&seq, false, &plan);
        coop_seq_verdict(&seq, &plan, true, f, 0, &v);
    }
    coop_seq_plan(&seq, false, &plan);
    CHECK(plan.kind == COOP_SEQ_TAKE_NEXT, "an inter picture of ours must not wait on the sticky IDR");
    CHECK(plan.min_frame == 4285, "the restart IDR is the expected next pane, min=%u", plan.min_frame);
    coop_seq_verdict(&seq, &plan, true, 4285, 0, &v);
    CHECK(v.accept && !v.drop_lastpeer && !v.request_idr_keep,
          "a contiguous restart IDR must be spliced at once, with no hole and no request");
    for (uint32_t f = 4286; f <= 6315; ++f) {
        coop_seq_plan(&seq, false, &plan);
        coop_seq_verdict(&seq, &plan, true, f, 0, &v);
        if (!v.accept || v.request_idr_keep) { break; }
    }
    CHECK(seq.peer_next == 6316 && seq.peergap == 0,
          "the restarted chain must run unbroken (next=%u, peergap=%u)", seq.peer_next, seq.peergap);

    /* The same restart with frames lost in the teardown: still spliced at once, and the gap
     * asks for a keyframe -- the panes before it are on the far side of a hole. */
    coop_seq_reset(&seq);
    coop_seq_plan(&seq, false, &plan);
    coop_seq_verdict(&seq, &plan, true, 4250, 0, &v);
    coop_seq_plan(&seq, false, &plan);
    coop_seq_verdict(&seq, &plan, true, 4285, 0, &v);
    CHECK(v.accept && v.request_idr_keep && v.drop_lastpeer,
          "a restart IDR after a numbering gap is spliced and the gap is reported");
}

static void test_inter_pairing_never_bootstraps(void) {
    current_test = "an inter pairing is not a bootstrap";
    /* 175 composed pictures, IRAP count ZERO, first picture TRAIL_R at POC 5: what happens
     * when any successful splice counts as bootstrap. */
    coop_seq_t seq;
    coop_seq_reset(&seq);
    CHECK(!coop_seq_on_paired(&seq, false), "an inter pairing must not ask for a flush");
    CHECK(!seq.bootstrapped, "an inter pairing must not bootstrap");
}

static void test_session_reset(void) {
    current_test = "per-session reset";
    coop_seq_t seq;
    coop_seq_reset(&seq);
    coop_seq_plan_t plan;
    coop_seq_verdict_t v;
    coop_seq_plan(&seq, false, &plan);
    coop_seq_verdict(&seq, &plan, true, 900, 3, &v);
    (void) coop_seq_on_paired(&seq, true);
    seq.bootstrapped = true;
    coop_seq_reset(&seq);
    /* A stale anchor would refuse the next session's first panes; a stale `bootstrapped`
     * would let our own 1088-row parameter sets reach the decoder. */
    CHECK(seq.peer_next == 0 && !seq.bootstrapped && !seq.idr_cleared &&
          seq.peergap == 0 && seq.idr_unpaired == 0 && seq.stale_dropped == 0,
          "reset left state behind");
}

int main(void) {
    test_sequencing_holds_across_timings();
    test_queue_depth_sets_the_jitter_the_pane_survives();
    test_a_hole_is_reported_and_re_anchored();
    test_a_hole_never_freezes_the_anchor();
    test_peer_renumbering_releases_the_anchor();
    test_stale_panes_are_dropped_not_refused();
    test_idr_clearing_is_once_per_bootstrap();
    test_superseded_sticky_idr_is_not_paired();
    test_peer_only_restart_is_taken_in_order();
    test_inter_pairing_never_bootstraps();
    test_session_reset();
    if (failures > 0) {
        printf("FAIL: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS: coop_seq\n");
    return 0;
}

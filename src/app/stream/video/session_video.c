#include "../avsync_probe.h"
#include "util/diag_logs.h"
#include "session_video.h"

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
/* NETCAP (continuous composite capture) needs sockets + its own thread. Kept here rather than
 * inside the webOS-only block: nothing below is platform specific. */
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "stream/session.h"

#include "app_settings.h"
#include "sps_parser.h"

#include "ui/streaming/streaming.controller.h"
#include "util/bus.h"
#include "logging.h"
#include "ss4s.h"
#include "stream/connection/session_connection.h"
#include "stream/session_priv.h"
#include "app.h"
#include "app_launch.h"
#include "coop_splice.h"
#include "coop_conceal.h"
#include "coop_poc.h"
#include "platform/webos/vendor_diag.h"
#include "coop_coord.h"
#include "coop_seq.h"
#include "coop_trace.h"
#include "coop_secondary.h"
#include "coop_session.h"
#include "util/path.h"

#include <SDL.h>
#include <assert.h>
#include <time.h>

// Starting capacity for the decode-unit reassembly buffer. Grows on
// demand to accommodate larger frames (e.g. 4K IDR frames at high
// bitrate routinely exceed 2 MB), capped to keep a malformed stream
// from exhausting memory.
#define DECODER_BUFFER_MAX_SIZE (32 * 1024 * 1024)

/** Slices hint for pipeline decode while later slices still arrive (high bitrate / 120 Hz). */
/* Slices per frame requested from the host. Co-op split-screen needs exactly 2 so
 * each pane is one slice of one picture, which is what makes two streams
 * spliceable into a single decode (docs/coop-split-DESIGN.md). Overridable at
 * runtime via /tmp/coop-slices for testing without a rebuild. */
#define VDEC_STREAM_SLICES_PER_FRAME 4

/*
 * Throttle IDR requests: a large IDR frame (~4-8x a P-frame) refills the SMP appsrc
 * buffer immediately after a BufferFull signal, triggering another BufferFull → another
 * IDR request — a runaway feedback loop that stalls the stream permanently. Dropping
 * the request silently lets the pipeline drain and self-heal within a few P-frames.
 * 1000 ms also keeps the control channel (shared with gamepad input) clear of floods.
 */
#define IDR_REQUEST_MIN_INTERVAL_MS 1000u

static int vdec_stream_target_fps = 60;

/* Window after stream setup in which feed errors are tolerated while a backend
 * pipeline finishes coming up (see the feed error path). */
#define VDEC_FEED_GRACE_MS 3000u
/* Decode budget for the composite, in DELIVERED Mbit/s (what we actually feed the decoder).
 *
 * IT IS BITRATE, NOT PIXEL RATE. Measured 2026-09-09, latency read off the pattern's det-code
 * clock, three points on the same rig:
 *     3840x2176 @120 static desktop  5.3 Mbps  (1002 Mpixel/s)  -> latency LOW
 *     3840x2176 @120 test pattern   29.5 Mbps  (1002 Mpixel/s)  -> latency 16.5 SECONDS
 *     3840x2176 @ 60 test pattern   19.9 Mbps  ( 501 Mpixel/s)  -> latency  1.9 s
 * The first two have IDENTICAL pixel rate and opposite outcomes, so a geometry x fps budget is
 * the WRONG variable and would warn on a perfectly healthy static desktop. Delivered bitrate
 * separates all three, and it is content-sensitive, which is what the fault actually tracks.
 *
 * 24 is a BRACKETED ESTIMATE FROM n=3 -- the boundary is somewhere in 20..29 Mbps at this
 * geometry and is probably resolution-dependent. Narrow it with a sweep before trusting it. */
#define COOP_DECODE_BUDGET_MBPS 24.0
static unsigned long vdec_setup_ms = 0;
static bool coop_solo_active = false;   /* latched at setup, never per-frame */

static FILE *diag_log = NULL;

/* Frame-arrival jitter measurement (gated by /tmp/coop-measure).
 *
 * The splice locks both panes to ONE presentation time, so pairing only works
 * if two independent sessions deliver frames within a fraction of a frame
 * interval. A single session's inter-arrival spread bounds that: if arrivals
 * are tight, two sessions pair comfortably inside 16.7ms and skip-slices are a
 * rare safety net; if they are loose, merging independent sources will judder
 * however clever the encoder work is -- worth knowing BEFORE writing a HEVC
 * slice writer. */
static bool coop_spawned = false;
static bool coop_live = false;             /* real two-source splice */
static unsigned char *coop_pane_buf = NULL;
static size_t coop_pane_cap = 0;
static unsigned coop_live_paired = 0, coop_live_fallback = 0;

/* HAS THE COMPOSITE ACTUALLY PUT A PICTURE ON SCREEN YET?
 *
 * The UI closes its "Connecting..." dialog on USER_STREAM_OPEN, which fires when the STREAM
 * opens -- but in co-op that is not when the PICTURE starts. Horizontal suppresses its own
 * pictures until it can splice ("suppressing picture #N before the FIRST composite"), so
 * between those two moments the panel has nothing to show and the user gets a blank black
 * screen with no indication anything is happening. Vertical hides it by bootstrap-MIRRORING
 * the primary into both tiles (mirrored=291 on a live session) where horizontal has
 * mirrored=0, which is why the startup oddity is horizontal-only.
 *
 * `coop_live_paired` already counts emitted composites, so "started" needs no new state --
 * only a way for the UI thread to ask. Reading one unsigned that the video thread only ever
 * increments is safe without a lock: a stale read costs one more poll interval. */
/* Set ONLY where a real peer slice is spliced in -- not where one is concealed. */
static bool coop_first_real_splice = false;
bool coop_composite_started(void) { return coop_first_real_splice; }
static bool coop_splice_test = false;
static unsigned char *coop_splice_buf = NULL;
static unsigned char coop_hh_nonvcl[8192];
static size_t coop_hh_nonvcl_len = 0;
static coop_poc_params_t coop_hh_params;
static bool coop_hh_ready = false;
static unsigned coop_hh_addr = 0;
/* HALF-HEIGHT CONCEALMENT STATE. The peer delivers fewer pictures than we do (measured
 * 7117 against 10800 over one run), and a composite picture with no peer slice used to go
 * out with its bottom half UNCODED. Keeping the last peer access unit lets every picture
 * be whole. `bootstrapped` gates the other half of the same fix: until one complete
 * composite exists, submitting our own AU would hand the decoder a 1088-row SPS and fix
 * the video plane at half height for the rest of the session. */
static unsigned char coop_hh_lastpeer[4u * 1024u * 1024u];
static size_t coop_hh_lastpeer_len = 0;
/* OUR most recent INTER slice, kept only as a HEADER DONOR.
 *
 * A concealment slice is built from a slice header of ours, and coop_build_skip_slice()
 * returns 0 for an I-slice because I slices have no skip syntax. So on the one picture that
 * needs concealment most -- our own unpaired IRAP -- the only header to hand is the wrong
 * type. This is the P-type header to use instead; its payload is discarded and its POC is
 * rewritten to the picture it joins. */
static unsigned char coop_hh_lastmine[4u * 1024u * 1024u];
static size_t coop_hh_lastmine_len = 0;
/* THE PEER'S PREDICTION CHAIN IS BROKEN AND ONLY ITS NEXT KEYFRAME CAN MEND IT.
 *
 * A gap means we dropped one of the peer's panes, so every later inter slice of its predicts
 * from a picture the composite never held. Splicing across that hole was the right call while
 * concealment did not work -- a frozen pane beat a permanently stuck one -- but it decodes as
 * pale blocks (an absent reference is synthesised flat grey with residual painted on top)
 * which then ACCUMULATE around moving edges until something re-intras the pane. Measured
 * 2026-08-02: peergap=127 over 46796 pictures at ~60fps is one every ~6s, matching the user's
 * "1-2 seconds at the shortest, 14 at the longest" exactly.
 *
 * Now that the all-skip slice is bit-exact, the better trade is to HOLD the pane frozen from
 * the gap until the peer's next IDR arrives and re-intras it. That turns a spreading
 * corruption into a brief freeze. The IDR is already requested on every gap and arrives at
 * the same rate the gaps do (converted=124 against peergap=127). */
static bool coop_hh_chain_broken = false;
/* PORTED FROM THE TILE PATH, WHICH IS THE ONE THAT WORKS. Horizontal carried only the
 * COMPOSITE's parameter set and used it to parse both hosts' slice headers. That is survivable
 * only because a pane's slice is `first_slice_segment_in_pic_flag = 1` and therefore carries no
 * address field, so the one geometry-dependent width never gets read -- i.e. it was correct by
 * accident, not by construction. Vertical keeps all three (`coop_vt_src`, `_peer_params`,
 * `_comp`) and walks each host's slices with the parameters that host actually encoded against;
 * horizontal now does the same, so the two paths differ only where the GEOMETRY differs. */
static coop_poc_params_t coop_hh_src;          /* OUR pane's own SPS/PPS */
static coop_poc_params_t coop_hh_peer_params;  /* the PEER pane's, or ours if it will not parse */
/* THE COMPOSITE MUST OWN ITS PICTURE ORDER COUNT, and this is the keystone of the rebuild.
 * Riding the primary's POC leaves NO ROOM to insert a picture -- consecutive primary POCs
 * differ by 1 -- which is the entire reason `catchup=0` on every horizontal run ever measured
 * while the tile path absorbs its peer's surplus happily. Numbering here, and rewriting BOTH
 * halves to it, is what makes a catch-up picture expressible at all. */
static unsigned coop_hh_poc = 0;
/* The POC issued to the picture currently being built, 0 if none. Lets the feed gate return an
 * unused number instead of leaving a gap in the emitted sequence. MEASURED 2026-08-05: with
 * tiles armed the composite emitted POCs 1,2,5,9,10,11 -- and ffmpeg answered "Could not find
 * ref with POC 4" then "POC 8", i.e. exactly the numbers that were consumed by pictures the
 * tiles gate then dropped. Every later slice's short-term RPS is delta-coded against a
 * sequence with holes in it, so the decoder cannot build a reference list and the peer's tile
 * predicts from whatever it has -- which is the progressive magenta decay on a static scene. */
static unsigned coop_hh_poc_pending = 0;
static unsigned coop_hh_poc_returned = 0;
static bool coop_hh_poc_init = false;
static unsigned coop_hh_last_evicted = 0, coop_hh_mended = 0;
/* Our own retimed slice. Its own buffer: coop_conv_buf is in use for the peer's IDR conversion
 * in the same picture, and coop_poc_buf for the peer's retime. */
static unsigned char coop_hh_mine_buf[4u * 1024u * 1024u];
static unsigned coop_hh_poc_fail = 0;
/* HORIZONTAL AS TWO TILE ROWS, armed with /tmp/coop-hh-tiles. Latched at stream setup like every
 * other per-session choice, because the composite's parameter sets are built once. The point is
 * not the composition -- the half-height splice already worked -- but to put horizontal on
 * coop_slice_to_tile(), the address path the VERTICAL mode uses and which is demonstrably
 * correct, instead of the bespoke one that produced the catch-up readdress bug and the
 * concealer's inexactness. */
static bool coop_hh_tiles = false;
/* Did THIS picture actually get composed by the tiled path? In tiles mode nothing else may be
 * fed: an untiled escapee still carries OUR PANE's own SPS (34 CTB rows), which re-initialises
 * the decoder away from the 68-row composite, and a decoder flip-flopping between two SPSs
 * reports `First slice in a frame missing` on EVERYTHING and renders black. That is what the
 * first two live tiles runs did, while the primitives themselves decode cleanly offline
 * (tests/coop/test_tile_compose.c). Gate the feed rather than trust every branch. */
static bool coop_hh_tiled_ok = false;
#if TARGET_ANDROID
/* PADDING BEFORE THE SECOND SLICE. The hosts end each picture's slice data with cabac_zero_words, and a
 * composite puts one host's last slice FIRST, so the padding then precedes the other host's slice. The
 * SHIELD TV's decoder does not decode a slice after such padding: that pane is flat green, in both co-op
 * layouts, with no decoder error (measured 2026-10-16; FFmpeg and webOS skip the padding). On Android every
 * composite goes to the decoder through coop_au_strip_zero_words(), which removes only padding that has a
 * slice after it -- the pictures are unchanged. Reset with the session. */
static unsigned coop_zw_aus = 0, coop_zw_words = 0;
#endif
static unsigned coop_hh_untiled_blocked = 0;
/* THE PEER'S PANE AS RECEIVED, gated on /tmp/coop-dump-peer. The primary's own pane is already
 * capturable (/tmp/coop-dump) but the PEER's never was, and without it no offline harness can
 * reproduce the real composite -- it can only mirror one pane into both halves, which is exactly
 * the gap that has left the tiled path unexplained: the harness DECODES while the live composite
 * is black, and the only untested difference is that live pairs two DIFFERENT panes. */
static FILE *coop_peer_dump = NULL;
static unsigned coop_peer_dump_n = 0;
/* WHY CATCH-UP REFUSED. `catchup=0` has been true on every horizontal run ever measured, and a
 * single counter cannot say whether the gate never opened, the sequencer declined, the queue
 * had nothing to give, or the builder refused. Each of those wants a DIFFERENT fix, so tally
 * them separately -- otherwise the next live run costs a whole cycle to learn one bit. */
static unsigned coop_hh_cu_gate = 0;    /* gate reached, backlog present */
static unsigned coop_hh_cu_qmax = 0;    /* deepest queue actually seen at the gate */
static unsigned coop_hh_cu_plan = 0;    /* sequencer did not say TAKE_NEXT */
static unsigned coop_hh_cu_take = 0;    /* take_next gave nothing, or the verdict refused */
static unsigned coop_hh_cu_build = 0;   /* coop_hh_build_catchup() refused */
/* WHICH refusal inside the builder: 1 args, 2 nonvcl, 3 pane parse, 4 peer IRAP, 5 skip slice,
 * 6 top would overflow, 7 readdress, 8 bottom would overflow. */
static unsigned coop_hh_cu_why[9] = {0};
/* Per-picture peer provenance, purely for the /tmp/coop-dump-out correlation (see [COOP-AU]). */
static uint32_t coop_hh_dbg_frame = 0;
static unsigned coop_hh_dbg_dropped = 0, coop_hh_dbg_gap = 0;
/* RUNNING TOTALS FOR PEER FRAME SKIPS. The per-picture values above are overwritten every
 * picture and were only ever surfaced by the `[COOP-AU]` line, which is gated on
 * `coop_dump_out_open && coop_dump_out_frames < 400` -- the legacy 400-AU dump path that netcap
 * replaced precisely because ~6.7 s cannot reach a fault developing over minutes. Measured
 * 2026-09-02: that line emitted ZERO times across two 700-second runs, so the signal existed and
 * was unreachable. It matters -- offline analysis of the netcap index put peer frame-skips at
 * 2.25% of access units INSIDE corruption bursts against 0.30% at baseline, a 7.5x separation,
 * and it was the strongest in-burst signal found. These accumulate for the life of the stream
 * and ride the [COOP-AVC] line, so a live run can show it without a capture. */
static unsigned coop_hh_skip_events = 0;   /* pictures arriving with a peer-frame gap */
static unsigned coop_hh_skip_frames = 0;   /* peer frames skipped in total */
static unsigned coop_hh_skip_max = 0;      /* worst single gap */
static bool coop_hh_dbg_accept = false;
/* JITTER BUFFER PRIMING. With the two hosts paced to the same rate the queue hovers at 0-1
 * panes, because we consume one per composed picture the instant one exists. Any momentary
 * lateness then STARVES us and the pane is held -- measured 8% of pictures even with
 * delivery running 1:1 (forwarded 5365 vs recv 5364) and the peer steady at exactly our
 * rate. The queue is 12 deep but never gets the chance to fill.
 *
 * Priming once, at the start of composition, gives jitter somewhere to absorb: hold until a
 * couple of panes are banked, then consume one per picture as usual and the buffer persists
 * for as long as the rates track. Costs COOP_HH_PRIME frames of pane latency once
 * (~50ms at 40fps) and nothing thereafter -- against a visible stutter every twelfth
 * picture, which is the trade [[latency-minimization]] would make. */
#define COOP_HH_PRIME 2u
static bool coop_hh_primed = false;
static unsigned coop_hh_repeated = 0;
static unsigned coop_hh_typemismatch = 0;
static unsigned coop_hh_suppressed = 0;
static unsigned coop_hh_retyped = 0;
static unsigned coop_hh_repeat_mark = 0;
static unsigned coop_hh_refreshes = 0;
static unsigned coop_hh_reparam = 0;
static unsigned coop_hh_skipconceal = 0;
/* CONCEALMENT IS THE DAMAGE, AND THE TILE PATH NEVER PAID FOR IT.
 *
 * A concealed picture holds the peer's tile bit-exact, which is correct on its own -- but the
 * peer does not hold. It keeps encoding, predicting each frame from ITS previous one, so the
 * slice after a conceal references a picture the decoder never received. The error is small
 * per event and CUMULATIVE, and it lands only where residual is large: static regions survive
 * and moving detail shreds. The slice path already knew this (COOP_CONCEAL_BUDGET, the
 * re-bootstrap at the bottom of this file, and its own comment "nothing ever cleared it, which
 * is the stream degraded over time the user reports") -- the horizontal TILE path only ever
 * incremented a counter. It has no budget and never re-anchors.
 *
 * Measured on a live two-host horizontal session 2026-08-15: skipconceal +99 per 600 pictures
 * (16.5%), climbing steadily, with mended=2 and every eviction counter static -- i.e. no chain
 * BREAKS at all, just conceals quietly accumulating. On the panel: a band of bright streaks
 * over an ANIMATED wallpaper, exactly where the motion is.
 *
 * Same budget as the slice path deliberately: 300 is a measured value there and this is the
 * same quantity counted the same way. Re-anchoring reuses the proven recovery -- mark the
 * chain broken, which requests a keyframe (throttled) and holds the tile until an IRAP lands.
 * One keyframe per budget, not the constant IDR traffic that made gop30 flash.
 *
 * ^^^ THE BUDGET IS RETRACTED AND ITS TRIGGER REMOVED 2026-08-16 (:3787). Everything above is
 * kept so nobody re-derives it. THREE of its claims are false:
 *
 * 1. "the slice after a conceal references a picture the decoder never received" -- it does
 *    receive it. The skip conceal is spliced INTO the composite and is BIT-EXACT (measured
 *    2026-08-04: concealed[n] vs control[n-1] = inf), so the peer's next slice references
 *    POC-1 and gets precisely the pixels its encoder held. Nothing accumulates. That error
 *    model belongs to the REPEAT concealer, which this path does not use: repeated=0
 *    skipconceal=16821 over 35,998 composites, 2026-08-16.
 * 2. "One keyframe per budget" -- no. chain_broken LATCHES and :3795 re-requests every picture
 *    until an IRAP arrives. Measured over 600 composites: canchor=1 produced idrreq=30/358
 *    (388 demands) and converted=9. One expiry, nine whole-tile repaints.
 * 3. "not the constant IDR traffic that made gop30 flash" -- it is exactly that, at 1/7th the
 *    rate. Each forced IDR repaints the peer tile at a coarse QP: one soft frame refined back
 *    to sharp = the one-frame whole-pane flicker, ~every 2s. Identical to the fault vertical
 *    measured at 3.5/s on 2026-08-03 and fixed by 0bd183a4.
 *
 * The 2026-08-15 streak observation at line 244 was REAL and is NOT explained away by this --
 * but the budget did not fix it either (peer-pane streaks stayed open through every run since),
 * and it cost the flicker. Streaks remain a separate open item; chase them with a mechanism
 * that does not fire on healthy holds.
 *
 * Both counters are KEPT so the removal is checkable: `canchor` must stay 0 while
 * `skipconceal` climbs. COOP_HH_CONCEAL_BUDGET is now unreferenced -- do not re-wire it. */
static unsigned coop_hh_conceal_since_anchor = 0;
#define COOP_HH_CONCEAL_BUDGET 300  /* RETIRED 2026-08-16 -- unreferenced, see above */
static unsigned coop_hh_conceal_anchors = 0;   /* MUST STAY 0: the budget trigger is removed */

/* --- DIVERGENCE DETECTORS (detect-only, 2026-08-16) ---------------------------------------
 *
 * WHAT THEY ARE FOR. Removing the conceal budget removed the only thing that periodically
 * re-intra'd the peer tile, and the peer runs an infinite GOP (gop_length=0,
 * idr_refresh_interval_sec=0), so an error that enters by a route the depacketizer cannot see
 * now persists for the rest of the session. Observed 2026-08-16: the bottom tile corrupted
 * across most-but-not-all of the pane and stayed that way, with every existing counter flat
 * (canchor=0 held+0 peergap+0 repeated=0, no eviction, secondary dropped=0).
 *
 * The design we want is detect-then-reset. The reset already exists and is proven
 * (coop_hh_chain_broken: throttled peer keyframe, tile held until an IRAP lands, mend on the
 * IRAP). The missing half is a signal that is TRUE when the tile is wrong, so these two are
 * wired to counters and a log line ONLY. Nothing acts on them yet: a detector that fires
 * wrongly would re-create exactly the storm the budget caused, so it earns its trigger by
 * being measured against a real corruption first.
 *
 *  refbad   -- the peer's slice named a reference the composite cannot honour. Only delta -1
 *              is safe (see coop_slice_refcheck). This one names the CAUSE, not just damage.
 *  ppocjump -- the peer's OWN coded POC stepped by something other than its established step.
 *              Distinct from `peergap`, which watches moonlight's frameNumber = Apollo's
 *              frame_nr (incremented per encode() call). The coded POC comes from NVENC's
 *              picture ordering. The two CAN diverge, and if they do, the pane skipped a
 *              picture while frameNumber stayed continuous -- invisible to everything today.
 */
static unsigned coop_hh_refbad = 0;
static unsigned coop_hh_ppocjump = 0;
static unsigned coop_hh_ppoc_last = 0;
static unsigned coop_hh_ppoc_step = 0;         /* 0 = not yet learned */
static bool coop_hh_ppoc_have = false;
static unsigned coop_hh_irapconceal = 0;
static unsigned coop_hh_heldbroken = 0;
/* ON BY DEFAULT since 2026-08-03. It was written OFF because the synthesised all-skip slice
 * had only offline numbers behind it (45.4 dB vs 29.2 dB for a repeat) and the rule here is
 * that the TV decides. The TV has now decided, twice: arming it took `repeated` 284 -> 0 and
 * `fallback` 253 -> 9, the peer pane's horizontal smearing disappeared, and the microtext went
 * from a blocky mess to legible across every DETAIL line.
 *
 * WHY IT IS A DEFAULT AND NOT A FLAG ANY MORE. It spent a session as `/tmp/coop-skipconceal`,
 * armed by hand, and the TV's /tmp does not survive indefinitely -- so the fix silently
 * un-applied itself and the peer column went back to the repeat floor, where a repeated inter
 * slice replays its residual and smears whatever is MOVING while static areas stay clean. That
 * cost a session to rediscover from a photograph of grass. A fix that has to be remembered is
 * not a fix.
 *
 * `/tmp/coop-noskipconceal` turns it off, so the A/B is still available in the direction that
 * now matters -- proving the default is the better one. */
static bool coop_skipconceal_enabled = true;
/* Horizontal self-mirror: our own live pane into BOTH halves, so the horizontal composite is
 * testable with ONE host. The vertical counterpart is /tmp/coop-vmirror. Exists so a
 * single-host horizontal test never has to fall back to replaying a recording. */
static bool coop_hmirror_enabled = false;
static unsigned coop_hh_mirrored = 0;
static unsigned char coop_skip_scratch[64u * 1024u];
/* Which of the peer's pictures may pair with ours, and what to ask for when none may. Its
 * own module (coop_seq.c) because every defect in it has been an ORDERING defect, and those
 * are only reproducible live with the second PC free -- offline replay of the measured
 * traces is what tests/coop/test_coop_seq.c gives instead. */
static coop_seq_t coop_hh_seq;
/* Vertical uses the SAME sequencer as horizontal -- see the call site for why the
 * hand-rolled version was wrong in four separate ways. */
static coop_seq_t coop_vt_seq;
/* Repeats tolerated before a forced paired-IRAP refresh. At 60fps with a peer running
 * ~25% slow that is roughly one refresh every 8 seconds -- often enough that decay never
 * becomes visible, rare enough that the brief hold while both hosts IDR is not a tic. */
/* Repeats tolerated before the peer is asked for a keyframe. 120 was ~3s of visible decay
 * at the rate this runs; the request is rate limited to one per 120 ms in coop_coord, so a
 * tighter budget costs nothing when the peer is keeping up (no repeats, no requests) and
 * bounds the decay to well under a second when it is not. */
#define COOP_HH_REPEAT_BUDGET 30u
static unsigned char *coop_poc_buf = NULL;
/* Holds the peer's IDR slice after conversion to a TRAIL_R I-slice. It cannot share
 * coop_poc_buf: the conversion happens BEFORE the POC rewrite and the rewrite reads from
 * this buffer while writing into that one. It cannot be done in place either -- inserting
 * the non-IRAP header fields makes the slice LONGER than the peer's original. */
/* LIVE VERTICAL (TILE) MIRROR. Composes OUR OWN live pane into BOTH tile columns, so the
 * vertical split can be judged on a real desktop at real frame rate with ONE host -- no
 * second machine, and above all NO RECORDING. Replaying a captured file only ever proves
 * the pipeline accepts it: any stutter or flash present in the capture is replayed forever,
 * which is exactly how five bitstream hypotheses were burned chasing an artifact that was
 * baked into the content. Gated on /tmp/coop-vmirror so it cannot affect a normal stream. */
static bool coop_vt_enabled = false, coop_vt_ready = false;
static unsigned char coop_vt_nonvcl[64 << 10];
static size_t coop_vt_nonvcl_len = 0;
static coop_poc_params_t coop_vt_src, coop_vt_comp, coop_vt_peer_params;
static unsigned char *coop_vt_buf = NULL;
static size_t coop_vt_cap = 0;
static unsigned coop_vt_frames = 0;
static unsigned coop_vt_peer_used = 0, coop_vt_peer_missing = 0;
/* THE PEER'S LAST ACCESS UNIT, kept so a gap can HOLD its column instead of filling it with
 * ours. Substituting our pane into tile 1 leaves the peer's later inter slices predicting
 * from a tile the composite never held, which is the intermittent horizontal displacement /
 * overlap seen on the right column -- and it heals only at the peer's next IRAP, which is why
 * it comes and goes. Horizontal learned this in bd2ce96c; the tile path never did. */
static unsigned char *coop_vt_lastpeer = NULL;
static size_t coop_vt_lastpeer_len = 0;
static unsigned coop_vt_repeated = 0, coop_vt_mirrored = 0;
/* NOTHING EVER MENDED THE PEER'S TILE. Horizontal asks for a peer IDR the moment its chain
 * breaks; vertical never called coop_coord_request_peer_idr_keep() at all, and with the stock
 * infinite GOP the peer emits exactly ONE IRAP -- at session start. So every pane lost in the
 * startup burst (measured 2026-08-03: evicted=227 plus 215 bootstrap-mirrored pictures) stayed
 * in the tile FOREVER. On the panel that is a peer column showing edge outlines over black
 * flat areas -- residual with no intra to sit on -- while every counter reads perfect 1:1 and
 * idr_sent stays 0. Ask for a keyframe whenever the chain is known broken, and clear on the
 * peer's next IRAP. */
static bool coop_vt_chain_broken = false;
static unsigned coop_vt_idrreq = 0, coop_vt_mended = 0;
static unsigned coop_vt_last_evicted = 0;
static unsigned coop_vt_slicemap = 0;
/* BIT-EXACT CONCEALMENT FOR THE TILE PATH. Repeating the peer's last inter slice replays its
 * residual against a reference it was not coded for, which is why the artefacts that survived
 * every pacing fix were CLUSTERED ON MOVING CONTENT while static areas stayed clean. An
 * all-skip slice reproduces the previous picture's tile exactly instead. Built from OUR slice
 * header (so POC, RPS and temporal-MVP match the picture, as HEVC requires) over synthesised
 * skip data, then re-homed into tile 1 by the same coop_slice_to_tile the real panes use --
 * both halves are already proven, so this adds no new bitstream writing. */
static unsigned coop_vt_concealed = 0;
static unsigned coop_vt_sizelog = 0;
/* CATCH-UP EMISSION -- see docs/coop-rate-decoupling.md.
 *
 * The composite emits one picture per PRIMARY access unit, so the peer's panes are consumed at
 * the PRIMARY's rate. When the primary slows (a loading screen is the case the user caught)
 * the peer's panes back up in the queue: the backlog IS added latency, and the overflow is
 * dropped, which tears a hole in the peer's prediction chain. The peer's quality is therefore
 * hostage to whatever the primary is doing, which is exactly what must not be true.
 *
 * Fix: when the peer has backlog, emit EXTRA pictures pairing its next pane with a concealed
 * PRIMARY tile -- the mirror of what already happens when the peer is short. The composite
 * rate becomes max(primary, peer), whichever host is behind gets bit-exact concealment, and no
 * pane is ever dropped. Counted so a runaway is visible: this session was full of faults that
 * no counter showed. */
/* 4 -> 6 (2026-08-15). THIS CONSTANT IS THE PEER-TILE DECAY, and the failure is arithmetic.
 *
 * The bound above -- composite <= (1 + COOP_VT_CATCHUP_MAX) x primary -- is not just a floor
 * formula input, it is a hard budget: per primary picture the composite can consume 1 paired
 * pane plus at most COOP_VT_CATCHUP_MAX catch-up panes. Measured on scenario S12 (static
 * primary, peer free-running) with the rig's default 30/30 floors:
 *
 *     peer panes         171/s      (above the 165Hz refresh -- minimum_fps_target pads it)
 *     primary pictures    32/s      (a static primary emits nothing but its floor)
 *     needed per cycle   5.34
 *     budget per cycle   5.00       <- 1 + 4
 *     deficit            0.34/cycle x 32/s = ~11 panes/s, against 14-16/s measured evictions
 *
 * Every evicted pane cuts the peer's reference chain, and the recovery (request an IDR, hold
 * the tile until an IRAP arrives) is a round trip throttled to 120ms against breaks arriving
 * ~138/s. It cannot win, so the tile shreds: crisp at +2s, destroyed by +45s.
 *
 * WHY RAISE THIS RATHER THAN THE PRIMARY'S FLOOR. Both satisfy the same inequality, but the
 * floor raises the LEFT side by manufacturing duplicate frames, and duplicates are latency
 * with no information ([[delivered-rate-is-not-feel]], [[latency-minimization]]) -- the exact
 * cost the 30/30 floors were chosen to avoid. Raising the budget lowers the RIGHT side
 * instead: it costs nothing when there is no backlog, because catch-up only fires on one.
 * It also makes 30/30 structurally VALID rather than 10% under the bound -- PRIMARY_FLOOR_MIN
 * becomes (165+6)/7 = 24.
 *
 * 6, not 5: 5 clears the measured case by 12%, 6 by 31%, and the peer transiently exceeds 171
 * while the primary can dip below 32. Catch-up is bounded by the backlog anyway, so unused
 * headroom is free. It does NOT raise the composite rate in the matched case -- with both
 * hosts near 165 the queue never reaches COOP_VT_CATCHUP_START and this never fires.
 *
 * MUST STAY IN SYNC with COOP_CATCHUP_MAX in scripts/coop/host_encoder.sh, which derives the
 * primary floor minimum from it. */
#define COOP_VT_CATCHUP_MAX 6u
/* LEAVE A RESERVE. Draining to the last pane starves the NORMAL picture that follows: it finds
 * an empty queue and conceals, so the peer's tile alternates between frozen and jumping
 * several frames forward. Measured 2026-08-03 with a `> 1` threshold: catchup=1181 and
 * conceal=1082 in the same 3600 pictures -- the peer reading as both ahead AND short, which is
 * the oscillation, and on screen it is a ghost that movement paints over. Only catch up on a
 * REAL backlog, and stop while a pane is still in hand for the normal picture. */
#define COOP_VT_CATCHUP_START 4u   /* backlog that means "genuinely behind" */
#define COOP_VT_CATCHUP_KEEP  2u   /* never drain below this */
static unsigned coop_vt_catchup = 0;
static unsigned coop_vt_irap_logged = 0;   /* [COOP-VT/IRAP] lines this session, capped */
#ifdef COOP_PC_DECODER
/* cudrop: a peer pane TAKEN by catch-up and never fed (lost from the peer's chain). cubig: a
 * catch-up carried a peer slice larger than the old 256 KB tile buffer, i.e. one the old build lost. */
static unsigned coop_vt_cu_drop = 0, coop_vt_cu_big = 0;
/* cuirap: catch-up stopped in front of a peer IDR and left it to the normal path (issue #27). */
static unsigned coop_vt_cu_irap = 0;
/* demoted: OUR IDR folded into a TRAIL picture because the peer had no IRAP to pair with (#42);
 * demotefail: the conversion refused, so the picture went out as before. */
static unsigned coop_vt_demoted = 0, coop_vt_demote_fail = 0;
/* idrstale: our IRAP found a sticky peer IDR the composite had already moved past, and left it. */
static unsigned coop_vt_idr_stale = 0;
/* Static, never on the stack (see `tmp` below): our converted IDR slice for this picture. */
static unsigned char coop_vt_demote_buf[1 << 20];
#endif
/* ON BY DEFAULT since 2026-08-03, for the same reason as skip concealment above: it was gated
 * for its first runs, proved itself (evicted 2413 -> 0, and recv == peer+catchup, i.e. every
 * received pane delivered instead of the surplus being silently dropped), and was then left
 * armed by hand where it could not survive. Without it the composite emits one picture per
 * PRIMARY access unit, so the peer's panes are consumed at the PRIMARY's rate, back up, and
 * overflow -- and every overflow tears the peer's prediction chain.
 * `/tmp/coop-nocatchup` turns it off. */
static bool coop_vt_catchup_enabled = true;
static unsigned char coop_vt_catch_buf[1 << 20];
/* The composite's OWN picture order count. A catch-up picture has no primary AU to inherit a
 * POC from, so the composite can no longer borrow the primary's -- it must number its own
 * pictures and rewrite BOTH tiles to match. */
static unsigned coop_vt_poc = 0;
/* Vertical's counterpart of coop_hh_poc_returned/coop_hh_untiled_blocked. Both were added to
 * the horizontal path on 2026-08-05 after a consumed-but-unemitted POC was proved from a
 * capture to be what broke its reference chain. Vertical had neither counter. */
static unsigned coop_vt_poc_returned = 0;
static unsigned coop_vt_refused = 0;
static bool coop_vt_poc_init = false;
static unsigned char coop_vt_skip_pane[256u * 1024u];
/* WINDOWS ONLY (rpsfix3, 2026-09-23/24). Every `#ifdef _WIN32` block in this file carries that
 * build verbatim so the repo reproduces the Windows exe that was tested; the TV (webOS) build
 * compiles the #else side, which is the shipped behaviour, unchanged. Do not ungate any of it
 * for webOS: the keepref swap at the HORIZONTAL conversion site turned the TV BLACK on 2026-08-16
 * (parameter register §11); the vertical site's swap has never been measured on the TV. */
#ifdef COOP_PC_DECODER
/* 1 MB, as the ordinary path's re-tile buffer. It also carries the PEER's slice in both catch-up
 * builders, and at 256 KB every larger peer pane was taken off the queue and then lost when the
 * re-tile refused -- WINDOWS DIAGNOSTIC 2026-09-23 (12 of 247 ordinary peer slices > 256 KB, 0 of
 * 152 catch-up ones). */
static unsigned char coop_vt_skip_tile[1u << 20];
#else
static unsigned char coop_vt_skip_tile[256u * 1024u];
#endif

static unsigned char *coop_conv_buf = NULL;
static size_t coop_conv_cap = 0;
static unsigned coop_hh_converted = 0;
/* Last pane slice received, kept so a fallback picture can REPEAT it instead of
 * substituting our own half. Emitting our slice 1 writes THIS host's content into
 * the bottom of the decoder's reference picture, which breaks the other host's
 * prediction chain -- visible as blocky drift across its pane, and as its pane
 * showing OUR wallpaper. */
static unsigned char *coop_lastpane = NULL;
static size_t coop_lastpane_len = 0;
static int coop_lastpane_type = -1;
static unsigned coop_repeated = 0;
/* OFF by default. Measured offline (tests/coop/coop_sim.py, two real captures at
 * a 60% pane rate): when the host BLANKS the pane it does not own, our slice 1 is
 * a static black region that NVENC codes as skip, so emitting it COPIES the other
 * pane out of the reference picture and holds it at 45dB. Repeating the previous
 * pane instead re-applies its motion and scored 29dB. Repeating only wins when
 * our own half carries real content there, so it stays a sentinel-gated A/B. */
static bool coop_repeat_pane = false;
static unsigned coop_rebootstraps = 0;
/* Consecutive unpaired pictures before we treat the pane feed as stalled. ~2s at
 * 60fps: long enough that ordinary interleaved fallbacks (runs of 1-2, even when the
 * peer delivers at half our rate) never trip it, short enough that a real stall is
 * corrected before the pane visibly rots. */
#define COOP_STALE_PICTURES 120
/* Raised from 10: with the stale trigger below, recovery is the common path rather
 * than a rare one, and a session that degrades late still deserves to recover.
 * Each re-bootstrap costs one paired IRAP. */
#define COOP_MAX_REBOOTSTRAPS 40
static unsigned coop_stale_run = 0;
/* Concealed pictures since the last (re-)bootstrap. CONCEALMENT IS THE DAMAGE: each
 * concealed picture leaves the peer's next inter slice predicting from a reference the
 * decoder never received, so error accumulates and shows as ghosting. Measured live:
 * 61%% of pictures concealed (concealed=1044 of ~1800) with reboot=0 -- nothing ever
 * cleared it, which is the "stream degraded over time" the user reports. ~300 at 60fps
 * is a handful of seconds of drift before a paired IRAP wipes it. */
#define COOP_CONCEAL_BUDGET 300
static unsigned coop_conceal_since_boot = 0;
static size_t coop_poc_cap = 0;
static coop_poc_params_t coop_poc_params;
static unsigned coop_poc_rewrites = 0, coop_poc_failed = 0, coop_poc_unaligned = 0;
static size_t coop_splice_cap = 0;
static unsigned coop_splice_ok = 0, coop_splice_identical = 0, coop_splice_fail = 0;
static bool coop_measure = false;
static unsigned long coop_prev_ms = 0;
static unsigned coop_hist[8];      /* <2,<4,<8,<12,<17,<25,<40,>=40 ms */
static unsigned coop_samples = 0;
static unsigned long coop_delta_sum = 0;
static unsigned long coop_delta_max = 0;

static unsigned coop_skips = 0;      /* frameNumber advanced by >1 */
static int coop_prev_frame = 0;

/* One [COOP-LATE] line per arrival outlier, so a PERIOD can be read off the timestamps.
 * Bounded: at 165fps a wedged session could otherwise write a line per frame. */
#define COOP_LATE_MAX 4000u
static unsigned coop_late_logged = 0;

/* HOLD-RUN: how many consecutive composites showed the SAME peer pane. See the block in
 * the live splice path for why this, and not any existing counter, is the judder. */
#define COOP_HOLD_MAX 4000u
/* THE AGE/HOLD TRACE — passover 07 §3 items 1-2, the measurement Astra asked for first.
 *
 * WHAT WAS MISSING. Every existing co-op gauge is a COUNT: composites, conceals, hold runs. None
 * of them is a DURATION, so a peer half that stops advancing for a second and one that stops for
 * twenty look identical in the log. The user's symptom is measured in seconds, so counts cannot
 * settle it -- passover 07 §3.2 puts it plainly: "Track uninterrupted hold duration in
 * milliseconds, not only counts."
 *
 * ALL MONOTONIC. These are durations; the coordinator's clock was moved to CLOCK_MONOTONIC in
 * the same pass, because a wall-clock step would otherwise appear as a pane that arrived in the
 * future. */
static unsigned long coop_age_peer_ms_max = 0;     /* worst complete-arrival -> composed age */
static unsigned long coop_age_peer_ms_last = 0;
static unsigned long coop_age_peer_ms_sum = 0;
static unsigned coop_age_peer_n = 0;
static unsigned long coop_hold_started_ms = 0;     /* when the current peer pane was first shown */
static unsigned long coop_hold_ms_max = 0;         /* longest uninterrupted hold, ms */
static unsigned long coop_broken_started_ms = 0;   /* when the current chain break began */
static unsigned long coop_broken_ms_max = 0;
static unsigned long coop_recov_request_ms = 0;    /* last peer-IDR request ADMITTED locally */
static unsigned long coop_recov_anchor_ms = 0;     /* last usable peer anchor actually RECEIVED */

static unsigned long coop_mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long) ts.tv_sec * 1000ul + (unsigned long) (ts.tv_nsec / 1000000ul);
}

/* Sampled by the heartbeat thread, which is NOT the feed thread -- that is the whole point of
 * it (passover 07 §3.2: "a stopped feed path cannot censor its own stall"). These are all
 * word-sized reads of values written by the feed thread; a torn read would at worst mis-state
 * one heartbeat's duration by one sample, which is not worth a lock on the video path. */
static void coop_trace_sample(coop_trace_state_t *out) {
    const unsigned long now = coop_mono_ms();
    out->hold_open_ms = coop_hold_started_ms ? now - coop_hold_started_ms : 0ul;
    out->hold_max_ms = coop_hold_ms_max;
    out->broken_open_ms = coop_broken_started_ms ? now - coop_broken_started_ms : 0ul;
    out->broken_max_ms = coop_broken_ms_max;
    out->recov_req_ms = coop_recov_request_ms ? now - coop_recov_request_ms : 0ul;
    out->recov_anchor_ms = coop_recov_anchor_ms ? now - coop_recov_anchor_ms : 0ul;
    out->queued = coop_coord_queued();
}

/* THE PER-PICTURE TRACE'S HANDOFF -- passover 07 §3.1/§3.2.
 *
 * The composite is decided in one place and FED in another (diag_feed, the single choke point
 * every one of the four feed sites already goes through). Passover 07 §3.2 wants both halves on
 * the same row -- "both source identities represented, fresh/held/broken-chain reason, selection
 * time, feed entry/exit and result" -- so the decision arms this struct and the feed completes
 * and emits it. One emit point means a feed can never be traced twice, and an ARMED picture
 * that is never fed is visible as the absence of a row between two heartbeats.
 *
 * Armed per picture, consumed by the next diag_feed, cleared either way. */
static coop_trace_pic_t coop_trace_pending;
static bool coop_trace_pending_armed = false;
static uint32_t coop_trace_prim_frame = 0;
static uint32_t coop_trace_prim_pts = 0, coop_trace_prim_recv = 0, coop_trace_prim_reasm = 0;
static uint16_t coop_trace_prim_host = 0;

/* Arm the trace for the picture about to be built. @p took_peer says whether a pane was just
 * taken from the coordinator -- only then is its provenance meaningful, and reading the
 * snapshot after a take that returned nothing would attribute the PREVIOUS pane to this
 * picture, which is exactly the kind of silent mis-association this trace exists to expose. */
static void coop_trace_arm(coop_trace_kind_t kind, bool took_peer) {
    if (!coop_trace_active()) { return; }
    memset(&coop_trace_pending, 0, sizeof(coop_trace_pending));
    coop_trace_pending.kind = kind;
    coop_trace_pending.prim_frame = coop_trace_prim_frame;
    coop_trace_pending.prim_pts_ms = coop_trace_prim_pts;
    coop_trace_pending.prim_recv_ms = coop_trace_prim_recv;
    coop_trace_pending.prim_reasm_ms = coop_trace_prim_reasm;
    coop_trace_pending.prim_host_10us = coop_trace_prim_host;
    if (took_peer) {
        unsigned long arrived = 0;
        coop_coord_last_taken_meta(&coop_trace_pending.peer, &arrived);
        /* Age computed HERE, not passed in, so both take sites report it the same way. The
         * aged take already returns this number; the catch-up take uses the un-aged entry
         * point and had no way to report it at all. */
        const unsigned long t = coop_mono_ms();
        coop_trace_pending.peer_arrival_age_ms = (arrived != 0 && t > arrived) ? t - arrived : 0ul;
    }
    coop_coord_ipc_timing(&coop_trace_pending.ipc_dwell_ms, NULL,
                          &coop_trace_pending.send_block_ms, NULL);
    coop_trace_pending.queued_after = coop_coord_queued();
    coop_trace_pending_armed = true;
}

/* Publish the two identities the panel overlay shows. Called on every co-op composite,
 * INCLUDING when the trace is off -- the overlay is the human half of passover 07 §3.4 and
 * must not depend on a sentinel somebody forgot to arm. `peer_frame` is 0 when this picture
 * did not take a fresh pane, and the published value then holds at the last one that did,
 * which is exactly the reading the camera comparison needs: a frozen accepted identity under
 * an advancing received one. */
static void coop_trace_publish_identity(uint32_t peer_frame) {
    coop_trace_identity_set(coop_trace_prim_frame, peer_frame);
}

static unsigned coop_hold_run = 0;      /* composites the current peer pane has been shown */
static unsigned coop_hold_hist[9];      /* index = run length, 8 = "8 or more" */
static unsigned coop_hold_n = 0;        /* completed holds */
static unsigned long coop_hold_sum = 0;
static unsigned coop_hold_logged = 0;

/* Only consecutive frames (frameNumber +1) measure delivery jitter; a gap in
 * frame numbers is a loss, and a long gap WITH consecutive numbers is the host
 * simply not producing a frame (static content). Conflating those is what made
 * the first run unreadable. */
static void coop_measure_sample2(unsigned long now, int frame) {
    bool consecutive = (coop_prev_frame != 0) && (frame == coop_prev_frame + 1);
    if (frame > coop_prev_frame + 1 && coop_prev_frame != 0) { coop_skips++; }
    coop_prev_frame = frame;
    if (!consecutive) { coop_prev_ms = now; return; }
    if (coop_prev_ms != 0) {
        unsigned long d = now - coop_prev_ms;
        int b = d < 2 ? 0 : d < 4 ? 1 : d < 8 ? 2 : d < 12 ? 3
              : d < 17 ? 4 : d < 25 ? 5 : d < 40 ? 6 : 7;
        coop_hist[b]++;
        coop_samples++;
        coop_delta_sum += d;
        if (d > coop_delta_max) { coop_delta_max = d; }
        /* WHEN each hitch happened, not just how many there were.
         *
         * The histogram above can prove long deltas EXIST and can never show that they
         * arrive on a fixed cadence -- and the user's report takes exactly that form
         * ("either an exact 3 second interval or really close"). Periodicity is only
         * readable from TIMESTAMPS, so emit one line per outlier and let the analysis
         * difference them. This is the measurement two stills per phase and a 0.5/s
         * screenshot probe cannot make, whatever their resolution.
         *
         * The threshold is RELATIVE (3x the running mean, floor 20ms) because the matrix
         * drives phases from 5fps to 165fps: a fixed ms threshold would be silent at 165
         * and a firehose at 5. Bounded at COOP_LATE_MAX lines because /tmp is RAM on this
         * box and a pathological session must not fill it. */
        if (coop_samples > 30 && coop_late_logged < COOP_LATE_MAX && diag_log != NULL) {
            const unsigned long mean = coop_delta_sum / coop_samples;
            const unsigned long thresh = (mean * 3 > 20) ? mean * 3 : 20;
            if (d >= thresh) {
                fprintf(diag_log, "[COOP-LATE] t=%.3f d=%lums mean=%lums frame=%d\n",
                        (double) now / 1000.0, d, mean, frame);
                fflush(diag_log);
                coop_late_logged++;
            }
        }
        if (coop_samples % 300 == 0 && diag_log != NULL) {
            fprintf(diag_log,
                    "[COOP-JITTER] n=%u mean=%.2fms max=%lums  <2:%u <4:%u <8:%u <12:%u "
                    "<17:%u <25:%u <40:%u >=40:%u\n",
                    coop_samples, (double) coop_delta_sum / (double) coop_samples,
                    coop_delta_max, coop_hist[0], coop_hist[1], coop_hist[2], coop_hist[3],
                    coop_hist[4], coop_hist[5], coop_hist[6], coop_hist[7]);
            fprintf(diag_log, "[COOP-JITTER]   (consecutive frames only; frame-number skips=%u)\n",
                    coop_skips);
            fflush(diag_log);
        }
    }
    coop_prev_ms = now;
}
/* PLAY-FROM-FILE state: a bitstream we built, fed to the TV's own decoder. See the use
 * site in vdec_delegate_submit for why this exists. */
static unsigned char *coop_play_data = NULL;
/* Access units play-from-file will cycle. 64 was far too few: at 165fps that is a 0.39s
 * loop, and since AU 1 is an IDR the decoder gets a full reset 2.6 times a SECOND, which
 * on the panel looks like the whole picture flashing on and off in a steady pattern. It
 * reads exactly like a decode fault and is purely the harness -- confirmed by mirroring one
 * pane into both halves, where BOTH sides flashed together rather than one. */
#define COOP_PLAY_MAX_AUS 600u
static size_t coop_play_off[COOP_PLAY_MAX_AUS], coop_play_len[COOP_PLAY_MAX_AUS];
static size_t coop_play_au_count = 0, coop_play_next = 0;

static void coop_play_load(void) {
    coop_play_au_count = coop_play_next = 0;
    if (access("/tmp/coop-playfile", F_OK) != 0) {
        return;
    }
    FILE *pf = fopen("/tmp/coop-play.h265", "rb");
    if (pf == NULL) {
        return;
    }
    fseek(pf, 0, SEEK_END);
    long sz = ftell(pf);
    fseek(pf, 0, SEEK_SET);
    if (sz <= 8 || sz > (32 << 20)) { fclose(pf); return; }
    free(coop_play_data);
    coop_play_data = malloc((size_t) sz);
    if (coop_play_data == NULL) { fclose(pf); return; }
    size_t got = fread(coop_play_data, 1, (size_t) sz, pf);
    fclose(pf);
    /* Split on VPS: each generated AU starts with its parameter sets. */
    for (size_t i = 0; i + 5 < got && coop_play_au_count < COOP_PLAY_MAX_AUS; ++i) {
        if (coop_play_data[i] == 0 && coop_play_data[i + 1] == 0 &&
            coop_play_data[i + 2] == 0 && coop_play_data[i + 3] == 1 &&
            ((coop_play_data[i + 4] >> 1) & 0x3F) == 32) {
            if (coop_play_au_count > 0) {
                coop_play_len[coop_play_au_count - 1] = i - coop_play_off[coop_play_au_count - 1];
            }
            coop_play_off[coop_play_au_count++] = i;
        }
    }
    if (coop_play_au_count > 0) {
        coop_play_len[coop_play_au_count - 1] = got - coop_play_off[coop_play_au_count - 1];
    }
    if (diag_log != NULL) {
        fprintf(diag_log, "[COOP-PLAY] loaded %zu bytes, %zu access units\n", got,
                coop_play_au_count);
        fflush(diag_log);
    }
}

static FILE *coop_dump = NULL;
/* Per-session dump path, logged so a stale artifact can never be mistaken for a fresh one. */
static char coop_dump_path[64] = {0};
/* Post-composition dump: what the DECODER receives, as opposed to coop_dump which is
 * sampled before the splice and therefore cannot show a composition fault. */
static FILE *coop_dump_out = NULL;
static unsigned coop_dump_out_frames = 0;
/* Window placement for the post-composition dump: how many pictures to let pass first,
 * whether the IRAP we asked for has arrived, and the running count. */
static unsigned coop_dump_out_start = 0, coop_dump_out_seen = 0;

/* ── ROLLING CAPTURE: bridging the timescale gap ───────────────────────────────
 *
 * THE PROBLEM THIS SOLVES (user, 2026-08-05). The peer-tile decay takes MINUTES
 * to appear, while every instrument here works in seconds: the head capture takes
 * the first 400 access units -- about 4 seconds of a PRISTINE startup, which is
 * the wrong end of the timeline entirely. Three captures were taken during the
 * decay hunt and none contained the failure.
 *
 * So keep a rolling window of the MOST RECENT N megabytes instead, and write it
 * out when asked. When the artefact is on screen, the bitstream that produced it
 * is still in memory.
 *
 * SIZING IS A RAM DECISION, NOT A DISK ONE. /tmp is tmpfs, so the ring costs real
 * memory on a TV that has ~464MB available while decoding 4K. At ~78KB per access
 * unit, 96MB holds ~1250 AUs = ~13s at 96fps. Set the size in MB in
 * /tmp/coop-dump-ring; keep it modest.
 *
 * THE DUMP STARTS AT AN IRAP, always. A window starting mid-GOP decodes to
 * nothing, which is exactly the "capture is broken" dead end that cost most of a
 * day -- the file was often fine, it simply had no keyframe to start from.
 *
 * Trigger: `touch /tmp/coop-dump-now`. Polled once every 120 pictures rather than
 * per picture: the hot path must not stat() the filesystem at 165fps. */
typedef struct {
    uint32_t off, len;
    uint8_t irap;
} coop_cap_rec_t;

#define COOP_CAP_MAX_RECS 8192u

static unsigned char *coop_cap_buf = NULL;
static size_t coop_cap_bytes = 0, coop_cap_w = 0;
static coop_cap_rec_t *coop_cap_idx = NULL;
static unsigned coop_cap_head = 0, coop_cap_count = 0;
static unsigned coop_cap_poll = 0, coop_cap_dumps = 0;
static time_t coop_cap_trigger_mtime = 0;

/** Overlap test in the ring's linear address space. */
static bool coop_cap_overlaps(const coop_cap_rec_t *r, size_t start, size_t len) {
    return (size_t) r->off < start + len && start < (size_t) r->off + r->len;
}

static void coop_cap_store(const unsigned char *au, size_t len, bool irap) {
    if (coop_cap_buf == NULL || len == 0 || len > coop_cap_bytes / 4u) {
        return;
    }
    if (coop_cap_w + len > coop_cap_bytes) {
        coop_cap_w = 0;                       /* wrap; records are evicted below */
    }
    /* Evict every record the incoming AU is about to overwrite, oldest first. */
    while (coop_cap_count > 0 &&
           coop_cap_overlaps(&coop_cap_idx[coop_cap_head], coop_cap_w, len)) {
        coop_cap_head = (coop_cap_head + 1u) % COOP_CAP_MAX_RECS;
        coop_cap_count--;
    }
    if (coop_cap_count == COOP_CAP_MAX_RECS) {
        coop_cap_head = (coop_cap_head + 1u) % COOP_CAP_MAX_RECS;
        coop_cap_count--;
    }
    memcpy(coop_cap_buf + coop_cap_w, au, len);
    const unsigned slot = (coop_cap_head + coop_cap_count) % COOP_CAP_MAX_RECS;
    coop_cap_idx[slot].off = (uint32_t) coop_cap_w;
    coop_cap_idx[slot].len = (uint32_t) len;
    coop_cap_idx[slot].irap = irap ? 1u : 0u;
    coop_cap_count++;
    coop_cap_w += len;
}

/* A DUMP ASKED FOR IS NOT A DUMP THAT CAN BE WRITTEN. The window only decodes from an
 * IRAP, and the composite emits one at session start and then never again -- our host runs
 * an infinite GOP and the peer's IDRs are converted to TRAIL. Measured 2026-08-16: a
 * trigger 40s into a phase held 1712 AUs and not one keyframe, so the flush correctly
 * refused to write. So the trigger must FORCE a keyframe and then WAIT for it, rather than
 * flush whatever happens to be in memory. */
#define COOP_CAP_AFTER_IRAP  300u   /* AUs wanted after the keyframe (~1.8s at 165fps) */
#define COOP_CAP_WAIT_POLLS   20u   /* give up after ~15s and report honestly */
static bool coop_cap_want = false;
static unsigned coop_cap_want_polls = 0;

/** AUs held after the oldest IRAP in the window; 0 when there is no IRAP at all. */
static unsigned coop_cap_after_irap(void) {
    for (unsigned i = 0; i < coop_cap_count; ++i) {
        if (coop_cap_idx[(coop_cap_head + i) % COOP_CAP_MAX_RECS].irap) {
            return coop_cap_count - i;
        }
    }
    return 0;
}

/** Write the window out, starting at the OLDEST IRAP so the result decodes. */
static void coop_cap_flush(void) {
    if (coop_cap_buf == NULL || coop_cap_count == 0) {
        return;
    }
    unsigned start = 0;
    bool found = false;
    for (unsigned i = 0; i < coop_cap_count; ++i) {
        if (coop_cap_idx[(coop_cap_head + i) % COOP_CAP_MAX_RECS].irap) {
            start = i;
            found = true;
            break;
        }
    }
    if (!found) {
        if (diag_log != NULL) {
            fprintf(diag_log, "[COOP-DUMP] ring: no IRAP in the window -- nothing decodable "
                              "to write (%u AUs held)\n", coop_cap_count);
            fflush(diag_log);
        }
        return;
    }
    FILE *f = fopen("/tmp/coop_composed.h265", "wb");
    if (f == NULL) {
        return;
    }
    unsigned written = 0;
    for (unsigned i = start; i < coop_cap_count; ++i) {
        const coop_cap_rec_t *r = &coop_cap_idx[(coop_cap_head + i) % COOP_CAP_MAX_RECS];
        fwrite(coop_cap_buf + r->off, 1, r->len, f);
        written++;
    }
    fclose(f);
    coop_cap_dumps++;
    if (diag_log != NULL) {
        fprintf(diag_log, "[COOP-DUMP] ring flushed: %u AUs from the oldest IRAP "
                          "(%u held, dump #%u)\n", written, coop_cap_count, coop_cap_dumps);
        fflush(diag_log);
    }
}

/* ============================ NETCAP: continuous composite capture ============================
 * WHY THIS EXISTS, in one paragraph, because the alternatives were tried and are dead ends.
 * Every fault this project is chasing develops over MINUTES (a pane pristine at 13 s is heavily
 * ghosted at 73 s). The two existing captures cannot show that:
 *   - `/tmp/coop-dump-out` stops at 400 AUs, ~6.7 s -- and on 2026-09-02 a verdict was drawn by
 *     decoding its frame 300 and comparing it against a PANEL state a minute later. That
 *     comparison is meaningless, and it produced a wrong finding that had to be retracted.
 *   - the ring (`/tmp/coop-dump-ring`) can only flush from an IRAP inside its window, and this
 *     stream runs an INFINITE GOP by requirement (any periodic IDR is a visible flash the user
 *     has ruled out absolutely), so the only IRAP is at session start. Triggering it REQUESTS an
 *     IDR, which repairs the very drift we are trying to photograph.
 * tmpfs cannot hold minutes of this (~78 KB/AU at 60 fps is ~4.7 MB/s, and the TV has ~464 MB
 * free while decoding 4K -- that constraint is WHY the ring exists), and webOS has no `scp`, so
 * the capture cannot be pulled either. The only surface with room is SKYNET's disk, so we PUSH.
 *
 * NEVER DO I/O ON THE SUBMIT THREAD. This is a copy of the shape of coop_mirror_feed(): the
 * producer memcpy's into a slot and DROPS when the consumer is behind; a detached pthread owns
 * the socket and may block as much as it likes. That rule is not stylistic -- a failing feed on
 * this thread has twice stalled primary decode and produced a black screen.
 *
 * Arm with: echo "<host>:<port>" > /tmp/coop-netcap   (read once, at stream start)
 * Receiver: scripts/coop/netcap_recv.py
 */
#define COOP_NETCAP_SLOTS 48           /* ~3.7MB at 78KB/AU; deep enough to ride a network hiccup */
#define COOP_NETCAP_MAGIC 0x31434E41u  /* "ANC1" little-endian */

typedef struct {
    unsigned char *data;
    size_t size, cap;
    uint64_t wall_ns;
    unsigned frame, peer_frame;
    unsigned char irap;
    unsigned char kind;          /* 0 = spliced composite, 1 = peer pane as received */
    volatile int ready;
} coop_netcap_slot_t;

static coop_netcap_slot_t coop_netcap_ring[COOP_NETCAP_SLOTS];
static volatile unsigned coop_netcap_w = 0, coop_netcap_r = 0;
/* Slots abandoned after a bounded not-ready spin. MUST stay 0: any nonzero value means
 * the ring invariant broke, which on 2026-09-02 cost an entire capture silently. */
static unsigned coop_netcap_stuck = 0;
static pthread_t coop_netcap_thread;
static volatile int coop_netcap_run = 0;
static intptr_t coop_netcap_fd = -1;
static bool coop_netcap_armed = false;
static char coop_netcap_target[128];
/* Counters, because a capture that silently writes nothing is indistinguishable from a stream
 * with no fault -- which is exactly how a whole afternoon was lost to /tmp/coop-playfile. */
static unsigned coop_netcap_sent = 0, coop_netcap_drop = 0, coop_netcap_err = 0;

static uint64_t coop_netcap_wall_ns(void) {
    /* CLOCK_REALTIME DELIBERATELY, not coop_now_ns(): that one is CLOCK_MONOTONIC rebased at the
     * first fed frame, so it cannot be compared with a TV screenshot's mtime or with `date` on
     * either host. Alignment against the panel is the whole point of this capture. */
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t) t.tv_sec * 1000000000ull + (uint64_t) t.tv_nsec;
}

static bool coop_netcap_connect(void) {
    char host[96]; int port = 0;
    const char *colon = strrchr(coop_netcap_target, ':');
    if (colon == NULL || (size_t)(colon - coop_netcap_target) >= sizeof(host)) { return false; }
    memcpy(host, coop_netcap_target, (size_t)(colon - coop_netcap_target));
    host[colon - coop_netcap_target] = '\0';
    port = atoi(colon + 1);
    if (port <= 0 || port > 65535) { return false; }

    intptr_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { return false; }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t) port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) { close(fd); return false; }
    if (connect(fd, (struct sockaddr *) &sa, sizeof(sa)) != 0) { close(fd); return false; }
    /* TCP_NODELAY: we send ~78KB records at 60Hz; Nagle would coalesce and add nothing but
     * latency to the RECEIVER's view of when a picture was emitted. */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    coop_netcap_fd = fd;
    return true;
}

static void *coop_netcap_worker(void *arg) {
    (void) arg;
    /* RETRY INSIDE THE WORKER. The first version returned on a failed connect and cleared
     * coop_netcap_run, which made coop_netcap_feed() spawn A NEW WORKER ON EVERY PICTURE -- a
     * thread storm on the decode submit path at 60 fps. Measured 2026-09-02:
     * `nc[sent=0 drop=24850 err=0]` with "connect FAILED (Connection timed out)", because the
     * receiver had already exited after accepting an earlier stream's connection.
     * Retrying here also means a receiver started slightly late is still picked up, instead of
     * the run being silently uncaptured. Bounded, so a permanently absent receiver disarms
     * rather than retrying for the life of the stream. */
    int attempts = 0;
    while (coop_netcap_run && !coop_netcap_connect()) {
        if (++attempts >= 15) {          /* ~30 s of trying */
            if (diag_log != NULL) {
                fprintf(diag_log, "[COOP-NETCAP] connect to %s failed %d times (%s) -- "
                                  "DISARMING; start netcap_recv.py before the stream\n",
                        coop_netcap_target, attempts, strerror(errno));
                fflush(diag_log);
            }
            coop_netcap_run = 0;
            coop_netcap_armed = false;   /* do NOT let feed() respawn us */
            return NULL;
        }
        SDL_Delay(2000);
    }
    if (!coop_netcap_run) { return NULL; }
    /* DISCARD WHATEVER QUEUED WHILE WE WERE CONNECTING. The ring drops NEW access units when it
     * is full, so its 48 slots hold the frames from the moment it first filled -- which, after a
     * retry loop, is minutes in the past. Sending those first stamps the recording with ancient
     * wall clocks and makes the index lie about when the run happened. Measured 2026-09-02: a
     * 115-second capture reported a 779-second span for exactly this reason. A capture must be
     * CONTEMPORANEOUS; start it at the live edge. */
    /* START AT THE OLDEST IRAP STILL IN THE RING, ELSE AT THE LIVE EDGE.
     *
     * The first version discarded the whole ring at connect (`coop_netcap_r = coop_netcap_w`),
     * to stop a retry loop's minutes-old frames being sent as if they were current. That was
     * right about staleness and WRONG about decodability: with an infinite GOP the session's
     * only IDR may already be sitting in the ring, and throwing it away leaves the recording
     * with no entry point at all -- measured 2026-09-02, "composite 0 / peer 6520" after two
     * minutes, because the composite stream was still waiting for an IRAP that was never
     * going to come again.
     *
     * The SECOND version cleared `ready` on every non-IRAP slot and left `coop_netcap_r` where
     * it was. THAT DEADLOCKED THE SENDER, measured live 2026-09-02: the send loop below does
     * `if (!slot->ready) { SDL_Delay(1); continue; }` WITHOUT advancing `coop_netcap_r`, so it
     * span forever on the first cleared slot; `coop_netcap_w` ran away until
     * `w - r >= COOP_NETCAP_SLOTS` was permanently true, and every access unit was dropped.
     * The run reported `nc[sent=2 drop=10427 err=0]` and produced a 1-AU capture.
     * Note the trap for anyone re-reading this: that broken run's meta.json still said
     * `composite_iraps: 1, composite_skipped_pre_irap: 0` -- the obvious success indicators
     * PASSED on a capture that was 3 KB of nothing. `aus` and `span_s` are what actually say
     * whether a capture happened.
     *
     * So: do not touch `ready` at all. MOVE THE READ POINTER to the oldest slot that is both
     * ready and an IRAP, which keeps a decodable entry point and drops everything staler; if
     * the ring holds no IRAP, fall back to the original live-edge behaviour. The read pointer
     * then always lands on a ready slot, which is the invariant the sender relies on. */
    /* SNAPSHOT `coop_netcap_w` ONCE. It is volatile and the producer advances it concurrently,
     * so re-reading it live gives two different answers in the same block. Reviewed 2026-09-03:
     * the first version snapshotted it for `keep` but RE-READ it in the diag test below, so if
     * the producer advanced between the two reads the no-IRAP case printed "resuming at IRAP
     * slot N". That error is one-directional -- it can only ever falsely report SUCCESS -- which
     * is precisely the shape this diag line exists to rule out, since the whole reason it names
     * the mechanism is that the symptom indicators lie. */
    const unsigned w_snap = coop_netcap_w;
    unsigned had_r = coop_netcap_r;         /* only so the diag line can report what was dropped */
    unsigned keep = w_snap;                 /* no IRAP found -> live edge, the original behaviour */
    bool found = false;                     /* explicit, so the diag cannot infer success wrongly */
    for (unsigned i = had_r; i != w_snap; ++i) {
        coop_netcap_slot_t *s = &coop_netcap_ring[i % COOP_NETCAP_SLOTS];
        /* THE `kind == 0` FILTER IS THE POINT, NOT AN OPTIMISATION. This ONE ring interleaves
         * both tagged streams -- kind 0 the spliced COMPOSITE, kind 1 the peer's pane as
         * received. An unpaired PEER IRAP satisfies `ready && irap` just as well, so anchoring
         * on one leaves the COMPOSITE stream with no entry point at all: precisely the
         * `composite 0 / peer 6520` this whole block exists to kill, except now the diag line
         * would confidently report "resuming at IRAP slot N" while doing it. It hides in the
         * common case -- at t=0 the peer AU is fed just before the composite for the same
         * picture, so `r` lands one slot early and the composite IDR is swept in anyway -- and
         * bites after any connect retry. Found by review 2026-09-03, not by a run. */
        if (s->ready && s->irap && s->kind == 0) { keep = i; found = true; break; }
    }
    coop_netcap_r = keep;
    if (diag_log != NULL) {
        /* SAY WHICH PATH WAS TAKEN. The 2026-09-02 deadlock produced a meta.json reading
         * `composite_iraps: 1, composite_skipped_pre_irap: 0` -- every obvious success
         * indicator PASSED on a 3 KB capture of nothing. So the proof that this logic worked
         * has to name the mechanism, not the symptom. */
        /* `seq=` NOT `slot=`: these are monotonic SEQUENCE numbers, not ring indices 0..47,
         * so an unlabelled "slot 10427" reads as nonsense. The ring index is printed beside it.
         * This is the register's documented "labels meaning different things per [COOP-*] line"
         * trap, and it costs one format specifier to not fall into. */
        if (found) {
            fprintf(diag_log, "[COOP-NETCAP] connected to %s -- resuming at COMPOSITE IRAP "
                              "seq=%u (ring slot %u), dropped %u stale AUs\n",
                    coop_netcap_target, keep, keep % COOP_NETCAP_SLOTS, keep - had_r);
        } else {
            fprintf(diag_log, "[COOP-NETCAP] connected to %s -- no COMPOSITE IRAP in ring, "
                              "starting at live edge (dropped %u stale AUs)\n",
                    coop_netcap_target, keep - had_r);
        }
        fflush(diag_log);
    }
    unsigned char hdr[32];
    unsigned notready = 0;
    while (coop_netcap_run) {
        if (coop_netcap_r == coop_netcap_w) { SDL_Delay(1); continue; }
        coop_netcap_slot_t *slot = &coop_netcap_ring[coop_netcap_r % COOP_NETCAP_SLOTS];
        /* NOT-READY SLOT: SPIN, BUT BOUNDED. Never spin forever, never skip immediately.
         *
         * The producer sets `ready` BEFORE incrementing `coop_netcap_w`, so every slot in
         * [r, w) is ready in normal operation and this branch should be unreachable. Two
         * failure modes have to be covered and they want OPPOSITE things:
         *   - A WEDGE (what happened 2026-09-02): the read pointer parked on a slot whose
         *     `ready` had been cleared behind it. The old `SDL_Delay(1); continue;` never
         *     advanced `coop_netcap_r`, so the sender span forever, the ring read permanently
         *     full and every AU was dropped -- visible ONLY as a climbing `drop` counter.
         *   - A STORE-VISIBILITY TRANSIENT: there is not one atomic or barrier in this file
         *     (`volatile` orders nothing on ARM), so a consumer that observes `coop_netcap_w`
         *     advanced may briefly still see `ready == 0` for that slot. Skipping on sight
         *     would DISCARD a good access unit -- and if it were the IRAP, it would cost the
         *     recording the very entry point this connect logic exists to preserve.
         * So: retry for up to ~1 s, which is orders of magnitude beyond any store-visibility
         * window, then treat it as a wedge -- advance, count it, and SAY SO. The original bug
         * was silent; this one cannot be.
         *
         * COST OF THE BOUND, corrected by review 2026-09-03: a 1 s stall costs ~100-200 AUs of
         * CAPTURE, not ~60 -- the 48-slot ring is SHARED by the composite and peer streams
         * (~0.4 s of history in total), so both stop together. It costs no STREAM latency: the
         * worker is a detached pthread and `coop_netcap_feed()` drops rather than blocks. And
         * the bound does not RECOVER from a real wedge -- draining one slot per second against
         * ~100 AU/s inbound leaves the capture dead, just loud. Loud is the point. */
        if (!slot->ready) {
            if (++notready < 1000) { SDL_Delay(1); continue; }
            coop_netcap_stuck++;
            if (diag_log != NULL) {
                fprintf(diag_log, "[COOP-NETCAP] seq=%u (ring slot %u) not ready after ~1s -- "
                                  "skipping (ncstuck=%u sent=%u drop=%u)\n",
                        coop_netcap_r, coop_netcap_r % COOP_NETCAP_SLOTS,
                        coop_netcap_stuck, coop_netcap_sent, coop_netcap_drop);
                fflush(diag_log);
            }
            notready = 0;
            coop_netcap_r++;
            continue;
        }
        notready = 0;

        memset(hdr, 0, sizeof(hdr));
        uint32_t magic = COOP_NETCAP_MAGIC, len = (uint32_t) slot->size;
        uint32_t fr = slot->frame, pfr = slot->peer_frame;
        uint64_t wall = slot->wall_ns;
        memcpy(hdr + 0,  &magic, 4);
        memcpy(hdr + 4,  &len,   4);
        memcpy(hdr + 8,  &wall,  8);
        memcpy(hdr + 16, &fr,    4);
        memcpy(hdr + 20, &pfr,   4);
        hdr[24] = slot->irap;
        hdr[25] = slot->kind;

        bool ok = true;
        size_t off = 0;
        while (off < sizeof(hdr) && ok) {
            ssize_t n = send(coop_netcap_fd, hdr + off, sizeof(hdr) - off, 0);
            if (n > 0) { off += (size_t) n; } else if (n < 0 && errno == EINTR) { continue; } else { ok = false; }
        }
        off = 0;
        while (off < slot->size && ok) {
            ssize_t n = send(coop_netcap_fd, slot->data + off, slot->size - off, 0);
            if (n > 0) { off += (size_t) n; } else if (n < 0 && errno == EINTR) { continue; } else { ok = false; }
        }
        if (ok) {
            coop_netcap_sent++;
        } else {
            coop_netcap_err++;
            if (diag_log != NULL) {
                fprintf(diag_log, "[COOP-NETCAP] send failed after %u AUs (%s) -- stopping\n",
                        coop_netcap_sent, strerror(errno));
                fflush(diag_log);
            }
            coop_netcap_run = 0;   /* a half-written AU makes the recording undecodable; stop clean */
        }
        slot->ready = 0;
        coop_netcap_r++;
    }
    if (coop_netcap_fd >= 0) { close(coop_netcap_fd); coop_netcap_fd = -1; }
    return NULL;
}

/* Called from the submit thread: COPY ONLY, NEVER BLOCK. */
static void coop_netcap_feed(const unsigned char *data, size_t size,
                             unsigned frame, unsigned peer_frame, bool irap,
                             unsigned char kind) {
    if (!coop_netcap_armed || size == 0) { return; }
    if (!coop_netcap_run) {
        coop_netcap_run = 1;
        if (pthread_create(&coop_netcap_thread, NULL, coop_netcap_worker, NULL) != 0) {
            coop_netcap_run = 0;
            coop_netcap_armed = false;   /* do not retry every picture */
            return;
        }
        pthread_detach(coop_netcap_thread);
    }
    if (coop_netcap_w - coop_netcap_r >= COOP_NETCAP_SLOTS) {
        coop_netcap_drop++;
        return;   /* consumer behind -- drop, never stall the stream */
    }
    coop_netcap_slot_t *slot = &coop_netcap_ring[coop_netcap_w % COOP_NETCAP_SLOTS];
    if (slot->cap < size) {
        unsigned char *q = realloc(slot->data, size);
        if (q == NULL) { coop_netcap_drop++; return; }
        slot->data = q;
        slot->cap = size;
    }
    memcpy(slot->data, data, size);
    slot->size = size;
    slot->wall_ns = coop_netcap_wall_ns();
    slot->frame = frame;
    slot->peer_frame = peer_frame;
    slot->irap = irap ? 1u : 0u;
    slot->kind = kind;
    slot->ready = 1;
    coop_netcap_w++;
}

static void coop_netcap_setup(void) {
    /* STOP ANY PREVIOUS WORKER AND DRAIN THE RING BEFORE ARMING.
     *
     * vdec_delegate_setup() can run TWICE IN ONE PROCESS: `ares-launch --close` has repeatedly
     * reported success while the app kept running (it is one of the documented reasons
     * run_coop.sh exists), so what looks like a restart may be a second stream in the same
     * process. Zeroing the indices alone is not enough -- the slots keep their `ready` flags and
     * their PREVIOUS stream's bytes, and the worker sends those first.
     *
     * MEASURED 2026-09-02, the first time this ran twice: a recording whose first AU was
     * timestamped 785 s before its last, and which contained NO IRAP at all, because the stale
     * AUs went out ahead of the new stream's IDR. An 80-second capture reported an 865-second
     * span. Undecodable, and it would have read as "the capture works" from the byte count alone.
     * A stale worker also still owns a socket to a receiver that is long gone. */
    coop_netcap_run = 0;
    SDL_Delay(5);                 /* the worker polls run every 1 ms; let it exit and close its fd */
    for (int i = 0; i < COOP_NETCAP_SLOTS; ++i) {
        coop_netcap_ring[i].ready = 0;
        coop_netcap_ring[i].size = 0;
    }
    coop_netcap_armed = false;
    coop_netcap_target[0] = '\0';
    coop_netcap_sent = coop_netcap_drop = coop_netcap_err = 0;
    coop_hh_skip_events = coop_hh_skip_frames = coop_hh_skip_max = 0;
    coop_netcap_w = coop_netcap_r = 0;
    FILE *f = fopen("/tmp/coop-netcap", "r");
    if (f == NULL) { return; }
    if (fgets(coop_netcap_target, sizeof(coop_netcap_target), f) != NULL) {
        char *nl = strpbrk(coop_netcap_target, "\r\n");
        if (nl != NULL) { *nl = '\0'; }
        if (coop_netcap_target[0] != '\0') { coop_netcap_armed = true; }
    }
    fclose(f);
    if (coop_netcap_armed && diag_log != NULL) {
        fprintf(diag_log, "[COOP-NETCAP] armed for %s (connect happens on the first picture)\n",
                coop_netcap_target);
        fflush(diag_log);
    }
}

static void coop_netcap_teardown(void) {
    coop_netcap_run = 0;
    coop_netcap_armed = false;
    /* The worker owns the fd and closes it on exit; it polls coop_netcap_run every 1 ms, so it
     * is gone well before the next stream opens. Slots are freed here because the worker must
     * not touch them after it stops. */
    SDL_Delay(5);
    for (int i = 0; i < COOP_NETCAP_SLOTS; ++i) {
        free(coop_netcap_ring[i].data);
        coop_netcap_ring[i].data = NULL;
        coop_netcap_ring[i].cap = coop_netcap_ring[i].size = 0;
        coop_netcap_ring[i].ready = 0;
    }
}

static unsigned coop_dump_out_reach = 0;
static bool coop_dump_out_open = false;
static FILE *coop_spliced_dump = NULL;
static bool coop_bootstrapped = false;
static bool coop_conceal = false;
static unsigned coop_fb_nopane = 0, coop_fb_mismatch = 0, coop_concealed = 0;
static unsigned coop_idr_requests = 0;
static unsigned coop_spliced_frames = 0;
static unsigned coop_dump_frames = 0;
/* Access units /tmp/coop-dump captures. 90 was fine for "what shape is the bitstream", but
 * it is only 0.55s at 165fps -- far too short to AVERAGE anything. An encoder A/B over 90
 * AUs was pure noise: within-config spread in mean slice QP was as large as the difference
 * being measured. The sentinel may now contain a number to override this. */
#define COOP_DUMP_FRAMES_DEFAULT 600u
static unsigned coop_dump_limit = COOP_DUMP_FRAMES_DEFAULT;

static int frames_since_idr = 0;

static session_t *session = NULL;
static SS4S_Player *player = NULL;
static unsigned char *buffer = NULL;
static size_t buffer_size = 0;
static size_t buffer_initial_size = 0;
static int lastFrameNumber;
static unsigned long lastIdrRequestMs = 0;
static struct VIDEO_STATS vdec_temp_stats;

/* WHICH PROCESS WROTE THIS LINE, and it is not cosmetic.
 *
 * In co-op the primary and the secondary are SEPARATE PROCESSES (coop_secondary.c) that
 * both append to /tmp/aurora-diag.log with no tag, and each keeps its own `diag_session`
 * counter -- so two independent runs' counters interleave in one file and read as one
 * stream of numbers. Measured 2026-08-02: `[COOP-LIVE] v5 staleRun=` climbed 1:1 with
 * `paired=` and looked like a total pane stall; the two lines were simply from different
 * processes, and the HH and v5 composition paths are mutually exclusive WITHIN a process
 * (slice_count==1 vs ==2), so no single process can produce both. Without a tag there is
 * no way to see that in the log.
 *
 * Placed AFTER the "[COOP-xx]" token so existing greps (run_coop.sh:170) still match. */
static const char *coop_role_tag(void) {
    return coop_secondary_mode() != NULL ? "secondary" : "primary";
}

/* Build ONE catch-up composite access unit: the peer's pane in tile 1 against a CONCEALED
 * primary tile, both rewritten to @p poc. Returns bytes written to @p out, 0 if it cannot be
 * built (which is always safe -- the caller simply does not emit one).
 *
 * See docs/coop-rate-decoupling.md. Both primitives are already proven: coop_build_skip_slice
 * is the bit-exact concealer used for the peer's tile, and coop_slice_to_tile re-homes a
 * pane-shaped slice into either column (tile 0 = address 0, tile 1 = address w_ctbs).
 *
 * @p mine / @p mine_len is the primary's LAST real slice -- its header supplies the reference
 * picture set and the temporal-MVP flag, which every slice of the picture must agree on. */
/* HORIZONTAL's catch-up, the exact counterpart of coop_vt_build_catchup(). The half-height
 * path has the SAME structural bug the vertical one had -- it emits one picture per PRIMARY
 * access unit, so the peer's panes are consumed at the primary's rate and back up when the
 * primary slows. Kept as a separate function rather than generalised because the two composes
 * differ in shape (tiles vs stacked halves) and merging them would touch the proven
 * horizontal path more than porting does.
 *
 * Top half = OUR last slice concealed all-skip at address 0; bottom half = the peer's pane
 * relocated to @p bottom_addr. Both retimed to @p poc. Returns 0 (safely) if it cannot build.
 */
static size_t coop_hh_build_catchup(const unsigned char *mine, size_t mine_len,
                                    const unsigned char *pane, size_t pane_len,
                                    unsigned poc, unsigned char *out, size_t out_cap) {
    if (mine == NULL || mine_len == 0 || pane == NULL || pane_len == 0) {
        coop_hh_cu_why[1]++; return 0;
    }
    if (coop_hh_nonvcl_len == 0 || coop_hh_nonvcl_len > out_cap) {
        coop_hh_cu_why[2]++; return 0;
    }
    coop_au_t pau;
    if (!coop_au_parse(pane, pane_len, &pau) || pau.slice[0] == NULL || pau.slice_len[0] == 0) {
        coop_hh_cu_why[3]++; return 0;
    }
    /* An IRAP peer pane cannot pair with our all-skip top half: every VCL NAL of one picture
     * must share nal_unit_type, and skip carries no intra. Let the normal path have it. */
    const int pt = (pau.slice[0][0] >> 1) & 0x3F;
    if (pt >= 16 && pt <= 23) {
        coop_hh_cu_why[4]++; return 0;
    }
    static const unsigned char sc[4] = {0, 0, 0, 1};
    size_t o = 0;
    memcpy(out, coop_hh_nonvcl, coop_hh_nonvcl_len);
    o = coop_hh_nonvcl_len;

    /* TOP -- ours, concealed, kept at address 0.
     *
     * BUILT OVER OUR PANE'S PARAMETERS, NOT THE COMPOSITE'S, and that is a correctness fix
     * rather than tidying. coop_build_skip_slice() codes CTBs from start_ctb to the END OF THE
     * PICTURE the parameters describe, so over the composite (8160 CTBs) from 0 it emits a
     * slice spanning BOTH halves -- and the peer's slice below then claims an address inside
     * it. The pane's parameters give exactly the 4080 CTBs of the top half, which is what the
     * tile path does for the same reason. Never exercised before because catch-up could not
     * fire at all without the composite owning its POC. */
    size_t sn = coop_build_skip_slice(&coop_hh_src, mine, mine_len, 0,
                                      coop_skip_scratch, sizeof(coop_skip_scratch),
                                      coop_vt_skip_pane, sizeof(coop_vt_skip_pane));
    if (sn == 0) {
        coop_hh_cu_why[5]++; return 0;
    }
    size_t rn = coop_poc_rewrite(&coop_hh_src, coop_vt_skip_pane, sn, poc,
                                 coop_poc_buf, coop_poc_cap);
    const unsigned char *top = (rn > 0) ? coop_poc_buf : coop_vt_skip_pane;
    size_t topn = (rn > 0) ? rn : sn;
    /* TILES: re-home our concealed half into tile 0. Retime FIRST and re-home second --
     * coop_poc_rewrite() walks with the pane's parameters, and once the header is tiled those
     * no longer describe it. Same order the paired path uses. */
    if (coop_hh_tiles) {
        size_t tn = coop_slice_to_tile(&coop_hh_src, &coop_hh_params, top, topn, 0,
                                       coop_skip_scratch, sizeof(coop_skip_scratch));
        if (tn == 0) { coop_hh_cu_why[6]++; return 0; }
        top = coop_skip_scratch;
        topn = tn;
    }
    if (o + 4 + topn > out_cap) {
        coop_hh_cu_why[6]++; return 0;
    }
    memcpy(out + o, sc, 4); o += 4;
    memcpy(out + o, top, topn); o += topn;

    /* BOTTOM -- the peer's pane, retimed then relocated to the bottom half. Retimed against
     * the PEER's parameters and re-addressed FROM them INTO the composite's, which is what the
     * two-parameter form of coop_slice_readdress() is for; passing the composite as both is
     * only harmless while the two hosts happen to agree. */
    size_t pn = coop_poc_rewrite(&coop_hh_peer_params, pau.slice[0], pau.slice_len[0], poc,
                                 coop_conv_buf, coop_conv_cap);
    const unsigned char *pb = (pn > 0) ? coop_conv_buf : pau.slice[0];
    const size_t pbn = (pn > 0) ? pn : pau.slice_len[0];
    /* set_address, NOT readdress -- and this is what blocked catch-up on horizontal from the
     * day it was written. coop_slice_readdress() REPLACES an existing slice_segment_address,
     * which only a non-first slice of a multi-slice pane has. Horizontal's peer pane is ONE
     * slice with first_slice_segment_in_pic_flag = 1 and therefore NO address field at all, so
     * readdress refused every single call -- measured why[7]=2652 of 2746 attempts, i.e. every
     * attempt that got that far. coop_slice_set_address() INSERTS the field and clears the
     * first-slice flag, which is exactly what the paired path has always used successfully
     * (coop_au_splice_halves). The tile path needs readdress because its panes really do carry
     * per-row addresses; horizontal never did. */
    /* TILES takes the OTHER branch on purpose: coop_slice_set_address() walks the header with
     * the composite's parameters, and slice_header_end_bit_x() refuses those outright once
     * tiles_enabled is set -- so in tiles mode this call could only ever return 0. Re-home
     * from the PEER's parameters into the composite's, as the paired tile path does. */
    size_t bn = coop_hh_tiles
              ? coop_slice_to_tile(&coop_hh_peer_params, &coop_hh_params, pb, pbn, coop_hh_addr,
                                   coop_vt_skip_tile, sizeof(coop_vt_skip_tile))
              : coop_slice_set_address(&coop_hh_params, pb, pbn, coop_hh_addr,
                                       coop_vt_skip_tile, sizeof(coop_vt_skip_tile));
    if (bn == 0) { coop_hh_cu_why[7]++; return 0; }
    if (o + 4 + bn > out_cap) { coop_hh_cu_why[8]++; return 0; }
    memcpy(out + o, sc, 4); o += 4;
    memcpy(out + o, coop_vt_skip_tile, bn); o += bn;
    return o;
}

static size_t coop_vt_build_catchup(const unsigned char *mine, size_t mine_len,
                                    const unsigned char *pane, size_t pane_len,
                                    unsigned poc, unsigned char *out, size_t out_cap) {
    if (mine == NULL || mine_len == 0 || pane == NULL || pane_len == 0) {
        return 0;
    }
    if (coop_vt_nonvcl_len == 0 || coop_vt_nonvcl_len > out_cap) {
        return 0;
    }
    coop_au_t pau;
    if (!coop_au_parse(pane, pane_len, &pau) || pau.slice[0] == NULL) {
        return 0;
    }
    /* Never against an IRAP of the peer's: our concealed tile is all-skip and carries no intra,
     * so the picture would mix nal_unit_types. Let the normal path handle those. */
    const int pt = (pau.slice[0][0] >> 1) & 0x3F;
    if (pt >= 16 && pt <= 23) {
        return 0;
    }
    size_t o = 0;
    memcpy(out, coop_vt_nonvcl, coop_vt_nonvcl_len);
    o = coop_vt_nonvcl_len;
    static const unsigned char sc[4] = {0, 0, 0, 1};

    /* TILE 0 -- the primary, concealed. Skip data over our own header, re-timed to the
     * composite's POC, then re-homed to column 0. */
    size_t sn = coop_build_skip_slice(&coop_vt_src, mine, mine_len, 0,
                                      coop_skip_scratch, sizeof(coop_skip_scratch),
                                      coop_vt_skip_pane, sizeof(coop_vt_skip_pane));
    if (sn == 0) {
        return 0;                     /* refuses on I slices -- correct, caller falls through */
    }
    size_t pn = coop_poc_rewrite(&coop_vt_src, coop_vt_skip_pane, sn, poc,
                                 coop_poc_buf, coop_poc_cap);
    const unsigned char *t0 = (pn > 0) ? coop_poc_buf : coop_vt_skip_pane;
    const size_t t0n = (pn > 0) ? pn : sn;
    size_t tl = coop_slice_to_tile(&coop_vt_src, &coop_vt_comp, t0, t0n, 0,
                                   coop_vt_skip_tile, sizeof(coop_vt_skip_tile));
    if (tl == 0 || o + 4 + tl > out_cap) {
        return 0;
    }
    memcpy(out + o, sc, 4); o += 4;
    memcpy(out + o, coop_vt_skip_tile, tl); o += tl;

    /* TILE 1 -- the peer's pane, walked with the PEER's parameters, re-timed to the same POC
     * and re-homed to column w_ctbs. */
    for (size_t i = 0; i < pau.slice_count; ++i) {
        const unsigned char *sl = pau.slice[i];
        const size_t sll = pau.slice_len[i];
        if (sl == NULL || sll == 0) {
            return 0;
        }
        bool first = false;
        unsigned addr = 0;
        if (!coop_slice_address(&coop_vt_peer_params, sl, sll, &first, &addr)) {
            return 0;
        }
        size_t rn = coop_poc_rewrite(&coop_vt_peer_params, sl, sll, poc,
                                     coop_poc_buf, coop_poc_cap);
        const unsigned char *src = (rn > 0) ? coop_poc_buf : sl;
        const size_t srcn = (rn > 0) ? rn : sll;
        const unsigned prow = addr / coop_vt_peer_params.w_ctbs;
        const unsigned pcol = addr % coop_vt_peer_params.w_ctbs;
        const unsigned want = prow * coop_vt_comp.w_ctbs + coop_vt_src.w_ctbs + pcol;
        size_t pl = coop_slice_to_tile(&coop_vt_peer_params, &coop_vt_comp, src, srcn, want,
                                       coop_vt_skip_tile, sizeof(coop_vt_skip_tile));
        if (pl == 0 || o + 4 + pl > out_cap) {
            return 0;
        }
#ifdef COOP_PC_DECODER
        if (pl > 256u * 1024u) { coop_vt_cu_big++; }
#endif
        memcpy(out + o, sc, 4); o += 4;
        memcpy(out + o, coop_vt_skip_tile, pl); o += pl;
    }
    return o;
}

/* Diagnostic log — written to /tmp/aurora-diag.log, pull with ares-pull after failure */
static unsigned int diag_feed_ok = 0;
static unsigned int diag_idr_sent = 0;
static unsigned int diag_idr_throttled = 0;
static unsigned int diag_feed_errors = 0;
static unsigned long diag_last_flush_ms = 0;
/* Bytes fed since the last [LATENCY] line, so the delivered bitrate is measured over the
 * same window rather than inferred from a single AU's size. */
static uint64_t diag_bytes_since_flush = 0;

/* FEED ACCOUNTING — every SS4S_PlayerVideoFeed in this file goes through diag_feed().
 *
 * WHY THIS EXISTS. Three of the four feed sites DISCARDED their return value (the VT, AVC
 * and HH catch-up paths); only the ordinary path at the bottom of vdec_delegate_submit()
 * ever looked at it. So `errors=0` has only ever described the ordinary path, and every
 * verdict of the form "the decoder accepted every picture" rests on a counter that never
 * saw three quarters of the feed sites. Found independently twice: memory
 * `coop-avc-catchup-bypasses-chain-break` (2026-09-09) and passover 02 (2026-09-10).
 *
 * THIS IS AN OBSERVATION-ONLY CHANGE. It deliberately does NOT act on a rejection at the
 * catch-up sites — the callers behave exactly as before, including advancing the donors and
 * the reference state after a feed the decoder may have refused. Acting on it is the
 * transactional-reference-state work (passover 02 §F2), which must not be designed until we
 * know whether rejections actually occur. Changing behaviour here would destroy the very
 * measurement this build exists to take.
 *
 * `diag_feed_bytes` counts what was ACTUALLY FED at each site. The old
 * `diag_bytes_since_flush += decodeUnit->fullLength` counted the primary AU only, so in
 * co-op the delivered-bitrate figure omitted the composite's peer half entirely. */
typedef enum {
    FEED_SITE_ORDINARY = 0,   /* vdec_delegate_submit(), the only site that read its result */
    FEED_SITE_VT_CATCHUP,     /* vertical co-op catch-up */
    FEED_SITE_HH_CATCHUP,     /* horizontal co-op catch-up */
    FEED_SITE__COUNT
} feed_site_t;
static const char *const feed_site_name[FEED_SITE__COUNT] = {"ord", "vtcu", "hhcu"};
static unsigned diag_feed_n[FEED_SITE__COUNT];
static unsigned diag_feed_notready[FEED_SITE__COUNT];
static unsigned diag_feed_reqkey[FEED_SITE__COUNT];
static unsigned diag_feed_err[FEED_SITE__COUNT];
static uint64_t diag_feed_bytes[FEED_SITE__COUNT];
/* A rejection has never been observed at a catch-up site because nobody has ever looked.
 * Log the first few verbatim so the first one is unmissable, then fall back to counters. */
#define DIAG_FEED_VERBOSE_REJECTS 20u
static unsigned diag_feed_reject_logged = 0;

/* FAULT INJECTION FOR THE F2 TEST -- `/tmp/coop-feed-fail` holds "<site> <every-nth>".
 *
 * WHY THIS EXISTS. Passover 02 (F2) proposes making composition transactional so the reference
 * state cannot advance past a feed the decoder refused. The feed accounting added for F1 then
 * measured that the trigger NEVER FIRES in the wild: 0 rejections in 1,131 catch-up feeds, and
 * the only 5 ever seen were at the ordinary site, which already gates its state updates on the
 * result. A proposal whose premise never occurs cannot be tested by waiting -- so this forces
 * the condition, which is exactly the criterion F2 set for itself ("a mocked feed rejection ...
 * produces no committed missing reference state").
 *
 * DIAGNOSTIC ONLY, AND OFF UNLESS THE FILE EXISTS. It fakes the RESULT while still feeding the
 * decoder, so the picture keeps its own truth and only the accounting path is exercised. */
static feed_site_t diag_fail_site = FEED_SITE__COUNT;
static unsigned diag_fail_every = 0;
static unsigned diag_fail_fired = 0;

static void diag_feed_fault_arm(void) {
    diag_fail_site = FEED_SITE__COUNT;
    diag_fail_every = 0;
    diag_fail_fired = 0;
    FILE *f = fopen("/tmp/coop-feed-fail", "r");
    if (f == NULL) { return; }
    char site[16] = {0};
    unsigned every = 0;
    if (fscanf(f, "%15s %u", site, &every) == 2 && every > 0) {
        for (int i = 0; i < FEED_SITE__COUNT; ++i) {
            if (strcmp(site, feed_site_name[i]) == 0) {
                diag_fail_site = (feed_site_t) i;
                diag_fail_every = every;
            }
        }
    }
    fclose(f);
    if (diag_log != NULL && diag_fail_every > 0) {
        fprintf(diag_log, "[FEED-FAULT] ARMED: site=%s every=%u -- results are FAKED at that"
                          " site; this run is for the F2 test only\n",
                feed_site_name[diag_fail_site], diag_fail_every);
        fflush(diag_log);
    }
}

static void coop_trace_emit(SS4S_VideoFeedResult r, size_t len,
                            unsigned long enter, unsigned long exit);

static SS4S_VideoFeedResult diag_feed(feed_site_t site, const unsigned char *buf, size_t len,
                                      SS4S_VideoFeedFlags flags) {
    /* FEED ENTRY AND EXIT, ON THE SAME MONOTONIC CLOCK AS EVERY OTHER STAMP IN THE TRACE.
     * Passover 07 §3.2 asks for both, not just the result: a feed that takes 300 ms to return
     * and one that returns instantly are indistinguishable in every counter we had, and the
     * first is downstream backpressure the [LATENCY] line's sub_ms cannot see (sub_ms stops
     * at the START of the feed). Cheap enough to take unconditionally -- two clock_gettime
     * calls -- but taken only while tracing, so the ordinary path is unchanged. */
    const bool tracing = coop_trace_active();
    const unsigned long fe = tracing ? coop_trace_now_ms() : 0ul;
    /* CATCH-UP PICTURES BELONG IN THE /tmp/coop-dump-out CAPTURE TOO. It is written at the
     * ordinary feed, which never sees a catch-up picture, so every catch-up left a hole and an
     * offline decode of the capture could only report "Could not find ref with POC" -- measured
     * 2026-09-23, 359 of 400 pictures failing on a stream the decoder had actually accepted.
     * Every catch-up is fed here, before its call's ordinary picture, so file order is feed order. */
    if (site != FEED_SITE_ORDINARY && coop_dump_out != NULL && coop_dump_out_open &&
        coop_dump_out_frames < 400) {
        fwrite(buf, 1, len, coop_dump_out);
        if (++coop_dump_out_frames == 400) {
            fflush(coop_dump_out); fclose(coop_dump_out); coop_dump_out = NULL;
        }
    }
    avsync_probe_video(len);
#if TARGET_ANDROID
    if (coop_live || coop_vt_enabled) {
        static unsigned char stripped[DECODER_BUFFER_MAX_SIZE];
        unsigned words = 0;
        const size_t got = coop_au_strip_zero_words(buf, len, stripped, sizeof(stripped), &words);
        if (got > 0) {
            buf = stripped;
            len = got;
            if (coop_zw_aus++ == 0 && diag_log != NULL) {
                fprintf(diag_log, "[COOP] removing cabac_zero_words before a slice (%u in the first picture): "
                                  "the decoder skips a slice after them\n", words);
                fflush(diag_log);
            }
            coop_zw_words += words;
        }
    }
#endif
    SS4S_VideoFeedResult r = SS4S_PlayerVideoFeed(player, buf, len, flags);
    if (tracing) { coop_trace_emit(r, len, fe, coop_trace_now_ms()); }
    if (diag_fail_every > 0 && site == diag_fail_site) {
        diag_fail_fired++;
        if (diag_fail_fired % diag_fail_every == 0) {
            r = SS4S_VIDEO_FEED_ERROR;
        }
    }
    diag_feed_n[site]++;
    diag_feed_bytes[site] += (uint64_t) len;
    diag_bytes_since_flush += (uint64_t) len;
    switch (r) {
        case SS4S_VIDEO_FEED_NOT_READY:        diag_feed_notready[site]++; break;
        case SS4S_VIDEO_FEED_REQUEST_KEYFRAME: diag_feed_reqkey[site]++;   break;
        case SS4S_VIDEO_FEED_ERROR:            diag_feed_err[site]++;      break;
        default: return r;
    }
    if (diag_log != NULL && diag_feed_reject_logged < DIAG_FEED_VERBOSE_REJECTS) {
        diag_feed_reject_logged++;
        fprintf(diag_log, "[FEED-REJECT] site=%s result=%d len=%zu n=%u\n",
                feed_site_name[site], (int) r, len, diag_feed_n[site]);
        fflush(diag_log);
    }
    return r;
}

/* Complete and write the armed row. A feed with NOTHING armed is still traced -- that is the
 * solo/HEVC path, or a co-op picture that reached the decoder without going through the AVC
 * decision at all, and silently dropping those would make the trace disagree with [FEED]. */
static void coop_trace_emit(SS4S_VideoFeedResult r, size_t len,
                            unsigned long enter, unsigned long exit) {
    if (!coop_trace_pending_armed) {
        memset(&coop_trace_pending, 0, sizeof(coop_trace_pending));
        coop_trace_pending.kind = COOP_TRACE_FRESH;
        coop_trace_pending.prim_frame = coop_trace_prim_frame;
        coop_trace_pending.prim_pts_ms = coop_trace_prim_pts;
        coop_trace_pending.prim_recv_ms = coop_trace_prim_recv;
        coop_trace_pending.prim_reasm_ms = coop_trace_prim_reasm;
        coop_trace_pending.prim_host_10us = coop_trace_prim_host;
    }
    coop_trace_pending.feed_enter_mono_ms = enter;
    coop_trace_pending.feed_exit_mono_ms = exit;
    coop_trace_pending.feed_result = (int) r;
    coop_trace_pending.bytes = (uint32_t) len;
    coop_trace_pic(&coop_trace_pending);
    coop_trace_pending_armed = false;
}
static unsigned long diag_last_snmp_ms = 0;
static unsigned int diag_session = 0;
static int vdec_stream_format = 0;
static bool vdec_warned_near_buffer_limit;
VIDEO_STATS vdec_summary_stats;
VIDEO_INFO vdec_stream_info;

static void diag_snmp_snap(unsigned long ms) {
    if (!diag_log) return;
    FILE *f = fopen("/proc/net/snmp", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0]=='U' && line[1]=='d' && line[2]=='p' && line[3]==':' &&
            line[5]>='0' && line[5]<='9') {
            fprintf(diag_log, "UDP@%lu %s", ms, line);
            break;
        }
    }
    fclose(f);
}

/* Radio signal level + wifi-layer drop counters — a strong level with rising
 * drops indicates interference/receiver overload rather than a weak link. */
static void diag_wifi_snap(const char *tag, unsigned long ms) {
    if (!diag_log) return;
    FILE *f = fopen("/proc/net/wireless", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        const char *p = line;
        while (*p == ' ') p++;
        if (strncmp(p, "wlan0:", 6) == 0) {
            fprintf(diag_log, "%s@%lu %s", tag, ms, p);
            break;
        }
    }
    fclose(f);
}

static int vdec_delegate_setup(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags);

static void vdec_delegate_cleanup();

static int vdec_delegate_submit(PDECODE_UNIT decodeUnit);

static void vdec_stat_submit(const struct VIDEO_STATS *src, unsigned long now);

/* PER-STREAM TOTALS FOR THE KEPT LOG (support logs, 2026-09-30). The overlay's windows are thrown away as they
 * are drawn, so a user's log said nothing about how a stream actually went. While a kept log is active: one
 * "Stats" line every ~10 s (only while frames arrive), and vdec_stats_log_totals() at the end of the stream. */
static struct {
    unsigned long start_ms, first_frame_ms, last_log_ms;
    uint64_t frames, received, dropped, submitted, bytes;
    uint64_t logged_frames, logged_dropped;   /* at the previous Stats line: each line covers the time since it */
    uint32_t rtt_last, rtt_var_last, rtt_max;
    double latency_sum_ms;
    uint32_t latency_samples;
    float worst_fps;
} vdec_totals;

static void stream_info_parse_size(PDECODE_UNIT decodeUnit, struct VIDEO_INFO *info);

#if HAVE_STARFISH_DIRECT
static void coop_mirror_feed(const unsigned char *data, size_t size, bool is_idr);
static void coop_mirror_teardown(void);
#endif

static size_t vdec_buffer_initial_bytes(void) {
    return (size_t) VDEC_REASSEMBLY_BUFFER_MB * 1024U * 1024U;
}

DECODER_RENDERER_CALLBACKS ss4s_dec_callbacks = {
        .setup = vdec_delegate_setup,
        .cleanup = vdec_delegate_cleanup,
        .submitDecodeUnit = vdec_delegate_submit,
        .capabilities = CAPABILITY_DIRECT_SUBMIT,
};

void session_video_prepare_stream(void) {
    int caps = CAPABILITY_DIRECT_SUBMIT;
    const bool hevc = app_configuration != NULL && app_configuration->hevc;
    const bool av1 = app_configuration != NULL && app_configuration->av1;
    /* REFERENCE COUNT IS *NOT* WHY SKIP CONCEALMENT FAILS -- TESTED AND DISPROVED
     * 2026-08-01, recorded so it is not tried again.
     *
     * The theory was: the all-skip concealment slice copies RefPicList0[0] with a zero MV
     * and assumes that is the previous picture, but SdpGenerator.c sends
     * x-nv-video[0].maxNumReferenceFrames = 0 ("host picks") whenever the decoder claims
     * RFI, and Apollo's set_ref_frames() defaults to five -- so the list head could be an
     * older picture. Dropping the RFI capability makes the client ask for exactly 1.
     *
     * It was done, and the diagnostic below confirmed the request went out
     * (caps=0x1000001 coop=1 RFI_hevc=0). THE COMPOSITE CAME BACK BYTE-IDENTICAL --
     * same md5 over 17MB, same pairing counters -- so the host's encode did not change at
     * all. Apollo already pins numRefL0 = NV_ENC_NUM_REF_FRAMES_1 regardless; the DPB size
     * is only a fallback pool for reference invalidation. Note also that Apollo's "rfi"
     * suffix in "created encoder ..." tracks its own capability, NOT the client's request,
     * so it is useless as a check here.
     *
     * So RFI stays advertised: it earns its keep on loss recovery and there is no measured
     * benefit to dropping it. The concealer's real fault is in the slice itself -- see the
     * verification block in coop_conceal.c for the isolated test and its target number. */
    const bool coop = app_launch_coop_requested();
    /* CO-OP ASKS FOR ONE REFERENCE FRAME, and this flag is what decides it.
     *
     * Concealment for a picture with no peer pane is an all-skip slice whose every CU copies
     * RefPicList0[0] with a zero motion vector -- its whole design is "that is the previous
     * picture". SdpGenerator.c sends x-nv-video[0].maxNumReferenceFrames = 0 ("host picks")
     * whenever the decoder claims RFI, and Apollo's set_ref_frames() defaults to FIVE, so the
     * RPS can make an OLDER picture the list head and the "copy" reproduces a frame from
     * several back.
     *
     * THIS WAS "DISPROVED" ONCE AND THE DISPROOF WAS VOID. The test that showed a
     * byte-identical composite ran while /tmp/coop-playfile was armed, so the client was
     * replaying a RECORDING and no host-side flag could have changed anything (see bff55b19).
     * Re-tested properly, the concealed picture's error is UNIFORM across every CTB row
     * (22-23 dB, row 0 through row 33) with no positional structure -- which is what copying
     * the wrong reference everywhere looks like, and not what a boundary or parse fault
     * looks like.
     *
     * Only for co-op: an ordinary stream wants RFI for loss recovery and multiple references
     * for efficiency. Nothing here is host state, so there is nothing to revert. */
    if (hevc && !coop) {
        caps |= CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC;
    }
    if (av1 && !coop) {
        caps |= CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1;
    }
    /* SLICES-PER-FRAME IS NOT AN HEVC FEATURE, AND EXCLUDING H.264 HERE IS WHAT FORCED THE
     * ENTIRE FRAGILE AVC PATH. CAPABILITY_SLICES_PER_FRAME is a PROTOCOL capability, not a
     * codec one: Apollo feeds it straight to ctx->slices (video.cpp:1698) and to NVENC's
     * sliceModeData (nvenc_base.cpp:267), both codec-agnostic, and h264_amf maps the same
     * AVCodecContext field onto AMF's slices-per-frame.
     *
     * While this was HEVC-only the client could never ASK for two slices in H.264, so the AVC
     * co-op path had no choice but half-height SINGLE-slice encodes re-homed by rewriting the
     * slice header (first_mb_in_slice, frame_num width, QP rebase, CABAC realignment). The
     * PROVEN path does none of that -- coop_au_splice() is a verbatim memcpy of A's parameter
     * sets, A's slice 0 and B's slice 1 -- and it needs exactly ONE thing: two real slices from
     * each host. That is what this now requests. H.264 is the only other codec here, so the
     * condition goes away entirely rather than growing a third term. */
    {
        unsigned slices = VDEC_STREAM_SLICES_PER_FRAME;
        FILE *sf = fopen("/tmp/coop-slices", "r");
        if (sf != NULL) {
            unsigned v = 0;
            if (fscanf(sf, "%u", &v) == 1 && v >= 1 && v <= 8) { slices = v; }
            fclose(sf);
        }
        caps |= CAPABILITY_SLICES_PER_FRAME(slices);
    }
    if (hevc || av1) {
        commons_log_info("Session", "Video SDP caps: %s+ %u slices/frame (HEVC=%d AV1=%d)",
                         coop ? "" : "RFI ",
                         (unsigned) VDEC_STREAM_SLICES_PER_FRAME, hevc ? 1 : 0, av1 ? 1 : 0);
    } else {
        commons_log_info("Session", "Video SDP caps: direct submit only (H.264)");
    }
    /* INTO THE DIAG LOG, not just commons_log. Whether RFI is advertised decides how many
     * reference frames the host uses, which decides whether concealment can be exact -- and
     * the only other place that is visible is Apollo's "rfi" suffix on the far host. When
     * that suffix did not change, "the flag was still set" and "the new build was not
     * running" were indistinguishable, and commons_log_info does not reach the file this
     * project reads. One line settles it. */
    {
        FILE *lf = fopen(AURORA_DIAG_LOG, "a");
        if (lf != NULL) {
            fprintf(lf, "[COOP] SDP caps=0x%x coop=%d RFI_hevc=%d (co-op asks the host for "
                        "ONE reference frame; multi-ref breaks skip concealment)\n",
                    (unsigned) caps, (int) coop,
                    (caps & CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC) ? 1 : 0);
            fclose(lf);
        }
    }
    ss4s_dec_callbacks.capabilities = caps;
}

static const char *video_format_name(int videoFormat) {
    switch (videoFormat) {
        case VIDEO_FORMAT_H264:
            return "H264";
        case VIDEO_FORMAT_H265:
            return "H265";
        case VIDEO_FORMAT_H265_MAIN10:
            return "H265 10bit";
        case VIDEO_FORMAT_AV1_MAIN8:
            return "AV1 8bit";
        case VIDEO_FORMAT_AV1_MAIN10:
            return "AV1 10bit";
        default:
            if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
                return "AV1";
            }
            return "Unknown";
    }
}

/* Co-op Spike #1 (docs/coop-split-DESIGN.md): can the G5 grant a SECOND video
 * decoder while the primary stream is decoding? Gated behind /tmp/coop-probe so
 * it NEVER runs in normal use (enable: `touch /tmp/coop-probe` over SSH, start a
 * stream, read [COOP-PROBE] in /tmp/aurora-diag.log, then `rm /tmp/coop-probe`).
 *
 * The 2nd player is VIDEO-ONLY, so SS4S_PlayerVideoOpen triggers the SMP Load
 * (the umediaserver decoder-grant point) immediately -- smp_video.c defers Load
 * only when audio is configured -- so the open result IS the grant result; no
 * frame feed needed. Briefly contends the decoder with the live stream; that is
 * deliberate and user-triggered.
 *
 * CAVEAT: this is a same-appId, same-process test. If it DENIES, the two-process
 * design likely needs a second appId (see the design doc). If it GRANTS, dual
 * decode is viable and possibly even simpler than two apps. */
/* SPIKE #1, done properly: can a CHILD PROCESS of Eclipse get its own NDL media
 * pipeline while this process is streaming?
 *
 * Everything before this failed to ask the question. In-process second players
 * only re-load the one process-global pipeline (ss4s ndl/webos5 keeps a single
 * ActivatePlayerContext), and a separate installed app is refused outright --
 * measured: NDL_DirectMediaInit succeeds but NDL_DirectMediaLoad returns -1 even
 * with nothing else running, because a bare native app has no foreground surface.
 * A child of Eclipse inherits the app container, appId and foreground surface,
 * and "one app, a coordinator plus a secondary process" is what the co-op design
 * actually calls for.
 *
 * Spawned DETACHED and delayed: never block this path (a 5s block here killed the
 * stream once already), and let the primary settle first so a denial cannot be
 * blamed on racing its setup. Results land in /tmp/coop_ndl_probe.log.
 */
static void coop_child_process_probe(void) {
#if defined(AURORA_TEST_HOOKS)
    if (access("/tmp/coop-probe-proc", F_OK) != 0 || diag_log == NULL) {
        return;
    }
    if (access("/tmp/ndl_pipeline_probe", X_OK) != 0) {
        fprintf(diag_log, "[COOP-PROBE run=%ld] child: /tmp/ndl_pipeline_probe missing or not executable\n",
                (long) time(NULL));
        fflush(diag_log);
        return;
    }
    fprintf(diag_log, "[COOP-PROBE run=%ld] child: spawning NDL probe (detached, +6s); results in /tmp/coop_child_probe.log\n",
            (long) time(NULL));
    fflush(diag_log);
    int rc = system("sh -c 'sleep 6; /tmp/ndl_pipeline_probe 0 /tmp/coop_child_probe.log >>/tmp/coop_child_probe.log 2>&1' &");
    (void) rc;
#endif
}

/* Co-op gate #1, attacked at the layer that actually gates it: EXPORTED WINDOWS.
 *
 * A starfish media pipeline is bound to a windowId from SDL_webOSCreateExportedWindow
 * (smp_resource_webos5.c:33). That is why a bare second process failed with "no video
 * device" — no exported window, so no pipeline. And a live Multi-View session is two
 * windows, so the system clearly grants two.
 *
 * Everything else is downstream of this question: if a second exported window can be
 * created in-process, a second pipeline can plausibly bind to it, and
 * SDL_webOSSetExportedWindow positions each one INDEPENDENTLY — which also fixes the
 * process-global NDL_DirectVideoSetArea limitation.
 *
 * Needs no starfish linkage, so it is a cheap gate before any pipeline work.
 * Sentinel /tmp/coop-probe-win.
 */
/* MOVED OUT OF #if HAVE_STARFISH_DIRECT. It is a monotonic clock and nothing more, but it
 * was declared and defined inside that block while coop_emit_rate() -- which is outside it
 * -- calls it. On webOS the block is always on so this never showed; the first build
 * without starfish failed on an implicit declaration. */
static uint64_t coop_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t) t.tv_sec * 1000000000ull + (uint64_t) t.tv_nsec;
}

static const char *coop_win_probe_window = NULL;

static void coop_exported_window_probe(void) {
#ifndef TARGET_WEBOS
    /* A DIAGNOSTIC, NOT A FEATURE. This probes whether webOS hands out a second video
     * plane, via SDL_webOSCreateExportedWindow/SetExportedWindow. There is no exported
     * window and no video plane to steal on any other platform, so the honest port is to
     * do nothing rather than fake a result the caller would log. */
    return;
#else
    if (access("/tmp/coop-probe-win", F_OK) != 0 || diag_log == NULL) {
        return;
    }
    if (coop_win_probe_window != NULL) {
        return;   /* one only, ever — no leaks */
    }
    long run = (long) time(NULL);
    const char *wid = SDL_webOSCreateExportedWindow(SDL_WEBOS_EXPORTED_WINDOW_TYPE_VIDEO);
    fprintf(diag_log, "[COOP-PROBE run=%ld] plane-steal test: window=%s (NO pipeline loaded)\n",
            run, wid ? wid : "(NULL)");
    fflush(diag_log);
    if (wid == NULL) {
        return;
    }
    coop_win_probe_window = wid;
    SDL_Rect src = {0, 0, 1920, 1080}, dst = {960, 0, 960, 1080};
    SDL_bool ok = SDL_webOSSetExportedWindow(wid, &src, &dst);
    fprintf(diag_log,
            "[COOP-PROBE run=%ld] plane-steal test: positioned RIGHT -> %d. If the PRIMARY "
            "picture moves right, the single plane was stolen (not a 2nd plane).\n", run, (int) ok);
    fflush(diag_log);
#endif
}

#if HAVE_STARFISH_DIRECT
#include "StarfishMediaAPIs_C.h"

/* window id from our own wl_webos_foreign export (see the wayland probe) */
static char coop_wl_window_id[64];

/* THE LAST UNTRIED DISPLAY ROUTE. Everything so far EXPORTED a window and hoped the
 * compositor would show it; the pipeline then decoded, reported RENDERED_FRAME, and put
 * nothing on screen. wl_webos_imported is the other direction: IMPORT that window id and
 * `attach_punchthrough`, which is the compositor's own way of cutting a hole in OUR surface
 * for the video to show through. That matters because the standing explanation for the black
 * pane is exactly this -- an exported window is a punch-through region in the app's graphics
 * surface, and Eclipse presents no surface with a hole while streaming. Defined further down,
 * where the Wayland headers are in scope. Gated on /tmp/coop-probe-import. */
static void coop_import_punchthrough(const char *wid);

/* videooutput/getStatus -- which sink owns which plane. Gated on /tmp/coop-probe-vo. */
static void coop_videooutput_probe(const char *mediaId);

/* Snapshot the videooutput clients BEFORE the 2nd pipeline exists, so it can be identified. */
static void coop_videooutput_snapshot(void);
#include <glib.h>

/* Mirror-mode state: a second, independent starfish pipeline kept alive and fed
 * the same frames as the primary, to prove two live decoders visually. */
static StarfishMediaAPIs_C *coop_mirror_api = NULL;
static const char *coop_mirror_window = NULL;
static uint64_t coop_mirror_open_ns = 0;
static uint64_t coop_pts_base_ns = 0;   /* set at first fed frame */
static unsigned coop_mirror_fed = 0;


static volatile int coop_sf_loadcompleted = 0;
static volatile int coop_sf_events = 0;

static void coop_starfish_load_cb(int type, int64_t numValue, const char *strValue, void *data) {
    (void) data;
    coop_sf_events++;
    if (type == STARFISH_EVENT_STR_STATE_UPDATE_LOADCOMPLETED) {
        coop_sf_loadcompleted = 1;
    }
    if (diag_log != NULL) {
        fprintf(diag_log, "[COOP-PROBE]   starfish cb type=0x%x num=%lld str=%s\n", type,
                (long long) numValue, strValue ? strValue : "");
        fflush(diag_log);
    }
}

/* THE decisive test for dual decode. The primary plane is running on SS4S/NDL.
 * Here we bind a SECOND, independent starfish pipeline to its OWN exported window,
 * bypassing SS4S entirely (SS4S has one selected driver per process, and NDL's
 * single global context is what made every earlier in-process attempt meaningless).
 *
 * Payload mirrors smp_player.c MakeLoadPayload: BUFFERSTREAM transport, RAW/Chrome
 * external streaming info, WEBRTC contentsType for low latency, and crucially the
 * windowId of our second exported window.
 *
 * Sentinel /tmp/coop-probe-sf.
 */
static void coop_starfish_second_pipeline_probe(SS4S_VideoCodec codec) {
    if (access("/tmp/coop-probe-sf", F_OK) != 0 || diag_log == NULL) {
        return;
    }
    long run = (long) time(NULL);
    const char *appId = getenv("APPID");
    /* Prefer the window WE exported over the Wayland protocol: that path does a
     * real wl_surface_commit, which SDL's wrapper appears not to do -- and an
     * uncommitted surface change is never live, which would explain windows that
     * got ids yet never composited. */
    const char *wid = coop_wl_window_id[0] != '\0' ? coop_wl_window_id : NULL;
    const bool wid_direct = wid != NULL;
    if (wid == NULL) {
        wid = SDL_webOSCreateExportedWindow(SDL_WEBOS_EXPORTED_WINDOW_TYPE_VIDEO);
    }
    fprintf(diag_log, "[COOP-PROBE run=%ld] starfish: appId=%s window=%s (%s)\n", run,
            appId ? appId : "(null)", wid ? wid : "(NULL)",
            wid_direct ? "direct wayland export" : "SDL wrapper");
    fflush(diag_log);
    if (wid == NULL) {
        return;
    }
    /* Place it in the right half so a successful decode is visually obvious. */
    SDL_Rect src = {0, 0, 1920, 1080}, dst = {960, 0, 960, 1080};
    SDL_webOSSetExportedWindow(wid, &src, &dst);

    coop_videooutput_snapshot();   /* must precede pipeline creation */
    StarfishMediaAPIs_C *api = StarfishMediaAPIs_create(NULL);
    if (api == NULL) {
        fprintf(diag_log, "[COOP-PROBE run=%ld] starfish: create FAILED\n", run);
        fflush(diag_log);
        return;
    }
    StarfishMediaAPIs_notifyForeground(api);

    /* Must match the stream we will actually feed. Hardcoding H265 while the
     * client was configured for H.264 meant the pipeline accepted every frame
     * ("Ok") and still showed nothing. */
    const char *codecName = "H265";
    if (codec == SS4S_VIDEO_H264) { codecName = "H264"; }
    else if (codec == SS4S_VIDEO_AV1) { codecName = "AV1"; }
    fprintf(diag_log, "[COOP-PROBE run=%ld] starfish: codec=%s\n", run, codecName);
    fflush(diag_log);

    /* videoDisplayMode: NEVER SET BY US, AND DOCUMENTED TO EXIST (webOS camera-pipeline
     * examples use "Textured"). It selects how the pipeline's output reaches the screen:
     * the default is an overlay/punch-through PLANE, and a plane must be OWNED -- our second
     * pipeline reports videoSinkType "main_video", the same sink the primary NDL pipeline
     * already owns, which is the best explanation yet for "decodes, reports RENDERED_FRAME,
     * shows nothing". "Textured" should instead composite into the graphics surface and
     * sidestep plane ownership entirely. Value comes from /tmp/coop-probe-displaymode so the
     * alternatives can be tried without a rebuild each time. */
    char dispmode[96] = "";
    FILE *dmf = fopen("/tmp/coop-probe-displaymode", "r");
    if (dmf != NULL) {
        char v[48] = "";
        if (fscanf(dmf, "%47s", v) == 1 && v[0] != '\0') {
            snprintf(dispmode, sizeof(dispmode), "\"videoDisplayMode\":\"%s\",", v);
            fprintf(diag_log, "[COOP-PROBE run=%ld] starfish: videoDisplayMode=%s\n", run, v);
        }
        fclose(dmf);
    }
    char payload[2048];
    snprintf(payload, sizeof(payload),
             "{\"args\":[{\"mediaTransportType\":\"BUFFERSTREAM\",\"option\":{"
             "%s\"appId\":\"%s\",\"windowId\":\"%s\","
             "\"externalStreamingInfo\":{"
             "\"contents\":{\"codec\":{\"video\":\"%s\"},"
             "\"esInfo\":{\"pauseAtDecodeTime\":true,\"ptsToDecode\":0,\"seperatedPTS\":true},"
             "\"format\":\"RAW\",\"provider\":\"Chrome\"},"
             "\"streamQualityInfo\":true,\"audioSync\":false,"
             "\"streamQualityInfoCorruptedFrame\":true,\"streamQualityInfoNonFlushable\":true,"
             "\"restartStreaming\":false,"
             "\"bufferingCtrInfo\":{\"bufferMaxLevel\":0,\"bufferMinLevel\":0,\"preBufferByte\":0,"
             "\"qBufferLevelAudio\":0,\"qBufferLevelVideo\":0,"
             "\"srcBufferLevelAudio\":{\"minimum\":1,\"maximum\":32768},"
             "\"srcBufferLevelVideo\":{\"minimum\":1,\"maximum\":1048576}}},"
             "\"transmission\":{\"contentsType\":\"WEBRTC\"},"
             "\"needAudio\":false,\"queryPosition\":false,\"lowDelayMode\":true,"
             "\"adaptiveStreaming\":{\"audioOnly\":false,\"maxWidth\":1920,\"maxHeight\":1080,"
             "\"maxFrameRate\":60.0}}}]}",
             dispmode, appId ? appId : APPID, wid, codecName);

    bool loaded = StarfishMediaAPIs_load(api, payload, coop_starfish_load_cb, NULL);
    fprintf(diag_log, "[COOP-PROBE run=%ld] starfish: load returned %d (NOT yet proof)\n", run,
            (int) loaded);
    fflush(diag_log);

    /* Pump a GLib main context so starfish can deliver its state callbacks. Without
     * this, LOADCOMPLETED never arrives, the pipeline never reaches PLAYING and every
     * feed is rejected -- which is what the previous run measured, and likely also why
     * smp-webos fails as the primary decoder here. */
    for (int i = 0; i < 300 && !coop_sf_loadcompleted; ++i) {
        while (g_main_context_iteration(NULL, FALSE)) { /* drain */ }
        SDL_Delay(10);
    }
    fprintf(diag_log, "[COOP-PROBE run=%ld] starfish: after pump events=%d loadcompleted=%d\n",
            run, coop_sf_events, coop_sf_loadcompleted);
    fflush(diag_log);

    if (coop_sf_loadcompleted) {
        /* ROUTE THE SINK BEFORE PLAY. The pipeline announces videoSinkType "main_video" at
         * load, so by the time it is playing and being fed it has already taken MAIN -- and
         * re-homing a running pipeline to SUB0 left SUB0 connected but blank. Connecting
         * between LOADCOMPLETED and play() is the one ordering that has not been tried. */
        coop_videooutput_probe(StarfishMediaAPIs_getMediaID(api));
        bool played = StarfishMediaAPIs_play(api);
        fprintf(diag_log, "[COOP-PROBE run=%ld] starfish: play -> %d\n", run, (int) played);
        /* Re-position AFTER load, as smp_resource_webos5.c StarfishResourcePostLoad
         * does. Setting it only before load is not enough -- loading attaches the
         * media to the window, which is why the pipeline decoded frames (FRAMEREADY)
         * while nothing appeared on screen. src must be the VIDEO rect. */
        /* /tmp/coop-probe-full => place the mirrored plane FULLSCREEN. If exported
         * windows composite at all, the duplicate stream must visibly cover the
         * primary. If the screen is unchanged, exported windows never display on
         * this firmware -- which would also explain why SMP (which depends on them
         * entirely) cannot feed a single frame as the primary decoder. */
        const bool coop_full = access("/tmp/coop-probe-full", F_OK) == 0;
        SDL_Rect psrc = {0, 0, 1920, 1080};
        SDL_Rect pdst = coop_full ? (SDL_Rect) {0, 0, 1920, 1080}
                                  : (SDL_Rect) {960, 0, 960, 1080};
        /* WHICH COORDINATE SPACE IS dst IN? The hardcoded rects above assume 1920x1080, but
         * the panel is 3840x2160 -- so a "right half" of {960,0,960,1080} may in fact land in
         * the upper-left QUADRANT of the panel, underneath the primary's plane, and read as
         * "the second pane never displays" when it is merely hidden. `/tmp/coop-probe-rect`
         * ("x y w h") places it explicitly so that question can be answered without a
         * rebuild per attempt. */
        FILE *rf = fopen("/tmp/coop-probe-rect", "r");
        if (rf != NULL) {
            int rx, ry, rw, rh;
            if (fscanf(rf, "%d %d %d %d", &rx, &ry, &rw, &rh) == 4 && rw > 0 && rh > 0) {
                pdst = (SDL_Rect) {rx, ry, rw, rh};
                fprintf(diag_log, "[COOP-PROBE run=%ld] starfish: dst from file = %d,%d %dx%d\n",
                        run, rx, ry, rw, rh);
            }
            fclose(rf);
        }
        SDL_bool rp = SDL_webOSSetExportedWindow(wid, &psrc, &pdst);
        fprintf(diag_log, "[COOP-PROBE run=%ld] starfish: re-position after load -> %d\n",
                run, (int) rp);
        fflush(diag_log);
        coop_import_punchthrough(wid);
    }

    if (access("/tmp/coop-probe-mirror", F_OK) == 0) {
        /* MIRROR MODE — the conclusive test. Keep this pipeline alive and feed it the
         * SAME decode units as the primary (see vdec_delegate_submit). Both halves
         * should then show the same game: the primary on SS4S/NDL in the left half,
         * this independent starfish pipeline in the right. A picture in the right
         * half is proof of two live decoders that no return code can fake. */
        coop_mirror_api = api;
        coop_mirror_window = wid;
        coop_mirror_open_ns = coop_now_ns();
        if (player != NULL && access("/tmp/coop-probe-full", F_OK) != 0) {
            /* PRIMARY GEOMETRY, SETTABLE AT RUN TIME (`/tmp/coop-probe-primary-rect`,
             * "x y w h"). Two reasons. First it is how a pane is made to FILL its half
             * instead of sitting letterboxed inside it. Second, and more useful right now:
             * mirror mode feeds both pipelines identical frames, so a screenshot cannot say
             * WHICH pipeline is on screen -- unless the two panes have visibly different
             * geometry. Give the primary a different shape and the ambiguity disappears. */
            SS4S_VideoRect left = {0, 0, 960, 1080};
            FILE *pf = fopen("/tmp/coop-probe-primary-rect", "r");
            if (pf != NULL) {
                int px, py, pw, ph;
                if (fscanf(pf, "%d %d %d %d", &px, &py, &pw, &ph) == 4 && pw > 0 && ph > 0) {
                    left = (SS4S_VideoRect) {px, py, pw, ph};
                    fprintf(diag_log, "[COOP-PROBE run=%ld] primary rect from file = %d,%d %dx%d\n",
                            run, px, py, pw, ph);
                }
                fclose(pf);
            }
            SS4S_PlayerVideoSetDisplayArea(player, NULL, &left);
        }
        fprintf(diag_log, "[COOP-PROBE run=%ld] starfish: MIRROR ACTIVE — primary=left, starfish=right\n", run);
        fflush(diag_log);
        return;
    }

    SDL_Delay(1500);
    StarfishMediaAPIs_unload(api);
    StarfishMediaAPIs_destroy(api);
    SDL_webOSDestroyExportedWindow(wid);
    fprintf(diag_log, "[COOP-PROBE run=%ld] starfish: cleaned up\n", run);
    fflush(diag_log);
}

/* Mirror feed — decoupled from the video submit path.
 *
 * Two hard rules learned the hard way:
 *  1. NEVER do pipeline work on the submit thread. Twice now a failing feed there
 *     stalled primary decode and produced a black screen + "unstable connection".
 *  2. NEVER hand starfish a pointer into the primary reassembly buffer: it is
 *     overwritten every frame, and we cannot assume the pipeline copies before
 *     returning. Own the memory.
 *
 * So: the submit thread only memcpys into a ring slot (dropping if full, never
 * blocking), and a dedicated thread does the feeding. PTS is NANOSECONDS relative
 * to load, matching StarfishPlayerGetTime(); the earlier probe passed microseconds,
 * a 1000x error, which is a strong candidate for the feed rejections.
 */
#define COOP_RING 8

typedef struct {
    unsigned char *data;
    size_t size, cap;
    int ready;
} coop_slot_t;

static coop_slot_t coop_ring[COOP_RING];
static volatile unsigned coop_w = 0, coop_r = 0;
static pthread_t coop_feed_thread;
static volatile int coop_feed_run = 0;
static volatile int coop_started = 0;   /* only start at an IDR */
static unsigned coop_dropped = 0;

static void *coop_feed_worker(void *arg) {
    (void) arg;
    while (coop_feed_run) {
        if (coop_r == coop_w) {
            SDL_Delay(1);
            continue;
        }
        coop_slot_t *slot = &coop_ring[coop_r % COOP_RING];
        if (!slot->ready) {
            SDL_Delay(1);
            continue;
        }
        char payload[256], result[256];
        /* Rebase on the FIRST FED FRAME, not on pipeline open. We wait for an IDR
         * before feeding, and that wait was ~20s in the last run, so the first frame
         * arrived stamped 20s into a stream the decoder had only just been handed --
         * which it rejected, correctly. The first frame must be pts=0. */
        if (coop_pts_base_ns == 0) {
            coop_pts_base_ns = coop_now_ns();
        }
        uint64_t pts = coop_now_ns() - coop_pts_base_ns;
        snprintf(payload, sizeof(payload),
                 "{\"bufferAddr\":\"%p\",\"bufferSize\":%u,\"pts\":%llu,\"esData\":1}",
                 (void *) slot->data, (unsigned) slot->size, (unsigned long long) pts);
        StarfishMediaAPIs_feed(coop_mirror_api, payload, result, sizeof(result));
        if (coop_mirror_fed < 5 && diag_log != NULL) {
            fprintf(diag_log, "[COOP-PROBE] feed #%u size=%u pts=%lluns -> %s\n",
                    coop_mirror_fed + 1, (unsigned) slot->size,
                    (unsigned long long) pts, result);
            fflush(diag_log);
        }
        coop_mirror_fed++;
        slot->ready = 0;
        coop_r++;
    }
    return NULL;
}

/* Release the mirrored pipeline + its exported window. MUST run on stream end:
 * mirror mode previously returned early and never cleaned up, leaking a pipeline
 * and a window per stream. Nine of those exhausted the TV's media resources and
 * left it unable to open ANY stream until power-cycled. */
static void coop_mirror_teardown(void) {
    coop_feed_run = 0;
    if (coop_win_probe_window != NULL) {
        SDL_webOSDestroyExportedWindow(coop_win_probe_window);
        coop_win_probe_window = NULL;
    }
    if (coop_mirror_api != NULL) {
        StarfishMediaAPIs_unload(coop_mirror_api);
        StarfishMediaAPIs_destroy(coop_mirror_api);
        coop_mirror_api = NULL;
    }
    if (coop_mirror_window != NULL) {
        if (coop_wl_window_id[0] == '\0' || strcmp(coop_mirror_window, coop_wl_window_id) != 0) {
            SDL_webOSDestroyExportedWindow(coop_mirror_window);   /* only SDL-created ones */
        }
        coop_mirror_window = NULL;
    }
    for (int i = 0; i < COOP_RING; ++i) {
        free(coop_ring[i].data);
        coop_ring[i].data = NULL;
        coop_ring[i].cap = coop_ring[i].size = coop_ring[i].ready = 0;
    }
    coop_w = coop_r = 0;
    coop_started = 0;
    coop_sf_loadcompleted = 0;
    coop_sf_events = 0;
    coop_pts_base_ns = 0;
    coop_mirror_fed = 0;
}

/* Called from the submit thread: copy only, never block. */
static void coop_mirror_feed(const unsigned char *data, size_t size, bool is_idr) {
    if (coop_mirror_api == NULL || !coop_sf_loadcompleted) {
        return;
    }
    if (!coop_started) {
        if (!is_idr) {
            return;   /* a decoder cannot start mid-GOP */
        }
        coop_started = 1;
        coop_feed_run = 1;
        pthread_create(&coop_feed_thread, NULL, coop_feed_worker, NULL);
        pthread_detach(coop_feed_thread);
    }
    if (coop_w - coop_r >= COOP_RING) {
        coop_dropped++;
        return;   /* consumer behind — drop, never stall the stream */
    }
    coop_slot_t *slot = &coop_ring[coop_w % COOP_RING];
    if (slot->cap < size) {
        unsigned char *p = realloc(slot->data, size);
        if (p == NULL) { return; }
        slot->data = p;
        slot->cap = size;
    }
    memcpy(slot->data, data, size);
    slot->size = size;
    slot->ready = 1;
    coop_w++;
}

#else
static void coop_starfish_second_pipeline_probe(SS4S_VideoCodec codec) { (void) codec; }
#endif

static void coop_second_decoder_probe(SS4S_VideoInfo primary) {
    coop_child_process_probe();
    coop_exported_window_probe();
    coop_starfish_second_pipeline_probe(primary.codec);
    if (access("/tmp/coop-probe", F_OK) != 0 || diag_log == NULL) {
        return;
    }
    fprintf(diag_log, "[COOP-PROBE] 2nd-decoder test while primary active (%dx%d codec=%d)\n",
            primary.width, primary.height, (int) primary.codec);
    fflush(diag_log);
    SS4S_Player *p2 = SS4S_PlayerOpen();
    if (p2 == NULL) {
        fprintf(diag_log, "[COOP-PROBE] FAIL: SS4S_PlayerOpen() == NULL (no 2nd player)\n");
        fflush(diag_log);
        return;
    }
    SS4S_VideoInfo info2 = primary;
    SS4S_VideoOpenResult r = SS4S_PlayerVideoOpen(p2, &info2);
    fprintf(diag_log,
            "[COOP-PROBE] 2nd SS4S_PlayerVideoOpen result=%d (0=OK 1=unsupported 2=error) => %s\n",
            (int) r,
            r == SS4S_VIDEO_OPEN_OK ? "SECOND DECODER GRANTED (dual-decode viable)"
                                    : "SECOND DECODER DENIED (dual-decode blocked on same appId)");
    fflush(diag_log);

    /* Release the 2nd decoder straight away: the grant result is already in hand,
     * and holding it open contends the decoder while the primary pipeline is still
     * coming up. An earlier revision held it for 5s here to make the split visible
     * and that is what made the stream die with "decoder reported error" — never
     * block this path. */
    SS4S_PlayerVideoClose(p2);
    SS4S_PlayerClose(p2);
    fprintf(diag_log, "[COOP-PROBE] 2nd player closed\n");

    /* SPIKE #2 — split-screen plane positioning, on the PRIMARY plane only.
     * The active backend is NDL webOS5, whose NDL_DirectVideoSetArea is a GLOBAL
     * call with no player handle: positioning a second player in this process
     * would just override this one, so a two-plane test here is meaningless. The
     * real feature places one plane per PROCESS, which this validates one half at
     * a time.
     * Layout comes from the sentinel's first byte: 'h' = horizontal (top half),
     * anything else = vertical (left half). The plane is LEFT IN PLACE — no
     * blocking sleep, no restore — so it stays plainly visible. Remove the
     * sentinel and restart the stream to get fullscreen back. */
    if (player != NULL) {
        int sw = session != NULL ? session->display_width : 0;
        int sh = session != NULL ? session->display_height : 0;
        if (sw <= 0 || sh <= 0) { sw = 1920; sh = 1080; }
        char orient = 'v';
        FILE *sf = fopen("/tmp/coop-probe", "r");
        if (sf != NULL) {
            int c = fgetc(sf);
            if (c == 'h' || c == 'H') { orient = 'h'; }
            fclose(sf);
        }
        SS4S_VideoRect d1 = orient == 'h' ? (SS4S_VideoRect) {0, 0, sw, sh / 2}
                                          : (SS4S_VideoRect) {0, 0, sw / 2, sh};
        bool ok1 = SS4S_PlayerVideoSetDisplayArea(player, NULL, &d1);
        fprintf(diag_log,
                "[COOP-PROBE] split=%s screen=%dx%d P1=(%d,%d %dx%d) set=%d => %s\n",
                orient == 'h' ? "horizontal" : "vertical", sw, sh,
                d1.x, d1.y, d1.width, d1.height, (int) ok1,
                ok1 ? "PLANE POSITIONED (watch: stream should fill only its half)"
                    : "POSITIONING REFUSED (backend has no SetDisplayArea)");
    }
    fflush(diag_log);
}

#if HAVE_STARFISH_DIRECT
/* Pump the default GLib main context for the life of a stream.
 *
 * starfish delivers its state callbacks (notably LOADCOMPLETED) through a GLib
 * main context. Eclipse is SDL/LVGL based and never iterates one, so those
 * callbacks never arrive -- which is why our direct pipeline sat in LOADED and
 * rejected every feed until we pumped it by hand.
 *
 * The SMP SS4S module has the same dependency, and it fails as the PRIMARY
 * decoder on this TV ("decoder reported error"). The missing pump is the leading
 * explanation, so run one for any stream: it costs almost nothing when the
 * context is idle, and it is what SMP needs to reach PLAYING.
 */
static pthread_t coop_pump_thread;
static volatile int coop_pump_run = 0;

static void *coop_pump_worker(void *arg) {
    (void) arg;
    GMainContext *ctx = g_main_context_default();
    while (coop_pump_run) {
        while (g_main_context_iteration(ctx, FALSE)) { /* drain */ }
        SDL_Delay(2);
    }
    return NULL;
}

static void coop_pump_start(void) {
    /* GATED. Draining the DEFAULT GLib main context from a background thread can
     * consume events other components rely on -- NDL included -- so this must not
     * run in normal use. It exists only for the starfish experiments, which need
     * LOADCOMPLETED delivered. Enable with /tmp/coop-pump. */
    if (access("/tmp/coop-pump", F_OK) != 0) { return; }
    if (coop_pump_run) { return; }
    coop_pump_run = 1;
    if (pthread_create(&coop_pump_thread, NULL, coop_pump_worker, NULL) == 0) {
        pthread_detach(coop_pump_thread);
    } else {
        coop_pump_run = 0;
    }
}

static void coop_pump_stop(void) { coop_pump_run = 0; }
#else
static void coop_pump_start(void) {}
static void coop_pump_stop(void) {}
#endif

#if HAVE_STARFISH_DIRECT
#include <SDL_syswm.h>
#include <wayland-client.h>
#include <wayland-webos-foreign-client-protocol.h>
#include <wayland-webos-surface-group-client-protocol.h>

/* Wayland registry probe.
 *
 * Everything we have tried at the surface layer went through SDL's thin wrapper
 * (SDL_webOSCreateExportedWindow / SetExportedWindow), which only exports and
 * gives no feedback about whether the compositor attached anything. The protocol
 * underneath -- wl_webos_foreign, with export_element, import_element,
 * set_crop_region and a window_id_assigned listener -- is a CLIENT-side Wayland
 * interface, so it is NOT gated by the surfaces.control ACL that blocks
 * surfacemanager's luna API.
 *
 * Step one is simply: does the compositor advertise wl_webos_foreign to us at
 * all? If it does, the richer protocol is reachable and worth driving directly.
 * If it does not, the surface layer really is closed and we stop here.
 *
 * Gated on /tmp/coop-probe-wl.
 */
static struct wl_display *coop_wl_dpy = NULL;
static struct wl_surface *coop_wl_surface = NULL;
static struct wl_webos_foreign *coop_wl_foreign = NULL;
static struct wl_compositor *coop_wl_compositor = NULL;
static struct wl_subcompositor *coop_wl_subcompositor = NULL;
static struct wl_webos_surface_group_compositor *coop_wl_sgc = NULL;
static FILE *coop_wl_log = NULL;

static void coop_wl_window_id_cb(void *data, struct wl_webos_exported *exported,
                                 const char *window_id, uint32_t exported_type) {
    (void) exported;
    FILE *log = data;
    fprintf(log, "  *** window_id_assigned: %s (type=%u) -- compositor ACCEPTED the export\n",
            window_id ? window_id : "(null)", exported_type);
    fflush(log);
    if (window_id != NULL) {
        strncpy(coop_wl_window_id, window_id, sizeof(coop_wl_window_id) - 1);
    }
}

static void coop_wl_bind_cb(void *data, struct wl_registry *reg, uint32_t name,
                            const char *interface, uint32_t version) {
    (void) data; (void) version;
    if (strcmp(interface, "wl_webos_foreign") == 0) {
        coop_wl_foreign = wl_registry_bind(reg, name, &wl_webos_foreign_interface, 1);
    } else if (strcmp(interface, "wl_compositor") == 0) {
        coop_wl_compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 1);
    } else if (strcmp(interface, "wl_subcompositor") == 0) {
        coop_wl_subcompositor = wl_registry_bind(reg, name, &wl_subcompositor_interface, 1);
    } else if (strcmp(interface, "wl_webos_surface_group_compositor") == 0) {
        coop_wl_sgc = wl_registry_bind(reg, name, &wl_webos_surface_group_compositor_interface, 1);
    }
}

static void coop_wayland_registry_cb(void *data, struct wl_registry *reg, uint32_t name,
                                     const char *interface, uint32_t version) {
    (void) reg; (void) name;
    FILE *log = data;
    fprintf(log, "  global: %-40s v%u\n", interface, version);
    fflush(log);
}

static void coop_wayland_registry_remove_cb(void *data, struct wl_registry *reg, uint32_t name) {
    (void) data; (void) reg; (void) name;
}

static void coop_wayland_probe(void) {
    if (access("/tmp/coop-probe-wl", F_OK) != 0) {
        return;
    }
    FILE *log = fopen("/tmp/coop_wayland.log", "a");
    if (log == NULL) {
        return;
    }
    fprintf(log, "=== wayland probe run=%ld ===\n", (long) time(NULL));
    /* Open our own compositor connection rather than borrowing SDL's window: the
     * probe runs on the video thread where there is no current GL window, and the
     * globals a compositor advertises are per-connection-independent anyway. */
    struct wl_display *dpy = wl_display_connect(NULL);
    fprintf(log, "  wl_display_connect -> %p (WAYLAND_DISPLAY=%s)\n", (void *) dpy,
            getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");
    if (dpy == NULL) {
        fprintf(log, "  cannot reach the compositor from this process\n");
        fclose(log);
        return;
    }
    static const struct wl_registry_listener listener = {
            .global = coop_wayland_registry_cb,
            .global_remove = coop_wayland_registry_remove_cb,
    };
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &listener, log);
    wl_display_roundtrip(dpy);
    wl_display_disconnect(dpy);

    /* Now drive wl_webos_foreign DIRECTLY on SDL's own connection. SDL's wrapper
     * exports a window and tells us nothing; the protocol has a
     * window_id_assigned listener that reports whether the compositor actually
     * accepted the export. That feedback is what we have been missing -- our
     * exported windows produced ids but never composited, and we could not see
     * which side failed. */
    if (session == NULL || session->app == NULL || session->app->ui.window == NULL) {
        fprintf(log, "  no SDL window on the session -- cannot export from our surface\n");
        fclose(log);
        return;
    }
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(session->app->ui.window, &info)) {
        fprintf(log, "  SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
        fclose(log);
        return;
    }
    fprintf(log, "  SDL subsystem=%d (wayland=%d)\n", (int) info.subsystem, (int) SDL_SYSWM_WAYLAND);
    if (info.subsystem != SDL_SYSWM_WAYLAND) {
        fprintf(log, "  SDL is not on wayland -- cannot reach its surface\n");
        fclose(log);
        return;
    }
    coop_wl_dpy = info.info.wl.display;
    coop_wl_surface = info.info.wl.surface;
    fprintf(log, "  SDL wl_display=%p wl_surface=%p\n", (void *) coop_wl_dpy,
            (void *) coop_wl_surface);
    if (coop_wl_dpy == NULL || coop_wl_surface == NULL) {
        fclose(log);
        return;
    }
    static const struct wl_registry_listener bind_listener = {
            .global = coop_wl_bind_cb,
            .global_remove = coop_wayland_registry_remove_cb,
    };
    coop_wl_log = log;
    struct wl_registry *reg2 = wl_display_get_registry(coop_wl_dpy);
    wl_registry_add_listener(reg2, &bind_listener, log);
    wl_display_roundtrip(coop_wl_dpy);
    fprintf(log, "  bound: foreign=%p compositor=%p\n", (void *) coop_wl_foreign,
            (void *) coop_wl_compositor);
    if (coop_wl_foreign == NULL) {
        fprintf(log, "  wl_webos_foreign not bindable\n");
        fclose(log);
        return;
    }
    static const struct wl_webos_exported_listener exp_listener = {
            .window_id_assigned = coop_wl_window_id_cb,
    };
    /* Export from a SUBSURFACE, not the main surface.
     *
     * Everything points to one video plane per app SURFACE: we can mint any number
     * of exported window ids and any number of decode pipelines, yet only one ever
     * displays -- and Multi-View manages two videos precisely because it is two
     * apps, i.e. two surfaces. A subsurface is a distinct wl_surface, so it may
     * earn its own plane assignment while still belonging to our window.
     */
    struct wl_surface *export_surface = coop_wl_surface;

    /* Surface GROUPS: webOS's own multi-surface compositing mechanism, with named
     * z-ordered layers. This is the most likely machinery behind Multi-View, and
     * unlike surfacemanager's luna API it is advertised to us unprivileged. If a
     * surface attached to a group layer earns its own video plane, one app can
     * display two videos -- which subsurfaces did not achieve. */
    if (coop_wl_sgc != NULL && coop_wl_compositor != NULL &&
        access("/tmp/coop-probe-group", F_OK) == 0) {
        struct wl_webos_surface_group *grp = wl_webos_surface_group_compositor_create_surface_group(
                coop_wl_sgc, coop_wl_surface, "aurora_coop");
        fprintf(log, "  create_surface_group -> %p\n", (void *) grp);
        if (grp != NULL) {
            wl_webos_surface_group_allow_anonymous_layers(grp, 1);
            wl_webos_surface_group_create_layer(grp, "pane2", 1);
            struct wl_surface *gs = wl_compositor_create_surface(coop_wl_compositor);
            if (gs != NULL) {
                wl_webos_surface_group_attach(grp, gs, "pane2");
                wl_surface_commit(gs);
                wl_display_roundtrip(coop_wl_dpy);
                export_surface = gs;
                fprintf(log, "  attached surface %p to group layer 'pane2'\n", (void *) gs);
            }
        }
    } else if (coop_wl_subcompositor != NULL && coop_wl_compositor != NULL &&
               access("/tmp/coop-probe-solo", F_OK) != 0) {
        /* In solo mode export from the MAIN surface: it is the only one with a
         * wl_buffer attached (SDL renders it). A surface with no buffer is never
         * mapped, so a subsurface/group surface we never attached to cannot
         * display regardless of planes -- that would confound the control. */
        struct wl_surface *sub = wl_compositor_create_surface(coop_wl_compositor);
        if (sub != NULL) {
            struct wl_subsurface *ss = wl_subcompositor_get_subsurface(
                    coop_wl_subcompositor, sub, coop_wl_surface);
            if (ss != NULL) {
                wl_subsurface_set_desync(ss);
                wl_subsurface_set_position(ss, 960, 0);
                export_surface = sub;
                fprintf(log, "  using SUBSURFACE %p (parent %p) for the export\n",
                        (void *) sub, (void *) coop_wl_surface);
            }
        }
    }
    fprintf(log, "  export surface = %s\n",
            export_surface == coop_wl_surface ? "MAIN" : "SUBSURFACE");
    struct wl_webos_exported *exported = wl_webos_foreign_export_element(
            coop_wl_foreign, export_surface, WL_WEBOS_FOREIGN_WEBOS_EXPORTED_TYPE_VIDEO_OBJECT);
    fprintf(log, "  export_element -> %p\n", (void *) exported);
    if (exported != NULL) {
        wl_webos_exported_add_listener(exported, &exp_listener, log);
        wl_display_roundtrip(coop_wl_dpy);
        if (coop_wl_compositor != NULL) {
            struct wl_region *srcr = wl_compositor_create_region(coop_wl_compositor);
            struct wl_region *dstr = wl_compositor_create_region(coop_wl_compositor);
            wl_region_add(srcr, 0, 0, 1920, 1080);
            wl_region_add(dstr, 960, 0, 960, 1080);
            wl_webos_exported_set_exported_window(exported, srcr, dstr);
            wl_surface_commit(export_surface);
            wl_surface_commit(coop_wl_surface);
            wl_display_roundtrip(coop_wl_dpy);
            fprintf(log, "  set_exported_window(dst 960,0 960x1080) + surface commit done\n");
        }
    }
    fprintf(log, "=== wayland probe end ===\n");
    fclose(log);
}

/* --- videooutput/getStatus: which sink is connected to which PLANE, and at what z-order ---
 *
 * The whole investigation has been blind on exactly this. A starfish pipeline decodes and
 * renders into a SINK; a sink must be connected to a video PLANE to be composited. Our second
 * pipeline reports videoSinkType "main_video" -- the same sink the primary NDL pipeline owns.
 * getStatus reports connected state, source, dimensions, mute and z-order for ALL sinks, so it
 * either confirms that collision or kills the theory outright. Read-only, so it cannot make
 * anything worse. Gated on /tmp/coop-probe-vo. */
#if HAVE_LUNA_SERVICE
#include <luna-service2/lunaservice.h>

/* Last reply body, kept so the caller can PARSE it and not merely log it. Finding the second
 * pipeline's context depends on diffing two getStatus replies. */
static char coop_vo_last[8192];

static bool coop_vo_reply(LSHandle *sh, LSMessage *msg, void *ctx) {
    (void) sh;
    const char *payload = LSMessageGetPayload(msg);
    snprintf(coop_vo_last, sizeof(coop_vo_last), "%s", payload ? payload : "");
    if (diag_log != NULL) {
        fprintf(diag_log, "[COOP-VO] %s reply: %.1600s\n", ctx ? (const char *) ctx : "?",
                payload ? payload : "(null)");
        fflush(diag_log);
    }
    return true;
}

/* Collect every clientId in a getStatus reply, '|'-separated. The service reports a `clients`
 * array of {appId, sinkName, sourceName, clientId}; the clientId IS the context that connect
 * and setDisplayWindow want. getMediaID() is NOT it -- it returned the id MAIN already held. */
static void coop_vo_client_ids(const char *json, char *out, size_t cap) {
    out[0] = '\0';
    const char *p = json;
    const char *key = "\"clientId\":\"";
    size_t used = 0;
    while ((p = strstr(p, key)) != NULL) {
        p += strlen(key);
        const char *e = strchr(p, '"');
        if (e == NULL) { break; }
        size_t n = (size_t) (e - p);
        if (n > 0 && used + n + 2 < cap) {
            memcpy(out + used, p, n);
            used += n;
            out[used++] = '|';
            out[used] = '\0';
        }
        p = e;
    }
}

/* Clients present in `after` but not in `before` -- i.e. the pipeline we just created. */
static bool coop_vo_new_client(const char *before, const char *after, char *out, size_t cap) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", after);
    char *save = NULL;
    for (char *tok = strtok_r(tmp, "|", &save); tok != NULL; tok = strtok_r(NULL, "|", &save)) {
        char needle[160];
        snprintf(needle, sizeof(needle), "%s|", tok);
        if (strstr(before, needle) == NULL) {
            snprintf(out, cap, "%s", tok);
            return true;
        }
    }
    return false;
}

/* Snapshot taken BEFORE the second pipeline exists, so the diff can identify it. */
static char coop_vo_clients_before[512];

/* One request + pump for its reply. The service is async and nothing dispatches unless we
 * iterate the context ourselves -- the same lesson as the starfish load callback. */
static void coop_vo_call(LSHandle *sh, GMainContext *ctx, const char *uri, const char *payload,
                         const char *label) {
    LSError e;
    LSErrorInit(&e);
    fprintf(diag_log, "[COOP-VO] -> %s %s\n", label, payload);
    fflush(diag_log);
    if (!LSCallOneReply(sh, uri, payload, coop_vo_reply, (void *) label, NULL, &e)) {
        fprintf(diag_log, "[COOP-VO] %s CALL FAILED: %s\n", label, e.message ? e.message : "?");
        fflush(diag_log);
        LSErrorFree(&e);
        return;
    }
    for (int i = 0; i < 800; ++i) {
        while (g_main_context_iteration(ctx, FALSE)) { }
        usleep(1000);
    }
}

/* Pre-creation snapshot of the videooutput client list. Registering, calling and tearing down
 * a throwaway anonymous LS2 handle is cheap and keeps this independent of the probe's own
 * handle lifetime. */
static void coop_videooutput_snapshot(void) {
    coop_vo_clients_before[0] = '\0';
    if (access("/tmp/coop-probe-vo", F_OK) != 0 || diag_log == NULL) {
        return;
    }
    LSError err;
    LSErrorInit(&err);
    LSHandle *sh = NULL;
    if (!LSRegister(NULL, &sh, &err)) {
        LSErrorFree(&err);
        return;
    }
    GMainContext *ctx = g_main_context_new();
    if (LSGmainContextAttach(sh, ctx, &err)) {
        coop_vo_last[0] = '\0';
        coop_vo_call(sh, ctx, "luna://com.webos.service.videooutput/getStatus", "{}",
                     "getStatus(pre-create)");
        coop_vo_client_ids(coop_vo_last, coop_vo_clients_before, sizeof(coop_vo_clients_before));
        fprintf(diag_log, "[COOP-VO] clients before pipeline = [%s]\n", coop_vo_clients_before);
        fflush(diag_log);
    } else {
        LSErrorFree(&err);
    }
    g_main_context_unref(ctx);
    LSUnregister(sh, &err);
    LSErrorFree(&err);
}

static void coop_videooutput_probe(const char *mediaId) {
    if (access("/tmp/coop-probe-vo", F_OK) != 0 || diag_log == NULL) {
        return;
    }
    LSError err;
    LSErrorInit(&err);
    LSHandle *sh = NULL;
    /* WHICH SERVICE NAME IS ALLOWED IS THE WHOLE QUESTION, so it comes from the sentinel's
     * contents (`echo <name> > /tmp/coop-probe-vo`) and each candidate costs a relaunch, not a
     * rebuild. An invented name is refused outright ("Invalid permissions for
     * com.aurora.gamestream.vo"), and the app has NO role file under /usr/share/luna-service2
     * at all -- so the only names with any chance are the app's own id, or an empty/NULL
     * registration which LS2 treats as an anonymous client. */
    char namebuf[128] = "";
    FILE *nf = fopen("/tmp/coop-probe-vo", "r");
    if (nf != NULL) {
        if (fscanf(nf, "%127s", namebuf) != 1) { namebuf[0] = '\0'; }
        fclose(nf);
    }
    const char *name = namebuf[0] != '\0' ? namebuf : APPID;
    if (strcmp(name, "NULL") == 0) { name = NULL; }   /* anonymous client */
    if (!LSRegister(name, &sh, &err)) {
        fprintf(diag_log, "[COOP-VO] LSRegister(%s) FAILED: %s\n", name ? name : "(NULL)",
                err.message ? err.message : "?");
        fflush(diag_log);
        LSErrorFree(&err);
        return;
    }
    fprintf(diag_log, "[COOP-VO] LSRegister(%s) ok\n", name ? name : "(NULL)");
    GMainContext *ctx = g_main_context_new();
    GMainLoop *loop = g_main_loop_new(ctx, FALSE);
    if (!LSGmainContextAttach(sh, ctx, &err)) {
        fprintf(diag_log, "[COOP-VO] attach FAILED: %s\n", err.message ? err.message : "?");
        LSErrorFree(&err);
    } else {
        const char *VO = "luna://com.webos.service.videooutput";
        char uri[160], pl[512];
        snprintf(uri, sizeof(uri), "%s/getStatus", VO);
        coop_vo_call(sh, ctx, uri, "{}", "getStatus(before)");

        /* THE FIX ATTEMPT. getStatus proved the second pipeline is connected to NO sink at
         * all: MAIN is held by the primary and SUB0/1/2 are connected=false AND muted=true.
         * So: connect its VDEC to SUB0, give it a display window, UNMUTE it (connecting alone
         * cannot be enough while muted=true), and set compositing. Gated separately so the
         * read-only diagnostic can still be run alone. */
        if (access("/tmp/coop-probe-vo-connect", F_OK) == 0) {
            int sx = 960, sy = 0, sw = 960, sh_ = 1080;
            FILE *rf2 = fopen("/tmp/coop-probe-vo-rect", "r");
            if (rf2 != NULL) {
                int a, b, c, d;
                if (fscanf(rf2, "%d %d %d %d", &a, &b, &c, &d) == 4 && c > 0 && d > 0) {
                    sx = a; sy = b; sw = c; sh_ = d;
                }
                fclose(rf2);
            }
            /* THE SEQUENCE FROM LG'S OWN TESTS, shipped on the TV at
             * /usr/share/com.webos.service.videooutput/test/luna/demo_script.py. Every
             * previous attempt was an incomplete request:
             *   - no `register`, so the connection had no client behind it. THE CONTEXT IS A
             *     NAME WE INVENT, not an id to discover -- the clients-array diffing that
             *     preceded this was solving a problem that does not exist.
             *   - no `outputMode:"DISPLAY"`, so nothing selected the display output.
             *   - no `setVideoData`, so the compositor never learned the video's geometry.
             *     That is why every getStatus showed width:0 height:0 frameRate:0 while also
             *     reporting connected + unmuted + progressive, and drew nothing.
             * setDisplayWindow needs the context too (errorCode 53 "Context Error"), and
             * setCompositing must list EVERY sink in one array (errorCode 4 otherwise). */
            const char *PID = "aurora_pane2";
            char uri2[160];

            snprintf(uri2, sizeof(uri2), "%s/disconnect", VO);
            coop_vo_call(sh, ctx, uri2, "{\"sink\":\"SUB0\"}", "disconnect(stale)");

            snprintf(uri2, sizeof(uri2), "%s/register", VO);
            snprintf(pl, sizeof(pl), "{\"context\":\"%s\",\"appId\":\"%s\"}", PID, APPID);
            coop_vo_call(sh, ctx, uri2, pl, "register");

            snprintf(uri2, sizeof(uri2), "%s/connect", VO);
            snprintf(pl, sizeof(pl),
                     "{\"outputMode\":\"DISPLAY\",\"sink\":\"SUB0\",\"source\":\"VDEC\","
                     "\"sourcePort\":0,\"context\":\"%s\"}", PID);
            coop_vo_call(sh, ctx, uri2, pl, "connect(SUB0)");

            /* The one that was missing outright. Dimensions must match what we actually feed. */
            snprintf(uri2, sizeof(uri2), "%s/setVideoData", VO);
            snprintf(pl, sizeof(pl),
                     "{\"sink\":\"SUB0\",\"context\":\"%s\",\"contentType\":\"media\","
                     "\"frameRate\":60.0,\"width\":1920,\"height\":1080,"
                     "\"scanType\":\"progressive\"}", PID);
            coop_vo_call(sh, ctx, uri2, pl, "setVideoData(SUB0)");

            snprintf(uri2, sizeof(uri2), "%s/blankVideo", VO);
            coop_vo_call(sh, ctx, uri2, "{\"sink\":\"SUB0\",\"blank\":false}", "unmute(SUB0)");

            snprintf(uri2, sizeof(uri2), "%s/display/setDisplayWindow", VO);
            snprintf(pl, sizeof(pl),
                     "{\"sink\":\"SUB0\",\"context\":\"%s\",\"fullScreen\":false,"
                     "\"displayOutput\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}}",
                     PID, sx, sy, sw, sh_);
            coop_vo_call(sh, ctx, uri2, pl, "setDisplayWindow(SUB0)");

            /* BOTH sinks, one array -- a single-sink composeOrder is rejected. */
            snprintf(uri2, sizeof(uri2), "%s/display/setCompositing", VO);
            coop_vo_call(sh, ctx, uri2,
                         "{\"composeOrder\":[{\"sink\":\"MAIN\",\"opacity\":255,\"zOrder\":0},"
                         "{\"sink\":\"SUB0\",\"opacity\":255,\"zOrder\":1}]}",
                         "setCompositing(both)");

            snprintf(uri, sizeof(uri), "%s/getStatus", VO);
            coop_vo_call(sh, ctx, uri, "{}", "getStatus(after)");
        }
    }
vo_done:
    fflush(diag_log);
    g_main_loop_unref(loop);
    g_main_context_unref(ctx);
    LSUnregister(sh, &err);
    LSErrorFree(&err);
}
#else
static void coop_videooutput_probe(const char *mediaId) { (void) mediaId; }
static void coop_videooutput_snapshot(void) {}
#endif

/* --- IMPORT + PUNCHTHROUGH: the last untried display route (see the forward decl) --- */

static void coop_imp_dest_changed(void *data, struct wl_webos_imported *imp,
                                  uint32_t width, uint32_t height) {
    (void) data; (void) imp;
    if (diag_log != NULL) {
        fprintf(diag_log, "[COOP-IMPORT] destination_region_changed %ux%u\n", width, height);
        fflush(diag_log);
    }
}

static void coop_imp_punch_attached(void *data, struct wl_webos_imported *imp, const char *ctx) {
    (void) data; (void) imp;
    if (diag_log != NULL) {
        /* THE EVENT THAT WOULD MEAN SUCCESS: the compositor confirming it cut the hole. */
        fprintf(diag_log, "[COOP-IMPORT] *** punchthrough_attached context=%s ***\n",
                ctx ? ctx : "(null)");
        fflush(diag_log);
    }
}

static void coop_imp_punch_detached(void *data, struct wl_webos_imported *imp, const char *ctx) {
    (void) data; (void) imp;
    if (diag_log != NULL) {
        fprintf(diag_log, "[COOP-IMPORT] punchthrough_detached context=%s\n", ctx ? ctx : "(null)");
        fflush(diag_log);
    }
}

static void coop_imp_surf_attached(void *data, struct wl_webos_imported *imp,
                                   struct wl_surface *surface) {
    (void) data; (void) imp;
    if (diag_log != NULL) {
        fprintf(diag_log, "[COOP-IMPORT] surface_attached %p\n", (void *) surface);
        fflush(diag_log);
    }
}

static void coop_imp_surf_detached(void *data, struct wl_webos_imported *imp,
                                   struct wl_surface *surface) {
    (void) data; (void) imp;
    if (diag_log != NULL) {
        fprintf(diag_log, "[COOP-IMPORT] surface_detached %p\n", (void *) surface);
        fflush(diag_log);
    }
}

static const struct wl_webos_imported_listener coop_imported_listener = {
        .destination_region_changed = coop_imp_dest_changed,
        .punchthrough_attached = coop_imp_punch_attached,
        .punchthrough_detached = coop_imp_punch_detached,
        .surface_attached = coop_imp_surf_attached,
        .surface_detached = coop_imp_surf_detached,
};

static void coop_import_punchthrough(const char *wid) {
    if (access("/tmp/coop-probe-import", F_OK) != 0 || diag_log == NULL || wid == NULL) {
        return;
    }
    /* The probe normally runs before the Wayland globals are bound, so bind them here if
     * needed rather than depending on probe ordering. */
    if (coop_wl_foreign == NULL) {
        coop_wayland_probe();
    }
    if (coop_wl_dpy == NULL || coop_wl_foreign == NULL) {
        fprintf(diag_log, "[COOP-IMPORT] no wl_webos_foreign (dpy=%p foreign=%p) -- cannot import\n",
                (void *) coop_wl_dpy, (void *) coop_wl_foreign);
        fflush(diag_log);
        return;
    }
    struct wl_webos_imported *imp = wl_webos_foreign_import_element(
            coop_wl_foreign, wid, WL_WEBOS_FOREIGN_WEBOS_EXPORTED_TYPE_VIDEO_OBJECT);
    fprintf(diag_log, "[COOP-IMPORT] import_element(%s) -> %p\n", wid, (void *) imp);
    fflush(diag_log);
    if (imp == NULL) {
        return;
    }
    wl_webos_imported_add_listener(imp, &coop_imported_listener, NULL);
    wl_webos_imported_set_surface_alignment(imp, WL_WEBOS_IMPORTED_SURFACE_ALIGNMENT_STRETCH);
    /* `/tmp/coop-probe-import-surface` swaps the punchthrough for attaching OUR surface to
     * the imported element -- the other half of this protocol, kept switchable because which
     * one a video element wants is not documented anywhere we have. */
    if (access("/tmp/coop-probe-import-surface", F_OK) == 0 && coop_wl_surface != NULL) {
        wl_webos_imported_attach_surface(imp, coop_wl_surface);
        fprintf(diag_log, "[COOP-IMPORT] attach_surface(%p) issued\n", (void *) coop_wl_surface);
    } else {
        wl_webos_imported_attach_punchthrough(imp);
        fprintf(diag_log, "[COOP-IMPORT] attach_punchthrough() issued\n");
    }
    if (coop_wl_surface != NULL) {
        wl_surface_commit(coop_wl_surface);
    }
    wl_display_roundtrip(coop_wl_dpy);
    fprintf(diag_log, "[COOP-IMPORT] commit + roundtrip done\n");
    fflush(diag_log);
}
#else
static void coop_wayland_probe(void) {}
static void coop_import_punchthrough(const char *wid) { (void) wid; }
#endif

#if TARGET_ANDROID
/* THE SECOND PC IS AWAITED BESIDE THE STREAM ON ANDROID, NOT BEFORE IT. This set-up runs inside moonlight's
 * startVideoStream, which starts the primary's UDP ping only once it returns -- and the primary host gives up on
 * a client whose ping has not arrived after a few seconds ("Initial Ping Timeout"). Waiting here for the secondary's
 * first live frame took 6.7 and 8.4 s on the SHIELD TV, against host time-outs of 6.6 and 8.3 s, and both starts
 * ended as a network error (2026-10-16). On Android the same 30 s readiness check runs on this thread instead, and
 * a secondary that fails still ends the session with the co-op error, so co-op never degrades into one pane. */
static pthread_t coop_ready_thread;
static bool coop_ready_thread_live = false;
static volatile int coop_ready_cancel = 0;
static bool coop_ready_ui = false;

/* On the main thread, like every other controller event. */
static void coop_reannounce_pads(void *data) {
    app_t *app = data;
    if (app->session != NULL) {
        stream_input_reannounce_peer_gamepads(session_get_input(app->session));
    }
}

static void *coop_ready_watch(void *arg) {
    session_t *watched = arg;
    if (coop_coord_wait_secondary_ready_cancellable(30000, &coop_ready_cancel)) {
        if (diag_log != NULL) {
            fprintf(diag_log, "[COOP-SPAWN] secondary READY with a live frame (%s, awaited beside the stream)\n",
                    coop_ready_ui ? "ui" : "sentinel");
            fflush(diag_log);
        }
        /* Its controllers may have been announced before it could hear them. */
        app_bus_post(watched->app, coop_reannounce_pads, watched->app);
        return NULL;
    }
    if (coop_ready_cancel) {
        return NULL;                            /* the session ended first */
    }
    if (diag_log != NULL) {
        fprintf(diag_log, "[COOP-SPAWN] secondary readiness FAILED: %s\n", coop_coord_start_error_message());
        fflush(diag_log);
    }
    streaming_error(watched, 0, "Could not start co-op: %s", coop_coord_start_error_message());
    session_interrupt(watched, false, STREAMING_INTERRUPT_ERROR);
    return NULL;
}
#endif

int vdec_delegate_setup(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags) {
    /* HALF-HEIGHT: the SESSION negotiated 3840x1080 (each host sends only its own rows),
     * but what we hand the decoder is the COMPOSITE of two such pictures. Setting the
     * plane up at the negotiated height letterboxes the result -- the picture decodes and
     * both panes are present, but the renderer sizes for 1080 and the composite is drawn
     * into a band with black above and below. Tell it the height it will actually get. */
    if (app_launch_coop_requested() && access("/tmp/coop-live", F_OK) == 0) {
        /* UNCONDITIONAL, AND NOT KEYED ON THE SLICE COUNT. It was briefly gated on
         * /tmp/coop-slices for a full-height two-slice design where each host encoded the WHOLE
         * composite; that design is abandoned (it puts the primary's taskbar inside the peer's
         * pane, so the two halves stop being two monitors). Slices per picture and pane height
         * are now INDEPENDENT: the panes stay half-height and each host may cut its own pane
         * into as many slices as the encoder needs. */
#if TARGET_ANDROID
        /* LEFT/RIGHT DOUBLES THE WIDTH. Each host sends a half-WIDTH pane there (session.c), so the composite
         * is twice as wide, not twice as tall. Other decoders follow the stream's SPS whatever they are opened
         * at, so elsewhere this stays as it was; MediaCodec on the SHIELD TV sizes its buffers from what it is
         * configured with, and opened at 960x2160 for a 1920x1080 composite it showed a sliver of one pane and
         * green beside it (2026-10-16). */
        if (access("/tmp/coop-vmirror", F_OK) == 0) {
            width *= 2;
        } else
#endif
        height *= 2;
    }
    coop_pump_start();   /* starfish/SMP callbacks need a pumped GLib context */
    vdec_setup_ms = SDL_GetTicks();
    (void) drFlags;
    session = context;
    player = session->player;
    coop_wayland_probe();   /* needs session->app->ui.window, so after this */
    coop_dump_frames = 0;
    coop_bootstrapped = false;
    coop_conceal = access("/tmp/coop-conceal", F_OK) == 0;
    /* Both are ON by default now (see their declarations). The sentinels only DISABLE, so a
     * cleared /tmp can no longer silently undo either fix -- which is exactly how both of them
     * were lost. The old enabling sentinels are still accepted and simply redundant. */
    coop_skipconceal_enabled = access("/tmp/coop-noskipconceal", F_OK) != 0;
    coop_vt_catchup_enabled = access("/tmp/coop-nocatchup", F_OK) != 0;
    /* BOTH ARE NOW TILE-AWARE (2026-08-05), so tiles mode no longer disables them.
     *
     * They used to be forced off here because both built their slices over the COMPOSITE's
     * parameters, and slice_header_end_bit_x() refuses tiled parameters outright -- so in
     * tiles mode they could not have built anything anyway. Both now build over OUR PANE's
     * parameters and re-home through coop_slice_to_tile(), the same primitive the paired
     * path uses for both halves.
     *
     * Turning them off had a MEASURED cost, which is why this matters: without skip-conceal
     * the tile path falls back to the REPEAT floor, and a repeated inter slice replays its
     * residual against a reference it was not coded for -- smear clustered on MOVING content
     * while static areas stay clean (user-observed on the peer's pane, 2026-08-05, with
     * repeated= climbing ~340 per sample against 24 for a whole run with it on). Without
     * catch-up the composite consumes one peer pane per picture and can never drain a
     * backlog, so the peer queue sits deep -- qmax=12 at 40fps is ~300ms of latency ahead of
     * what is on screen, reported as "really high latency".
     *
     * /tmp/coop-noskipconceal and /tmp/coop-nocatchup still turn them off individually, which
     * is the A/B for judging this change. */
    coop_hmirror_enabled = access("/tmp/coop-hmirror", F_OK) == 0;
    coop_fb_nopane = 0;
    coop_fb_mismatch = 0;
    coop_concealed = 0;
    coop_poc_rewrites = 0;
    coop_poc_failed = 0;
    coop_poc_unaligned = 0;
    coop_repeated = 0;
    coop_lastpane_len = 0;
    coop_lastpane_type = -1;
    coop_repeat_pane = access("/tmp/coop-repeat", F_OK) == 0;   /* latched: no syscall per frame */
    coop_rebootstraps = 0;
    coop_stale_run = 0;
    coop_conceal_since_boot = 0;
    coop_hh_nonvcl_len = 0;
    coop_idr_requests = 0;
    coop_hh_mirrored = 0;
    coop_hh_heldbroken = 0;
    coop_spliced_frames = 0;
    /* A UNIQUE FILENAME PER SESSION, BECAUSE A STALE DUMP CANNOT BE CLEARED AND CANNOT BE
     * TOLD APART FROM A FRESH ONE.
     *
     * The dump used to be a fixed /tmp/coop_stream.h265. The dev user (prisoner, uid 6122)
     * cannot delete it -- the app owns it (uid 6584) in a sticky /tmp -- so `rm` before a run
     * silently does nothing, and a run that captures nothing leaves the PREVIOUS run's file
     * sitting there looking exactly like a result. Measured 2026-08-01: after a session that
     * provably streamed at 3840x1088 (Apollo and the client both logged it) the file's md5
     * was unchanged and still described a 3840x2176 stream. Two captures taken for different
     * hosts turned out byte-identical, and a conclusion drawn from them had to be retracted.
     *
     * A per-session name makes staleness impossible to mistake: the path is logged here with
     * the geometry that session negotiated, so the reader always knows which file belongs to
     * which run, and an absent file is absent rather than yesterday's. */
    coop_dump = NULL;
    if (access("/tmp/coop-dump", F_OK) == 0) {
        snprintf(coop_dump_path, sizeof(coop_dump_path), "/tmp/coop_stream_%lu.h265",
                 (unsigned long) time(NULL));
        coop_dump = fopen(coop_dump_path, "wb");
        FILE *lf = fopen(AURORA_DIAG_LOG, "a");
        if (lf != NULL) {
            fprintf(lf, "[COOP-DUMP] %s open=%d for %dx%d\n", coop_dump_path,
                    coop_dump != NULL ? 1 : 0, width, height);
            fclose(lf);
        }
    }
    coop_dump_limit = COOP_DUMP_FRAMES_DEFAULT;
    if (coop_dump != NULL) {
        FILE *lf = fopen("/tmp/coop-dump", "r");
        if (lf != NULL) {
            unsigned want = 0;
            if (fscanf(lf, "%u", &want) == 1 && want > 0 && want <= 20000u) {
                coop_dump_limit = want;
            }
            fclose(lf);
        }
        if (diag_log != NULL) {
            fprintf(diag_log, "[COOP-DUMP] capturing %u access units\n", coop_dump_limit);
            fflush(diag_log);
        }
    }
    coop_dump_out_frames = 0;
    coop_dump_out_reach = 0;
    /* ROLLING CAPTURE. Independent of the head capture above: that one answers
     * "what did the stream look like at the start", this one answers "what was it
     * doing just before I saw the fault" -- which is the question a decay that
     * takes MINUTES actually poses. */
    diag_feed_fault_arm();       /* F2 test only; no-op unless /tmp/coop-feed-fail exists */
    coop_netcap_setup();          /* continuous capture: answers what the ring and the 400-AU
                                   * head capture structurally cannot -- see its header comment */
    if (coop_cap_buf != NULL) { free(coop_cap_buf); coop_cap_buf = NULL; }
    if (coop_cap_idx != NULL) { free(coop_cap_idx); coop_cap_idx = NULL; }
    coop_cap_bytes = coop_cap_w = 0;
    coop_cap_head = coop_cap_count = coop_cap_poll = coop_cap_dumps = 0;
    coop_cap_trigger_mtime = 0;
    /* A pending request must not survive into the next stream, or the first poll of a
     * fresh session flushes a ring belonging to the previous one. */
    coop_cap_want = false;
    coop_cap_want_polls = 0;
    {
        FILE *rf = fopen("/tmp/coop-dump-ring", "r");
        if (rf != NULL) {
            int mb = 0;
            if (fscanf(rf, "%d", &mb) != 1 || mb <= 0) { mb = 96; }
            if (mb > 256) { mb = 256; }        /* the TV has ~464MB free while decoding 4K */
            fclose(rf);
            coop_cap_bytes = (size_t) mb * 1024u * 1024u;
            coop_cap_buf = malloc(coop_cap_bytes);
            coop_cap_idx = calloc(COOP_CAP_MAX_RECS, sizeof(coop_cap_rec_t));
            if (coop_cap_buf == NULL || coop_cap_idx == NULL) {
                free(coop_cap_buf); free(coop_cap_idx);
                coop_cap_buf = NULL; coop_cap_idx = NULL; coop_cap_bytes = 0;
                if (diag_log != NULL) {
                    fprintf(diag_log, "[COOP-DUMP] ring: %dMB allocation FAILED -- "
                                      "rolling capture disabled\n", mb);
                    fflush(diag_log);
                }
            } else if (diag_log != NULL) {
                fprintf(diag_log, "[COOP-DUMP] rolling capture armed: %dMB "
                                  "(~%d AUs at 78KB, touch /tmp/coop-dump-now to write it)\n",
                        mb, (int) (coop_cap_bytes / 78000u));
                fflush(diag_log);
            }
        }
    }
    coop_dump_out = access("/tmp/coop-dump-out", F_OK) == 0
            ? fopen("/tmp/coop_composed.h265", "wb") : NULL;
    coop_dump_out_start = 0;
    coop_dump_out_seen = 0;
    coop_dump_out_open = true;                 /* no offset requested: dump from the start */
    if (coop_dump_out != NULL) {
        FILE *df = fopen("/tmp/coop-dump-out", "r");
        if (df != NULL) {
            unsigned at = 0;
            if (fscanf(df, "%u", &at) == 1 && at > 0) {
                coop_dump_out_start = at;
                coop_dump_out_open = false;    /* wait for the requested keyframe */
            }
            fclose(df);
        }
        if (diag_log != NULL) {
            fprintf(diag_log, "[COOP-DUMP] out capture armed: start=%u open=%d\n",
                    coop_dump_out_start, (int) coop_dump_out_open);
            fflush(diag_log);
        }
    }
    buffer_initial_size = vdec_buffer_initial_bytes();
    buffer_size = buffer_initial_size;
    buffer = malloc(buffer_size);
    if (!buffer) {
        commons_log_error("Session", "Failed to allocate video reassembly buffer (%zu bytes)", buffer_size);
        return CALLBACKS_SESSION_ERROR_VDEC_ERROR;
    }
    memset(&vdec_temp_stats, 0, sizeof(vdec_temp_stats));
    memset(&vdec_stream_info, 0, sizeof(vdec_stream_info));
    memset(&vdec_totals, 0, sizeof(vdec_totals));
    vdec_totals.start_ms = SDL_GetTicks();
    vdec_totals.worst_fps = -1;
    vdec_stream_format = videoFormat;
    vdec_stream_info.format = video_format_name(videoFormat);
    lastFrameNumber = 0;
    frames_since_idr = 0;
    lastIdrRequestMs = 0;
    vdec_stream_target_fps = redrawRate > 0 ? redrawRate : 60;
    diag_feed_ok = 0;
    diag_idr_sent = 0;
    diag_idr_throttled = 0;
    diag_feed_errors = 0;
    diag_last_flush_ms = 0;
    diag_last_snmp_ms = 0;
    diag_session++;
    if (diag_log) { fclose(diag_log); }
    /* ROTATE at 5 MB so /tmp (tmpfs) can't fill up. It used to set diag_log = NULL, i.e. go
     * SILENT FOREVER, and that is unrecoverable from the dev shell: /tmp is sticky
     * (drwxrwxrwt) and the log belongs to the APP's uid (6584) while the ssh user is
     * prisoner (6122), so `rm` and truncation both return "Operation not permitted". Hit on
     * 2026-08-03 with the file at 5,606,017 bytes -- the next session would have produced no
     * counters at all, which reads exactly like a broken build. WE own the file, so opening
     * it "w" truncates where the shell cannot. */
    {
        struct stat _st;
        if (stat(AURORA_DIAG_LOG, &_st) == 0 && _st.st_size > 5 * 1024 * 1024) {
            diag_log = fopen(AURORA_DIAG_LOG, "w");
#ifdef _WIN32
            /* TRUNCATE, THEN APPEND. On Windows stderr appends to this same file (app.c:88), and a
             * "w" stream writes from its own offset -- over every line stderr appended since, so
             * whole sessions of commons_log lines were lost (issue #9). */
            if (diag_log != NULL) {
                fclose(diag_log);
                diag_log = fopen(AURORA_DIAG_LOG, "a");
            }
#endif
        } else {
            diag_log = fopen(AURORA_DIAG_LOG, "a");
        }
        /* OUTSIDE the else: the 5 MB cap disables LOGGING, and must not also disable
         * FUNCTIONALITY. Having the loader in the else branch meant that once the log
         * filled, play-from-file silently stopped working -- and with logging off too,
         * there was nothing to say why. Cost two full build/deploy/test cycles. */
        coop_play_load();
    }
    /* THE PER-PICTURE TRACE OPENS WITH THE SESSION and is gated on its own sentinel, so an
     * ordinary run is byte-unchanged. Passover 07 §3 asked for a BOUNDED trace; the bound and
     * the row cap live in coop_trace.c. */
    coop_trace_open(coop_trace_sample);
    if (diag_log) {
        fprintf(diag_log, "\n=== SESSION %u [%s pid=%d] ===\n",
                diag_session, coop_role_tag(), (int) getpid());
        fprintf(diag_log, "codec=%s res=%dx%d fps=%d\n",
                video_format_name(videoFormat), width, height, redrawRate);

        /* log OS socket buffer limit */
        FILE *rmem = fopen("/proc/sys/net/core/rmem_max", "r");
        if (rmem) {
            char val[32] = {0};
            if (fgets(val, sizeof(val), rmem)) {
                fprintf(diag_log, "rmem_max=%s", val);
            }
            fclose(rmem);
        }

        /* snapshot UDP counters at session start */
        FILE *snmp = fopen("/proc/net/snmp", "r");
        if (snmp) {
            char line[256];
            while (fgets(line, sizeof(line), snmp)) {
                if (line[0] == 'U' && line[1] == 'd' && line[2] == 'p' && line[3] == ':' &&
                    line[5] >= '0' && line[5] <= '9') {
                    fprintf(diag_log, "UDP_START %s", line);
                    break;
                }
            }
            fclose(snmp);
        }

        /* snapshot driver-level counters — captures drops invisible to /proc/net/snmp */
        FILE *netdev = fopen("/proc/net/dev", "r");
        if (netdev) {
            char line[256];
            int skip = 2;
            while (fgets(line, sizeof(line), netdev)) {
                if (skip > 0) { skip--; continue; }
                /* skip loopback */
                const char *p = line;
                while (*p == ' ') p++;
                if (p[0] == 'l' && p[1] == 'o' && p[2] == ':') continue;
                fprintf(diag_log, "DEV_START %s", line);
            }
            fclose(netdev);
        }

        diag_wifi_snap("WIFI_START", SDL_GetTicks());

        fflush(diag_log);
    }
    vdec_warned_near_buffer_limit = false;

    if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
        vdec_stream_info.width = width;
        vdec_stream_info.height = height;
    }

    SS4S_VideoInfo info;
    memset(&info, 0, sizeof(info));
    info.width = width;
    info.height = height;
    info.frameRateNumerator = vdec_stream_target_fps;
    info.frameRateDenominator = 1;
    switch (videoFormat) {
        case VIDEO_FORMAT_H264:
            info.codec = SS4S_VIDEO_H264;
            break;
        case VIDEO_FORMAT_H265:
        case VIDEO_FORMAT_H265_MAIN10:
            info.codec = SS4S_VIDEO_H265;
            break;
        case VIDEO_FORMAT_AV1_MAIN8:
        case VIDEO_FORMAT_AV1_MAIN10:
            info.codec = SS4S_VIDEO_AV1;
            break;
        default: {
            commons_log_error("Session", "Unsupported codec %s", vdec_stream_info.format);
            return CALLBACKS_SESSION_ERROR_VDEC_UNSUPPORTED;
        }
    }

    app_t *app = session->app;
    if (app->ss4s.video_cap.transform & SS4S_VIDEO_CAP_TRANSFORM_UI_EXCLUSIVE) {
        app_bus_post_sync(app, (bus_actionfunc) app_ui_close, &app->ui);
    }

    /* CONTROL: skip the primary NDL open entirely, so the starfish pipeline is the
     * ONLY claimant on a video plane. Every previous test ran with NDL already
     * holding the plane, so "export accepted but black" could never distinguish a
     * broken export from simple plane contention. With /tmp/coop-probe-solo the
     * primary is deliberately not opened: if the starfish pane then displays, the
     * mechanism works and the limit is one plane per app. */
    const bool coop_solo = access("/tmp/coop-probe-solo", F_OK) == 0;
    coop_solo_active = coop_solo;
    coop_measure = access("/tmp/coop-measure", F_OK) == 0;
    coop_splice_test = access("/tmp/coop-splice-test", F_OK) == 0;
    /* Foreground-safety test: bring up the coordinator socket and spawn a
     * headless secondary while the primary streams. The child must NOT take the
     * foreground -- webOS grants it to one app, and losing it would suspend this
     * stream, which is how Multi-View ejected Eclipse when it tried to render.
     * If the primary survives with the child alive, a child process is a safe
     * home for the second session. */
    /* TWO WAYS IN, AND THE UI WINS. coop_session is what the launch popup fills in;
     * /tmp/coop-spawn is the dev-time sentinel run_coop.sh writes over SSH. The
     * sentinel is kept because the entire co-op test corpus drives through it and
     * that harness is what proved both modes -- but a session configured on screen
     * must not be overridden by a stale file left behind by an earlier test run,
     * which is exactly the trap app_webos.c already cleans up after. */
    const coop_session_t *coop_ui_session = coop_session_get();
    if (app_launch_coop_requested() && !coop_spawned &&
        (coop_ui_session != NULL || access("/tmp/coop-spawn", F_OK) == 0)) {
        if (coop_coord_start(COOP_COORD_SOCKET)) {
            char host[64] = {0};
            int app_id = 0;
            if (coop_ui_session != NULL) {
                snprintf(host, sizeof(host), "%s", coop_ui_session->secondary_uuid);
                app_id = coop_ui_session->secondary_app_id;
            } else {
                /* Sentinel form: "<host-uuid> <app-id>". */
                FILE *sf = fopen("/tmp/coop-spawn", "r");
                if (sf != NULL) {
                    if (fscanf(sf, "%63s %d", host, &app_id) != 2) { host[0] = '\0'; }
                    fclose(sf);
                }
            }
            /* Spawn the executable that is actually running. The old webOS install path was
             * assembled here unconditionally, so Windows passed it to CreateProcess and then
             * continued as a misleading one-pane "co-op" stream. Platform path ownership also
             * keeps this correct when a portable Windows folder is renamed or the future Linux
             * package installs somewhere other than /usr/bin. */
            char *self_exe = path_self_executable_alloc();
            const bool have_self = self_exe != NULL;
            int pid = have_self ? coop_coord_spawn_secondary_for(
                    self_exe, COOP_COORD_SOCKET, host[0] ? host : NULL, app_id,
                    session->app->settings.stream.width,
                    session->app->settings.stream.height) : -1;
            if (diag_log != NULL) {
                fprintf(diag_log, "[COOP-SPAWN] coordinator up, secondary pid=%d (%s, exe=%s)\n", pid,
                        coop_ui_session != NULL ? "ui" : "sentinel",
                        have_self ? self_exe : "UNRESOLVED");
                fflush(diag_log);
            }
            free(self_exe);
            if (pid < 0) {
                coop_coord_stop();
                return CALLBACKS_SESSION_ERROR_COOP_START;
            }
            coop_spawned = true;
            bool ready_in_background = false;
#if TARGET_ANDROID
            coop_ready_cancel = 0;
            coop_ready_ui = coop_ui_session != NULL;
            coop_ready_thread_live = pthread_create(&coop_ready_thread, NULL, coop_ready_watch, session) == 0;
            ready_in_background = coop_ready_thread_live;
#endif
            /* A process id only proves CreateProcess/fork succeeded.  Do not let
             * co-op degrade into a plausible one-pane stream: the helper must
             * prove that it queried the requested host, launched the requested
             * app, connected, and forwarded a live frame. */
            if (ready_in_background) {
                /* Android: coop_ready_watch() decides, beside the stream. */
            } else if (!coop_coord_wait_secondary_ready(30000)) {
                if (diag_log != NULL) {
                    fprintf(diag_log, "[COOP-SPAWN] secondary readiness FAILED: %s\n",
                            coop_coord_start_error_message());
                    fflush(diag_log);
                }
                coop_coord_request_peer_shutdown();
                coop_coord_reap_secondary();
                coop_coord_stop();
                coop_spawned = false;
                return CALLBACKS_SESSION_ERROR_COOP_START;
            }
            if (!ready_in_background && diag_log != NULL) {
                fprintf(diag_log, "[COOP-SPAWN] secondary READY with a live frame (%s)\n",
                        coop_ui_session != NULL ? "ui" : "sentinel");
                fflush(diag_log);
            }
        } else if (diag_log != NULL) {
            fprintf(diag_log, "[COOP-SPAWN] coordinator FAILED to start\n");
            fflush(diag_log);
            return CALLBACKS_SESSION_ERROR_COOP_START;
        } else {
            return CALLBACKS_SESSION_ERROR_COOP_START;
        }
    }
    coop_splice_ok = coop_splice_identical = coop_splice_fail = 0;
    coop_live = access("/tmp/coop-live", F_OK) == 0;
    coop_vt_enabled = access("/tmp/coop-vmirror", F_OK) == 0;
    coop_vt_ready = false;
    coop_vt_nonvcl_len = 0;
    coop_vt_frames = 0;
    coop_vt_peer_used = coop_vt_peer_missing = 0;
    coop_vt_lastpeer_len = 0;
    coop_vt_repeated = coop_vt_mirrored = 0;
    coop_vt_chain_broken = false;
    coop_vt_idrreq = coop_vt_mended = coop_vt_last_evicted = 0;
    coop_vt_slicemap = 0;
    coop_vt_concealed = 0;
    coop_vt_sizelog = 0;
    coop_vt_catchup = 0;
    coop_vt_irap_logged = 0;
#ifdef COOP_PC_DECODER
    coop_vt_cu_drop = coop_vt_cu_big = coop_vt_cu_irap = 0;
    coop_vt_demoted = coop_vt_demote_fail = 0;
    coop_vt_idr_stale = 0;
#endif
    coop_vt_poc = 0;
    coop_vt_poc_returned = 0;
    coop_vt_refused = 0;
    coop_vt_poc_init = false;
    coop_hh_poc = 0;
    coop_hh_poc_pending = 0;
    coop_hh_poc_returned = 0;
    coop_hh_poc_init = false;
    coop_hh_last_evicted = 0;
    coop_hh_mended = 0;
    coop_hh_poc_fail = 0;
    coop_hh_tiles = access("/tmp/coop-hh-tiles", F_OK) == 0;
#if TARGET_ANDROID
    coop_zw_aus = coop_zw_words = 0;
#endif
    coop_hh_tiled_ok = false;
    coop_hh_untiled_blocked = 0;
    coop_peer_dump_n = 0;
    coop_peer_dump = access("/tmp/coop-dump-peer", F_OK) == 0
            ? fopen("/tmp/coop_peer.h265", "wb") : NULL;
    if (coop_hh_tiles && diag_log != NULL) {
        fprintf(diag_log, "[COOP-HH] TILE ROWS armed -- conceal and catch-up are LIVE in this "
                          "mode (ported 2026-08-05); skipconceal=/catchup= prove it\n");
        fflush(diag_log);
    }
    coop_hh_cu_gate = coop_hh_cu_qmax = coop_hh_cu_plan = coop_hh_cu_take = coop_hh_cu_build = 0;
    memset(coop_hh_cu_why, 0, sizeof(coop_hh_cu_why));

    /* CO-OP MUST BE ARMED BY THE LAUNCH, NOT BY LEFTOVER FILES.
     *
     * A previous attempt consumed the sentinels here with unlink(). It CANNOT WORK and
     * was verified not to: the app runs as uid 6584 while the sentinels are created
     * over ssh by `prisoner` (6122), and /tmp is sticky, so unlink() is refused and the
     * files survive the session. Any scheme that relies on the app cleaning up /tmp is
     * dead on arrival.
     *
     * So activation is scoped to the launch instead -- `"coop":true` in the -p payload.
     * That is per-run by construction, needs no cleanup, and cannot leak into the next
     * ordinary stream (which is what made a normal game stream render as a half-pane).
     * The sentinels remain as CONFIG and are still read above; they simply no longer
     * decide whether co-op happens. */
    if (!app_launch_coop_requested()) {
        const bool sentinels_present = coop_live;
        coop_live = false;
        if (diag_log != NULL && sentinels_present) {
            fprintf(diag_log, "[COOP] sentinels present but launch did not request co-op"
                              " -- staying solo\n");
            fflush(diag_log);
        }
    }
    coop_live_paired = coop_live_fallback = 0;
    coop_first_real_splice = false;
    /* Per-session, all of it: a stale lastpeer from the previous session would be spliced
     * into the first pictures of this one, and a stale `bootstrapped` would let the
     * 1088-row SPS through again. */
    coop_hh_lastpeer_len = 0;
    coop_hh_lastmine_len = 0;
    coop_hh_chain_broken = false;
    coop_hh_primed = false;
    coop_hh_repeated = coop_hh_typemismatch = coop_hh_suppressed = 0;
    coop_hh_retyped = 0;
    coop_hh_converted = 0;
    coop_hh_repeat_mark = coop_hh_refreshes = 0;
    coop_hh_reparam = coop_hh_skipconceal = coop_hh_irapconceal = 0;
    coop_hh_conceal_since_anchor = coop_hh_conceal_anchors = 0;
    coop_hh_refbad = coop_hh_ppocjump = 0;
    coop_hh_ppoc_last = coop_hh_ppoc_step = 0;
    coop_hh_ppoc_have = false;
    coop_seq_reset(&coop_hh_seq);
    coop_seq_reset(&coop_vt_seq);
    coop_hh_ready = false;
    coop_hh_nonvcl_len = 0;
    if (coop_live && coop_pane_buf == NULL) {
        coop_pane_buf = malloc(DECODER_BUFFER_MAX_SIZE);
        /* CAP ONLY AFTER THE ALLOCATION SUCCEEDS. Setting it first leaves cap != 0 with a NULL
         * buffer on OOM, and every later bounds check reads as satisfied. */
        coop_pane_cap = (coop_pane_buf != NULL) ? DECODER_BUFFER_MAX_SIZE : 0;
    }
    /* coop_vt_enabled needs these too: the live vertical mirror composes into coop_vt_buf
     * and re-addresses through the same helpers, but sets neither coop_live nor
     * coop_splice_test -- without it here the buffers stay NULL and the compose block is
     * skipped silently, which looks exactly like "the feature did not activate". */
    if ((coop_splice_test || coop_live || coop_vt_enabled) && coop_splice_buf == NULL) {
        coop_splice_cap = DECODER_BUFFER_MAX_SIZE;
        coop_splice_buf = malloc(coop_splice_cap);
        coop_poc_cap = coop_splice_cap;
        coop_poc_buf = malloc(coop_poc_cap);
        coop_conv_cap = coop_splice_cap;
        coop_conv_buf = malloc(coop_conv_cap);
        coop_vt_cap = coop_splice_cap;
        coop_vt_buf = malloc(coop_vt_cap);
        coop_vt_lastpeer = malloc(coop_vt_cap);
        coop_vt_lastpeer_len = 0;
        coop_lastpane = malloc(coop_poc_cap);
        coop_lastpane_len = 0;
        coop_lastpane_type = -1;
        memset(&coop_poc_params, 0, sizeof(coop_poc_params));
    }
    coop_prev_ms = 0; coop_samples = 0; coop_delta_sum = 0; coop_delta_max = 0;
    coop_skips = 0; coop_prev_frame = 0;
    memset(coop_hist, 0, sizeof(coop_hist));
    /* THE LETTERBOX IS NOT HERE -- MEASURED, DO NOT RETRY THIS. `width`/`height` are the
     * size the DECODER is set up for, and in half-height co-op that is ALREADY the full
     * composite 3840x2160 (the half-height request in session_config_init changes what we
     * ask the HOST for, not what this callback receives). Doubling it opened the plane at
     * 3840x4320 against 2176-row pictures and the TV went completely black -- logged as
     * `[COOP] video plane opened at 3840x4320 (negotiated 3840x2160)`.
     *
     * Left as a log line so the size is visible in any future letterbox hunt, since two
     * separate attempts have now been spent doubling a number that was already right. */
    if (coop_live && diag_log != NULL) {
        fprintf(diag_log, "[COOP] video plane opened at %dx%d\n", info.width, info.height);
        /* Geometry only. The decode-budget WARNING lives in the periodic block with the
         * delivered bitrate, because pixel rate does not predict the fault (see
         * COOP_DECODE_BUDGET_MBPS). */
        fflush(diag_log);
    }
    vendor_diag_hang_arm("SS4S_PlayerVideoOpen");
    SS4S_VideoOpenResult open_result = coop_solo ? SS4S_VIDEO_OPEN_OK
                                                 : SS4S_PlayerVideoOpen(player, &info);
    vendor_diag_hang_disarm(open_result == SS4S_VIDEO_OPEN_OK ? "OK" : "FAILED");
    if (coop_solo && diag_log) {
        fprintf(diag_log, "[COOP-PROBE] SOLO MODE: primary video NOT opened; starfish only\n");
        fflush(diag_log);
    }
    if (diag_log) {
        /* Log which SS4S module is ACTUALLY selected. The decoder preference
         * accepts either a module id ("smp-webos") or a group ("smp"), and a
         * preference that fails SS4S_ModuleCheck falls back to auto SILENTLY --
         * so the setting alone never tells you what is running. Two runs that
         * looked like "SMP works" vs "SMP fails" were indistinguishable without
         * this. */
        const char *vmod = "(unknown)";
        if (session != NULL && session->app != NULL &&
            session->app->ss4s.selection.video_module != NULL) {
            vmod = session->app->ss4s.selection.video_module->id;
        }
        fprintf(diag_log, "SS4S video module ACTIVE = %s (pref=%s)\n", vmod,
                app_configuration && app_configuration->decoder ? app_configuration->decoder : "?");
        fprintf(diag_log, "SS4S_PlayerVideoOpen result=%d (0=OK 1=unsupported 2=error)\n", (int) open_result);
        fflush(diag_log);
    }
    if (open_result == SS4S_VIDEO_OPEN_OK) {
        coop_second_decoder_probe(info);   /* no-op unless /tmp/coop-probe exists */
    }
    switch (open_result) {
        case SS4S_VIDEO_OPEN_OK:
            return 0;
        case SS4S_VIDEO_OPEN_UNSUPPORTED_CODEC:
            return CALLBACKS_SESSION_ERROR_VDEC_UNSUPPORTED;
        default:
            return CALLBACKS_SESSION_ERROR_VDEC_ERROR;
    }
}

void vdec_delegate_cleanup() {
    coop_netcap_teardown();   /* stop the capture thread BEFORE anything it reads goes away */
    coop_pump_stop();
#if TARGET_ANDROID
    if (coop_ready_thread_live) {
        coop_ready_cancel = 1;              /* the readiness watch must not outlive the session it would end */
        pthread_join(coop_ready_thread, NULL);
        coop_ready_thread_live = false;
    }
#endif
    if (coop_spawned) {
        coop_coord_request_peer_shutdown();
        coop_coord_reap_secondary();   /* never leave a stale child behind */
        coop_coord_stop();
        coop_spawned = false;
    }
    free(coop_splice_buf);
    coop_splice_buf = NULL;
    coop_splice_cap = 0;
    free(coop_pane_buf);
    coop_pane_buf = NULL;
    coop_pane_cap = 0;
    free(coop_conv_buf);
    coop_conv_buf = NULL;
    coop_conv_cap = 0;
    free(coop_vt_buf);
    coop_vt_buf = NULL;
    coop_vt_cap = 0;
    free(coop_vt_lastpeer);
    coop_vt_lastpeer = NULL;
    coop_vt_lastpeer_len = 0;
#if HAVE_STARFISH_DIRECT
    coop_mirror_teardown();   /* never leak a pipeline/window across streams */
#endif
    assert(player != NULL);
    free(buffer);
    buffer = NULL;
    buffer_size = 0;
    buffer_initial_size = 0;
    SS4S_PlayerVideoClose(player);
    session = NULL;
    if (diag_log) {
        fprintf(diag_log, "=== session ended: ok=%u idr_sent=%u idr_throttled=%u errors=%u ===\n",
                diag_feed_ok, diag_idr_sent, diag_idr_throttled, diag_feed_errors);
#if TARGET_ANDROID
        if (coop_zw_aus > 0) {
            fprintf(diag_log, "[COOP] cabac_zero_words removed before a slice: %u pictures, %u words\n",
                    coop_zw_aus, coop_zw_words);
        }
#endif

        /* snapshot UDP counters at session end — diff vs UDP_START shows drops */
        FILE *snmp = fopen("/proc/net/snmp", "r");
        if (snmp) {
            char line[256];
            while (fgets(line, sizeof(line), snmp)) {
                if (line[0] == 'U' && line[1] == 'd' && line[2] == 'p' && line[3] == ':' &&
                    line[5] >= '0' && line[5] <= '9') {
                    fprintf(diag_log, "UDP_END   %s", line);
                    break;
                }
            }
            fclose(snmp);
        }

        /* driver-level counters at session end */
        FILE *netdev = fopen("/proc/net/dev", "r");
        if (netdev) {
            char line[256];
            int skip = 2;
            while (fgets(line, sizeof(line), netdev)) {
                if (skip > 0) { skip--; continue; }
                const char *p = line;
                while (*p == ' ') p++;
                if (p[0] == 'l' && p[1] == 'o' && p[2] == ':') continue;
                fprintf(diag_log, "DEV_END   %s", line);
            }
            fclose(netdev);
        }

        diag_wifi_snap("WIFI_END", SDL_GetTicks());

        fclose(diag_log);
        diag_log = NULL;
    }
    /* Closed AFTER the diag log so the trace's own "# closed at" line is the last thing
     * written: a trace that ends without it was cut short by a crash, not by a session end,
     * and that distinction is the first thing a reader needs. */
    coop_trace_close();
}

/* Observe ONE peer pane exactly as its encoder wrote it, for the divergence detectors.
 *
 * MUST BE CALLED WHEREVER A PANE IS TAKEN FROM THE COORDINATOR, not only on the main compose
 * path. Measured 2026-08-17, when it was only on the main path: `ppocjump` reached 233 purely
 * because the CATCH-UP loop takes panes through its own coop_coord_take_next() and the tracker
 * never saw them, so the next pane it DID see was several POCs ahead and every one read as a
 * jump. The proof is in the per-phase deltas -- ppocjump moved only when `catchup` moved, and
 * sat still through all 22,800 composites of S2, where catch-up was idle. The detector was
 * measuring its own blind spot, not the peer.
 *
 * Call it BEFORE any of our own edits (retype, IDR->TRAIL conversion, POC rewrite), or it
 * measures us instead of the peer. Read-only: counters and a log line, nothing acts on these.
 */
static void coop_hh_observe_peer(const coop_au_t *p, uint32_t frame) {
    if (p == NULL || p->slice[0] == NULL || p->slice_len[0] == 0 ||
        !coop_hh_peer_params.valid) {
        return;
    }
    const int dt = (p->slice[0][0] >> 1) & 0x3F;
    const bool d_irap = dt >= 16 && dt <= 23;

    /* (A) Which picture does this slice predict from? Only delta -1 is honourable. */
    coop_slice_info_t di;
    if (!d_irap && coop_slice_refcheck(&coop_hh_peer_params, p->slice[0],
                                       p->slice_len[0], &di) && di.slice_type != 2) {
        const bool bad = di.rps_unresolved || di.first_neg_used == 0 ||
                         di.first_neg_delta != 1u || di.ref_l0_active > 1u;
        if (bad) {
            coop_hh_refbad++;
            if (diag_log != NULL && coop_hh_refbad % 20 == 1) {
                fprintf(diag_log,
                        "[COOP-REF] peer slice references something the composite cannot "
                        "honour: delta=-%u used=%u l0active=%u unresolved=%u "
                        "(only delta -1 is safe) frame=%u poc=%u (#%u)\n",
                        di.first_neg_delta, di.first_neg_used, di.ref_l0_active,
                        di.rps_unresolved, frame, coop_hh_poc, coop_hh_refbad);
                fflush(diag_log);
            }
        }
    }

    /* (B) Did the peer's OWN coded POC stay on its established step? An IRAP restarts the
     * peer's POC, so the tracker rearms rather than reporting it. */
    unsigned dpoc = 0;
    if (d_irap) {
        coop_hh_ppoc_have = false;
        coop_hh_ppoc_step = 0;
        return;
    }
    if (!coop_poc_read(&coop_hh_peer_params, p->slice[0], p->slice_len[0], &dpoc)) {
        return;
    }
    const unsigned mod = 1u << coop_hh_peer_params.log2_max_poc_lsb;
    if (coop_hh_ppoc_have) {
        const unsigned step = (dpoc + mod - coop_hh_ppoc_last) % mod;   /* modulo: a wrap is not a jump */
        if (coop_hh_ppoc_step == 0) {
            coop_hh_ppoc_step = step;
            if (diag_log != NULL) {
                fprintf(diag_log, "[COOP-REF] peer POC step learned: %u (mod %u)\n", step, mod);
                fflush(diag_log);
            }
        } else if (step != coop_hh_ppoc_step) {
            coop_hh_ppocjump++;
            if (diag_log != NULL && coop_hh_ppocjump % 20 == 1) {
                fprintf(diag_log,
                        "[COOP-REF] peer POC step %u, expected %u (%u -> %u) frame=%u (#%u)\n",
                        step, coop_hh_ppoc_step, coop_hh_ppoc_last, dpoc, frame,
                        coop_hh_ppocjump);
                fflush(diag_log);
            }
            /* Re-anchor on the new value so ONE jump reports once rather than making every
             * later picture look wrong too. */
        }
    }
    coop_hh_ppoc_last = dpoc;
    coop_hh_ppoc_have = true;
}

/* THE COMPOSITE RATE LINE, SHARED BY BOTH COMPOSITION PATHS.
 * It was written inside the HEVC branch and stayed there, so an H.264 co-op session emitted
 * NO [COOP-RATE] AT ALL -- and stress_coop.sh scrapes exactly that line for its per-phase
 * rate series. Measured 2026-08-31: a full 14-row rate-mismatch matrix ran to completion
 * against an AVC session and every single phase recorded `0 lines` of rate evidence, i.e.
 * the quantitative half of the run was blind while the screenshots looked fine. That is the
 * SAME failure the comment at the top of stress_coop.sh records for the vertical mode; it
 * just had a third path to reappear in. Nothing here is codec-specific: both paths maintain
 * coop_live_paired and coop_coord_received(). */
static void coop_emit_rate(FILE *diag_log) {
    if (diag_log == NULL) { return; }
        static uint64_t hh_stat_last_ns = 0;
        static unsigned hh_stat_last_paired = 0, hh_stat_last_recv = 0;
        uint64_t hh_now = coop_now_ns();
        if (hh_stat_last_ns == 0) {
            hh_stat_last_ns = hh_now;
            hh_stat_last_paired = coop_live_paired;
            hh_stat_last_recv = coop_coord_received();
        } else if (hh_now - hh_stat_last_ns >= 2000000000ull) {
            double dt = (double) (hh_now - hh_stat_last_ns) / 1e9;
            /* A COUNTER THAT WENT BACKWARDS IS A RESET, NOT A RATE. Both counters
             * restart when the session is torn down and rebuilt (the PIN sign-in's
             * HDR relaunch does exactly that mid-run), and the unsigned subtraction
             * then wraps. Observed 2026-08-12:
             *   d_paired=4294966510 ... compose_fps=602089745.5
             * which is not a fast composite, it is -786 frames. Left alone it puts
             * a garbage line in the middle of the one log anybody reads to judge
             * whether the peer is keeping up. Report the reset and skip the sample. */
            const bool wrapped = coop_live_paired < hh_stat_last_paired ||
                 coop_coord_received() < hh_stat_last_recv;
            if (wrapped) {
        fprintf(diag_log, "[COOP-RATE] counters reset (paired %u->%u, recv "
                  "%u->%u) -- rebasing, no sample this tick\n",
                hh_stat_last_paired, coop_live_paired,
                hh_stat_last_recv, coop_coord_received());
        hh_stat_last_ns = hh_now;
        hh_stat_last_paired = coop_live_paired;
        hh_stat_last_recv = coop_coord_received();
        goto hh_rate_done;
            }
            unsigned d_paired = coop_live_paired - hh_stat_last_paired;
            unsigned d_recv = coop_coord_received() - hh_stat_last_recv;
            fprintf(diag_log,
            "[COOP-RATE] t=%.3f dt=%.3f d_paired=%u d_recv=%u "
            "compose_fps=%.1f peer_fps=%.1f ratio=%.3f\n",
            (double) hh_now / 1e9, dt, d_paired, d_recv,
            d_paired / dt, d_recv / dt,
            d_paired ? (double) d_recv / d_paired : 0.0);
            hh_stat_last_ns = hh_now;
            hh_stat_last_paired = coop_live_paired;
            hh_stat_last_recv = coop_coord_received();
            hh_rate_done:;
        }
    }

int vdec_delegate_submit(PDECODE_UNIT decodeUnit) {
    coop_hh_tiled_ok = false;      /* per picture; only the tiled splice sets it */
    if ((size_t) decodeUnit->fullLength > buffer_size) {
        if ((size_t) decodeUnit->fullLength > DECODER_BUFFER_MAX_SIZE) {
            commons_log_error("Session", "Decode unit %d bytes exceeds %zu byte cap, dropping",
                              decodeUnit->fullLength, (size_t) DECODER_BUFFER_MAX_SIZE);
            return DR_NEED_IDR;
        }
        size_t new_size = buffer_size > 0 ? buffer_size : buffer_initial_size;
        if (new_size == 0) {
            new_size = vdec_buffer_initial_bytes();
        }
        while (new_size < (size_t) decodeUnit->fullLength) {
            new_size *= 2;
        }
        unsigned char *new_buffer = realloc(buffer, new_size);
        if (new_buffer == NULL) {
            commons_log_error("Session", "Failed to grow decode buffer to %zu bytes", new_size);
            return DR_NEED_IDR;
        }
        commons_log_info("Session", "Grew decode buffer %zu -> %zu bytes (frame needed %d)",
                         buffer_size, new_size, decodeUnit->fullLength);
        buffer = new_buffer;
        buffer_size = new_size;
    }
    unsigned long ticksms = SDL_GetTicks();
    if (coop_measure) { coop_measure_sample2(ticksms, decodeUnit->frameNumber); }
    if (lastFrameNumber <= 0) {
        vdec_temp_stats.measurementStartTimestamp = ticksms;
        lastFrameNumber = decodeUnit->frameNumber;
    } else {
        // Any frame number greater than m_LastFrameNumber + 1 represents a dropped frame
        vdec_temp_stats.networkDroppedFrames += decodeUnit->frameNumber - (lastFrameNumber + 1);
        vdec_temp_stats.totalFrames += decodeUnit->frameNumber - (lastFrameNumber + 1);
        lastFrameNumber = decodeUnit->frameNumber;
    }
    unsigned stats_window_ms = streaming_stats_shown() ? 1000u : 2000u;
    if (ticksms - vdec_temp_stats.measurementStartTimestamp > stats_window_ms) {
        vdec_stat_submit(&vdec_temp_stats, ticksms);

        // Move this window into the last window slot and clear it for next window
        memset(&vdec_temp_stats, 0, sizeof(vdec_temp_stats));
        vdec_temp_stats.measurementStartTimestamp = ticksms;
    }

    vdec_temp_stats.receivedFrames++;
    vdec_temp_stats.totalFrames++;
    vdec_temp_stats.receivedBytes += (uint64_t) decodeUnit->fullLength;

    /* THE PRIMARY'S OWN SOURCE IDENTITY AND ARRIVAL STAMPS -- passover 07 §3.1. Read here
     * because moonlight frees the decode unit when this callback returns, and the trace is
     * written later from inside diag_feed. `presentationTimeMs` is capture-derived (epoch =
     * the session's first captured frame); `receiveTimeMs`/`enqueueTimeMs` are this process's
     * LiGetMillis epoch and must never be differenced against the peer's. */
    coop_trace_prim_frame = (uint32_t) decodeUnit->frameNumber;
    coop_trace_prim_pts = (uint32_t) decodeUnit->presentationTimeMs;
    coop_trace_prim_recv = (uint32_t) decodeUnit->receiveTimeMs;
    coop_trace_prim_reasm = (uint32_t) decodeUnit->enqueueTimeMs;
    coop_trace_prim_host = decodeUnit->frameHostProcessingLatency;

    vdec_temp_stats.totalCaptureLatency += decodeUnit->frameHostProcessingLatency;
    vdec_temp_stats.totalReassemblyTime += decodeUnit->enqueueTimeMs - decodeUnit->receiveTimeMs;
    vdec_stream_info.has_host_latency |= decodeUnit->frameHostProcessingLatency > 0;
    if (!vdec_warned_near_buffer_limit && buffer_initial_size > 0 &&
        (size_t) decodeUnit->fullLength > (buffer_initial_size * 9 / 10)) {
        vdec_warned_near_buffer_limit = true;
        commons_log_warn("Session", "Video frame size %d is near initial decoder buffer (%zu)",
                         decodeUnit->fullLength, buffer_initial_size);
    }
    size_t length = 0;
    PLENTRY entry = decodeUnit->bufferList;
    if (entry != NULL && entry->next == NULL) {
        memcpy(buffer, entry->data, entry->length);
        length = (size_t) entry->length;
    } else {
        for (; entry != NULL; entry = entry->next) {
            memcpy(buffer + length, entry->data, entry->length);
            length += entry->length;
        }
    }
    /* PLAY FROM FILE: replace the network access unit with ones from /tmp/coop-play.h265.
     *
     * The point is to put a bitstream WE constructed in front of the TV'S OWN DECODER.
     * ffmpeg accepting a picture proves it is legal HEVC; it does not prove this firmware
     * agrees, and this firmware has disagreed before. Access units are split on VPS (NAL
     * type 32), which is how our generated files begin each one, and cycled so the stream
     * keeps returning to its keyframe. Gated on /tmp/coop-playfile. */
    if (coop_play_au_count > 0) {
        size_t idx = coop_play_next % coop_play_au_count;
        coop_play_next++;
        size_t off = coop_play_off[idx], plen = coop_play_len[idx];
        if (plen > 0 && plen <= buffer_size) {
            memcpy(buffer, coop_play_data + off, plen);
            length = plen;
            if (diag_log != NULL && coop_play_next <= 4) {
                fprintf(diag_log, "[COOP-PLAY] fed AU %zu/%zu (%zu bytes)\n",
                        idx + 1, coop_play_au_count, plen);
                fflush(diag_log);
            }
        }
    }
    SS4S_VideoFeedFlags flags = SS4S_VIDEO_FEED_DATA_FRAME_START | SS4S_VIDEO_FEED_DATA_FRAME_END;
    if (decodeUnit->frameType == FRAME_TYPE_IDR) {
        flags |= SS4S_VIDEO_FEED_DATA_KEYFRAME;
    }
    /* Gated capture of the exact bitstream the host sends, so it can be analysed
     * with tests/coop/hevc_stitch.py (slices per picture, boundary, parameter
     * sets). This is how we verify Apollo really emits the 2 rectangular slices
     * the splice design depends on -- no Apollo source needed. */
    if (coop_dump != NULL) {
        if (coop_dump_frames < coop_dump_limit) {
            fwrite(buffer, 1, length, coop_dump);
            coop_dump_frames++;
        } else if (coop_dump_frames == coop_dump_limit) {
            fflush(coop_dump);
            fclose(coop_dump);
            coop_dump = NULL;
            coop_dump_frames++;
        }
    }
    /* Solo mode (sentinel PRESENT) skips the primary feed so the starfish pipeline
     * is the only claimant on a plane. Absent -- i.e. normal use -- we must FEED.
     * This condition was inverted, so normal streams silently fed nothing and
     * reported success: UI fine, ok= climbing, errors=0, black picture. */
    /* CO-OP SPLICE: replace slice 1 with the secondary's pane, so one picture
     * carries both games and the single existing decoder renders the split.
     * Falls back to the primary's own slice 1 whenever a pane is missing or
     * unusable -- a late secondary must degrade to a normal single-pane picture,
     * never to a broken one. */
    /* LIVE VERTICAL MIRROR: our own pane into BOTH tile columns, on the real desktop.
     *
     * Walks the slices itself rather than using coop_au_parse, which only holds TWO --
     * a pane emits one per requested slice (4 by default) and a tile column must carry
     * all of them. Each slice keeps its position within its tile because a tile column's
     * CTU scan IS a narrow picture's raster scan; only the address changes, mapped
     * through the composite's wider rows as row*Wc + side*Wp + col. */
    if (coop_vt_enabled && coop_vt_buf != NULL) {
        coop_au_t vau;
        static bool vt_said = false;
        if (!coop_vt_ready && diag_log != NULL && !vt_said) {
            /* Say WHY on the first frame. A silent init failure is indistinguishable
             * from "the feature never armed", which already cost one deploy cycle. */
            coop_au_t probe;
            const bool parsed = coop_au_parse(buffer, length, &probe);
            fprintf(diag_log, "[COOP-VT] init: parsed=%d non_vcl=%zu slices=%d len=%d\n",
                    (int) parsed, parsed ? probe.non_vcl_len : (size_t) 0,
                    parsed ? probe.slice_count : -1, (int) length);
            fflush(diag_log);
            vt_said = true;
        }
        if (coop_au_parse(buffer, length, &vau) && vau.non_vcl != NULL && !coop_vt_ready) {
            coop_vt_nonvcl_len = coop_nonvcl_tile_columns(vau.non_vcl, vau.non_vcl_len,
                                                          coop_vt_nonvcl,
                                                          sizeof(coop_vt_nonvcl));
            if (coop_vt_nonvcl_len > 0 &&
                coop_poc_params_parse(vau.non_vcl, vau.non_vcl_len, &coop_vt_src) &&
                coop_vt_src.valid &&
                coop_poc_params_parse(coop_vt_nonvcl, coop_vt_nonvcl_len, &coop_vt_comp) &&
                coop_vt_comp.valid && coop_vt_comp.tiles_enabled &&
                coop_vt_comp.w_ctbs == coop_vt_src.w_ctbs * 2) {
                coop_vt_ready = true;
                if (diag_log != NULL) {
                    fprintf(diag_log, "[COOP-VT] tiled composite %ux%u CTBs from pane "
                                      "%ux%u, addr_bits %u->%u\n",
                            coop_vt_comp.w_ctbs, coop_vt_comp.h_ctbs,
                            coop_vt_src.w_ctbs, coop_vt_src.h_ctbs,
                            coop_vt_src.slice_addr_bits, coop_vt_comp.slice_addr_bits);
                    fflush(diag_log);
                }
            }
            if (!coop_vt_ready && diag_log != NULL) {
                fprintf(diag_log, "[COOP-VT] init REJECTED: nvlen=%zu srcvalid=%d "
                                  "compvalid=%d tiles=%d wsrc=%u wcomp=%u\n",
                        coop_vt_nonvcl_len, (int) coop_vt_src.valid,
                        (int) coop_vt_comp.valid, (int) coop_vt_comp.tiles_enabled,
                        coop_vt_src.w_ctbs, coop_vt_comp.w_ctbs);
                fflush(diag_log);
            }
        }
        /* TILE 1 = THE PEER, when there is one. The mirror (our own pane in both columns)
         * stays as the fallback for a session with NO coordinator, so vertical is still
         * exercisable with a single machine, which is how it was developed and verified.
         * With a peer connected the mirror is never the answer -- see the hold below. */
        /* OUR first VCL slice -- the picture's type and POC come from it. buffer+4 is the
         * VPS: an access unit opens with the parameter sets, so reading the type there gives
         * 32 and the POC read simply fails. That mistake blacked out the peer column.
         * READ BEFORE THE PEER TAKE: coop_seq_plan needs to know whether OUR picture is an
         * IRAP, because that decides whether only a peer IDR may pair with it. */
        const unsigned char *vt_mine = NULL;
        size_t vt_mine_len = 0;
        for (size_t i = 0; i + 5 < length; ++i) {
            if (buffer[i] == 0 && buffer[i+1] == 0 && buffer[i+2] == 0 && buffer[i+3] == 1) {
                if ((int) ((buffer[i+4] >> 1) & 0x3F) <= 31) {
                    vt_mine = buffer + i + 4;
                    size_t j = i + 4;
                    while (j + 4 <= length &&
                           !(buffer[j] == 0 && buffer[j+1] == 0 && buffer[j+2] == 0 && buffer[j+3] == 1)) { j++; }
                    vt_mine_len = ((j + 4 <= length) ? j : length) - (i + 4);
                    break;
                }
                i += 3;
            }
        }
        const int vt_mine_type = (vt_mine != NULL) ? (int) ((vt_mine[0] >> 1) & 0x3F) : -1;
        const bool vt_mine_irap = vt_mine_type >= 16 && vt_mine_type <= 23;
        size_t vt_peer_len = 0;
        coop_au_t vt_peer;
        bool vt_have_peer = false;
        bool vt_conceal = false;
        const unsigned char *vt_peer_src = NULL;
        /* DRAIN THE PEER'S BACKLOG WITH CATCH-UP PICTURES -- docs/coop-rate-decoupling.md.
         * Emitted BEFORE the normal picture so they take the LOWER POCs and the normal
         * picture then pairs with the freshest pane. Each pairs the peer's next pane with a
         * concealed primary tile, which is the mirror of the concealment the peer already
         * gets. Safe against a single reference (Apollo pins numRefL0=1, docs/stack-profile):
         * the next picture's reference is this one, whose tile 0 is a bit-exact copy of the
         * previous primary tile and whose tile 1 is the peer's real next pane.
         * Runs the SAME sequencer as the normal take, so its anchor cannot desync. */
        if (coop_vt_catchup_enabled && coop_vt_ready && coop_live && coop_coord_connected() &&
            coop_pane_buf != NULL && vt_mine != NULL && vt_mine_len > 0 && !vt_mine_irap) {
            unsigned made = 0;
            if (coop_coord_queued() >= COOP_VT_CATCHUP_START) {
            while (made < COOP_VT_CATCHUP_MAX &&
                   coop_coord_queued() > COOP_VT_CATCHUP_KEEP) {
                coop_seq_plan_t cp;
                coop_seq_verdict_t cv;
                memset(&cp, 0, sizeof(cp));
                memset(&cv, 0, sizeof(cv));
                coop_seq_plan(&coop_vt_seq, false, &cp);
                if (cp.kind != COOP_SEQ_TAKE_NEXT) { break; }
#ifdef COOP_PC_DECODER
                /* A PEER IDR IS NOT CATCH-UP'S TO TAKE. The builder refuses an IRAP (it cannot
                 * sit beside our all-skip tile) -- but only AFTER take_next has removed it, so
                 * the one picture that re-anchors the peer's chain was lost (cudrop) and every
                 * later pane predicted from a picture the decoder never had: blocks that stay
                 * in static areas (issue #27, runs W4/W5). Stop in front of it; the normal path
                 * below takes it and converts it. */
                if (coop_coord_next_is_idr(cp.min_frame)) { coop_vt_cu_irap++; break; }
#endif
                uint32_t cf = 0;
                unsigned cd = 0;
                size_t cl = coop_coord_take_next(coop_pane_buf, coop_pane_cap,
                                                 cp.min_frame, &cf, &cd);
                coop_seq_verdict(&coop_vt_seq, &cp, cl > 0, cf, cd, &cv);
                if (cv.drop_lastpeer) {
                    coop_vt_lastpeer_len = 0;
                    if (cd > 0) { coop_vt_chain_broken = true; }
                }
                if (cv.request_idr_keep) { coop_coord_request_peer_idr_keep(); }
#ifdef COOP_PC_DECODER
                if (cl == 0 || !cv.accept) { if (cl > 0) { coop_vt_cu_drop++; } break; }
#else
                if (cl == 0 || !cv.accept) { break; }
#endif
                const unsigned cpoc = ++coop_vt_poc;
                size_t cn = coop_vt_build_catchup(vt_mine, vt_mine_len, coop_pane_buf, cl,
                                                  cpoc, coop_vt_catch_buf,
                                                  sizeof(coop_vt_catch_buf));
#ifdef COOP_PC_DECODER
                if (cn == 0) { coop_vt_poc--; coop_vt_cu_drop++; break; }
#else
                if (cn == 0) { coop_vt_poc--; break; }
#endif
                if (coop_vt_lastpeer != NULL && cl <= coop_vt_cap) {
                    memcpy(coop_vt_lastpeer, coop_pane_buf, cl);
                    coop_vt_lastpeer_len = cl;
                }
                diag_feed(FEED_SITE_VT_CATCHUP, coop_vt_catch_buf, cn, flags);
                coop_vt_catchup++;
                made++;
            }
            }
        }
        if (coop_vt_ready && coop_live && coop_coord_connected() && coop_pane_buf != NULL) {
            /* SEQUENCE THE PEER WITH coop_seq -- THE SAME STATE MACHINE HORIZONTAL USES.
             *
             * Vertical was written independently and never adopted it, and every peer defect
             * found on 2026-08-03 turned out to be a decision this module already makes
             * correctly and has offline tests for (tests/coop/test_coop_seq.c):
             *   - our IRAP may ONLY pair with a peer IDR, because every VCL NAL of a picture
             *     must share nal_unit_type. Hand-rolled, vertical emitted our type-19 slice
             *     beside the peer's type-1 -- ffmpeg says "Non-matching NAL types of the VCL
             *     NALUs: 19 1" -- and an IDR clears the DPB, so OUR keyframe wiped the peer's
             *     tile and it crawled back over ~165ms.
             *   - the sticky IDR is for a planned (re-)anchor, never a gap filler.
             *   - `min_frame` anchoring, so a hole is detected instead of spliced over.
             *   - keep-vs-clearing IDR request policy, whose livelock is documented there.
             * This is an OUTDATED MISMATCH, not a vertical-specific need: adopt it rather
             * than keep reinventing it. */
            coop_seq_plan_t plan;
            coop_seq_verdict_t v;
            memset(&plan, 0, sizeof(plan));
            memset(&v, 0, sizeof(v));
            coop_seq_plan(&coop_vt_seq, vt_mine_irap, &plan);
            uint32_t pf = 0;
            unsigned dropped = 0;
            bool got = false;
            if (plan.kind == COOP_SEQ_TAKE_IDR) {
                if (plan.request_idr_clearing) {
                    coop_coord_request_peer_idr();          /* clearing: drop the stale one */
                }
#ifdef COOP_PC_DECODER
                const uint32_t vt_idr_frame = coop_coord_idr_frame();
#endif
                vt_peer_len = coop_coord_take_idr(coop_pane_buf, coop_pane_cap);
#ifdef COOP_PC_DECODER
                /* NEVER PAIR WITH A PEER IDR THE COMPOSITE HAS ALREADY MOVED PAST. The clearing
                 * request above is the only guard against a stale sticky IDR, and it does not
                 * run when it matters: it is armed once per bootstrap and re-armed only by a
                 * PAIRED IRAP or a #42 demotion, so a session whose first IRAP went out mirrored
                 * (the peer not up yet) never re-arms it; and it is throttled as one unit with
                 * every other peer-IDR request. The 10-04 session shows the result: our host's
                 * mid-session IDR (its game going fullscreen) left demoted=0, demotefail=0 and
                 * mirrored unchanged, which only a sticky-IDR pairing does -- and the peer's
                 * last IDR before it was 36 s old. Pairing resets the peer's tile to that
                 * picture, every later pane predicts from the peer's CURRENT one, and since a
                 * pairing is not a break nothing asks for a keyframe: an old frozen pane with
                 * motion smeared over it until the peer next IDRs on its own. Leave it, and the
                 * unpaired path below asks for a fresh IDR while #42 folds ours into a TRAIL
                 * picture so the peer's chain carries on. Only where #42 can fold ours: a last
                 * pane to repeat (else the tile is mirrored) and a single-slice IDR (else the
                 * fold refuses and FFmpeg rejects the picture -- both worse than stale). */
                coop_au_t vt_mine_chk;
                if (vt_peer_len > 0 && coop_vt_lastpeer_len > 0 && coop_vt_poc_init &&
                    coop_seq_idr_superseded(&coop_vt_seq, vt_idr_frame) &&
                    coop_au_parse(buffer, length, &vt_mine_chk) && vt_mine_chk.slice_count == 1) {
                    if (coop_vt_idr_stale++ == 0 && diag_log != NULL) {
                        fprintf(diag_log, "[COOP-VT] our IRAP found only a superseded peer IDR "
                                          "(frame %u, composite past %u) -- not paired\n",
                                (unsigned) vt_idr_frame, (unsigned) (coop_vt_seq.peer_next - 1u));
                        fflush(diag_log);
                    }
                    vt_peer_len = 0;
                }
#endif
                got = vt_peer_len > 0;
            } else {
                vt_peer_len = coop_coord_take_next(coop_pane_buf, coop_pane_cap,
                                                   plan.min_frame, &pf, &dropped);
                got = vt_peer_len > 0;
            }
            coop_seq_verdict(&coop_vt_seq, &plan, got, pf, dropped, &v);
            if (!v.accept) {
                vt_peer_len = 0;                            /* a hole: conceal, do not splice */
            }
            if (v.drop_lastpeer) {
                coop_vt_lastpeer_len = 0;
                /* ONLY A PANE WE DISCARDED IS A REAL BREAK -- the same conclusion horizontal
                 * reached: Apollo advances its frame counter for pictures it never encoded,
                 * so a numbering gap alone is not a missing reference. `dropped` is the queue
                 * having overflowed, which is. */
                if (dropped > 0) {
                    coop_vt_chain_broken = true;
                }
            }
            if (v.request_idr_keep) {
                coop_coord_request_peer_idr_keep();         /* waiting: keep what arrives */
            }
            const bool vt_from_idr = (plan.kind == COOP_SEQ_TAKE_IDR) && got;
            vt_have_peer = vt_peer_len > 0 &&
                           coop_au_parse(coop_pane_buf, vt_peer_len, &vt_peer) &&
                           vt_peer.slice[0] != NULL;
            /* A GAP MUST HOLD THE PEER'S COLUMN, NOT BORROW OURS. The mirror fallback below
             * is right with no peer at all and catastrophic with one: our pane decoded into
             * tile 1 leaves every later peer inter slice predicting from content the
             * composite never held. Repeating the peer's last access unit keeps the picture
             * whole and the tile's own history intact. It is the same floor horizontal
             * settled on -- an inter slice replayed against a different reference applies
             * its residual twice, so it is soft, not shredded -- and a repeated IRAP costs
             * nothing at all because its slices carry no references. */
            const bool vt_fresh = vt_have_peer && !vt_from_idr;
            if (vt_from_idr && vt_have_peer) { coop_vt_repeated++; }
            if (vt_have_peer) {
                vt_peer_src = coop_pane_buf;
                if (coop_vt_lastpeer != NULL && vt_peer_len <= coop_vt_cap) {
                    memcpy(coop_vt_lastpeer, coop_pane_buf, vt_peer_len);
                    coop_vt_lastpeer_len = vt_peer_len;
                }
            } else if (coop_vt_lastpeer != NULL && coop_vt_lastpeer_len > 0 &&
                       coop_au_parse(coop_vt_lastpeer, coop_vt_lastpeer_len, &vt_peer) &&
                       vt_peer.slice[0] != NULL) {
                vt_peer_src = coop_vt_lastpeer;
                vt_peer_len = coop_vt_lastpeer_len;
                vt_have_peer = true;
                coop_vt_repeated++;
                /* PREFER THE SYNTHESISED SKIP SLICE over this repeat. The repeat stays as the
                 * fallback for the cases the builder refuses (an I slice has no skip syntax),
                 * exactly as horizontal keeps it. Gated on /tmp/coop-skipconceal so it stays
                 * an A/B rather than an assumption. */
                vt_conceal = coop_skipconceal_enabled;
            } else {
                /* Before the peer's FIRST pane there is no history to protect, so mirroring
                 * ours is the only way to emit a whole picture. Counted separately: any
                 * mirrored picture after the peer is established would be a real defect. */
                coop_vt_mirrored++;
            }
            /* WALK THE PEER'S SLICES WITH THE PEER'S OWN PARAMETERS. PC2 is a different GPU
             * (3070 vs 4070) and its SPS/PPS need not match ours bit for bit -- addr_bits,
             * log2_max_poc_lsb and the short-term RPS count all steer the header walk. Using
             * our parameters misparses its header, coop_slice_to_tile refuses, the whole
             * compose is abandoned and we fall through with our OWN 1920-wide pane -- which
             * on a 3840-wide decoder looks exactly like a clean left column and a black
             * right one. */
            if (vt_have_peer && vt_peer.non_vcl != NULL && vt_peer.non_vcl_len > 0) {
                if (!coop_poc_params_parse(vt_peer.non_vcl, vt_peer.non_vcl_len,
                                           &coop_vt_peer_params) ||
                    !coop_vt_peer_params.valid) {
                    coop_vt_peer_params = coop_vt_src;
                }
            } else {
                coop_vt_peer_params = coop_vt_src;
            }
            /* FRESH, not "we managed to fill the column" -- counting vt_have_peer here would
             * score a repeat as a delivered pane and hide the gap all over again. */
            if (vt_fresh) { coop_vt_peer_used++; } else { coop_vt_peer_missing++; }

            /* PANE SIZE PER PICTURE, ours beside the peer's, for a run of consecutive
             * pictures. A flash that alternates CRISP and BLURRED frame to frame is the
             * encoder's rate control oscillating -- it starves one picture of bits and pays
             * the next -- and that shows up as the slice BYTE COUNT swinging, with no decode
             * needed. Stills cannot resolve a per-frame flash and the TV probe only advances
             * every 3s, so this is the cheapest way to see every picture. Logged as a burst
             * once the session is warm, not from frame 0, because the pattern is placed on
             * the hosts several seconds after the stream starts. */
            if (diag_log != NULL && coop_vt_frames > 3000 && coop_vt_sizelog < 120) {
                coop_vt_sizelog++;
                fprintf(diag_log, "[COOP-VT/SZ] n=%u mine=%zu peer=%zu fresh=%d\n",
                        coop_vt_frames, length, vt_peer_len, (int) vt_fresh);
                fflush(diag_log);
            }

            /* ONLY AN EVICTION BREAKS THE CHAIN. A HOLD DOES NOT, and treating it as one is
             * what turned this mend into the artefact it was meant to cure.
             *
             * A dropped pane leaves the peer's later slices predicting from a picture the
             * composite never held -- unrecoverable without a keyframe. A hold is the
             * opposite: skip concealment reproduces the previous tile EXACTLY, so the
             * reference chain is intact and there is nothing to mend. Marking holds as breaks
             * meant chain_broken was set a few percent of all pictures, i.e. essentially
             * always, and `coop_coord_request_peer_idr_keep()` then ran on every picture.
             * Measured 2026-08-03: 232 peer keyframes actually put on the wire in 65s = ~3.5
             * PER SECOND. On incompressible content an intra pane is heavily quantised, so
             * each one landed as a BLURRED frame that the following inter frames refined back
             * to sharp -- a ~3.5Hz crisp/blur cycle, which is the "very consistent flashing"
             * the user reported and which no counter showed, because every counter was
             * healthy while it happened.
             *
             * The eviction happens on the coordinator thread, so it is detected by watching
             * the counter move rather than by a return value here. */
            const unsigned ev_now = coop_coord_replaced();
            if (ev_now != coop_vt_last_evicted) {
                coop_vt_chain_broken = true;
            }
            coop_vt_last_evicted = ev_now;
            if (coop_vt_chain_broken) {
                /* Throttled inside coop_coord (COOP_IDR_REQ_MIN_MS), so calling it every
                 * picture costs nothing and cannot drive the peer to all-intra. `_keep` and
                 * not the clearing variant: the sticky IDR is what our next IRAP pairs
                 * with, and dropping it is what previously left bootstrap at boot=0. */
                coop_coord_request_peer_idr_keep();
                coop_vt_idrreq++;
                /* MENDED BY THE PEER'S NEXT IRAP -- from that picture on, its tile no longer
                 * refers to anything we failed to give it. */
                if (vt_fresh && vt_peer.slice[0] != NULL) {
                    const int ft = (vt_peer.slice[0][0] >> 1) & 0x3F;
                    if (ft >= 16 && ft <= 23) {
                        coop_vt_chain_broken = false;
                        coop_vt_mended++;
                    }
                }
            }
            /* EVERY PEER IRAP THE COMPOSITE TAKES, on the [ok=] line's clock. A peer-only encoder
             * restart (its game going fullscreen) re-anchors the peer through an IDR taken here,
             * and nothing else records when -- `mended`/`cuirap` only move when a break or a
             * catch-up backlog happens to coincide, so on 10-04 (Halo, PC2) three restart IDRs
             * left no trace and the re-anchor could not be timed against the host's log. */
            if ((vt_fresh || vt_from_idr) && vt_peer.slice_count > 0 && diag_log != NULL &&
                vt_peer.slice_type[0] >= 16 && vt_peer.slice_type[0] <= 23 &&
                coop_vt_irap_logged < 64) {
                coop_vt_irap_logged++;
                fprintf(diag_log, "[COOP-VT/IRAP] irap_ms=%lu irap_frame=%u irap_type=%d irap_ours=%d irap_via=%s irap_q=%u\n",
                        ticksms, vt_from_idr ? (unsigned) coop_coord_idr_frame() : (unsigned) pf,
                        vt_peer.slice_type[0], vt_mine_type, vt_from_idr ? "paired" : "take",
                        coop_coord_queued());
                fflush(diag_log);
            }
        }
        if (coop_vt_ready) {
            /* THE COMPOSITE OWNS ITS PICTURE ORDER COUNT. It used to inherit the primary's,
             * which left no room to insert anything: consecutive primary POCs differ by 1.
             * Numbering here -- and rewriting BOTH tiles to it -- is what makes a catch-up
             * picture expressible at all. An IRAP resets the count, as HEVC requires. */
            /* OUR UNPAIRED IRAP (#42) -- the vertical port of the horizontal path's fix below.
             * When our picture is an IDR and the peer has no IRAP to pair with, the take above
             * repeats the peer's last access unit: a P-slice with its own POC, beside our IDR.
             * FFmpeg (the Windows decoder) rejects that picture whole -- `Ignoring POC change
             * between slices: 13 -> 0`, `Error parsing NAL unit #4` -- measured 198 times in run
             * G5 and 11 in K3 (no game), and every P-frame after it then predicts from a picture
             * the decoder never built: this PC's pane stayed corrupted for as long as the host
             * sent no further IDR. Fold OUR IDR into a TRAIL_R picture instead (keepref: slice
             * 0 must keep the previous picture alive for the peer's slice), numbered on the
             * composite's own timeline, so both slices are legal and the peer's history is
             * intact. Converted BEFORE committing the POC: if it refuses, the picture goes out
             * exactly as it did before. Windows only -- the TV's decoder has never needed it. */
            bool vt_irap_pic = vt_mine_irap;
#ifdef COOP_PC_DECODER
            size_t vt_demoted_len = 0;
            if (vt_mine_irap && coop_vt_poc_init && vt_have_peer && vt_peer.slice_count > 0 &&
                !(vt_peer.slice_type[0] >= 16 && vt_peer.slice_type[0] <= 23) &&
                vt_mine != NULL && vt_mine_len > 0) {
                coop_au_t vt_mine_au;
                if (coop_au_parse(buffer, length, &vt_mine_au) && vt_mine_au.slice_count == 1) {
                    vt_demoted_len = coop_slice_idr_to_trail_keepref(&coop_vt_src, vt_mine, vt_mine_len,
                                                                     coop_vt_poc + 1, coop_vt_demote_buf,
                                                                     sizeof(coop_vt_demote_buf));
                }
                if (vt_demoted_len > 0) {
                    vt_irap_pic = false;
                    /* RE-ARM THE STICKY-IDR CLEARING, as the paired path does in coop_seq_on_paired():
                     * this picture is not a paired bootstrap, but without this the NEXT primary IDR
                     * would skip the clearing request and pair with the peer IDR asked for here --
                     * seconds stale by then -- resetting the peer's tile to an old picture while its
                     * later P-panes predict from the current one (diff-sentinel 2026-09-28; G7B: two
                     * restarts, demoted=1). No flush and no anchor reset: nothing was paired. */
                    coop_vt_seq.idr_cleared = false;
                    if (coop_vt_demoted++ == 0 && diag_log != NULL) {
                        fprintf(diag_log, "[COOP-VT] our IDR had no peer IRAP to pair with -- folded into "
                                          "a TRAIL picture at composite POC %u (#42)\n", coop_vt_poc + 1);
                        fflush(diag_log);
                    }
                } else {
                    coop_vt_demote_fail++;
                }
            }
#endif
            if (vt_irap_pic) { coop_vt_poc = 0; coop_vt_poc_init = true; }
            const unsigned vt_this_poc = vt_irap_pic ? 0u : ++coop_vt_poc;
            size_t out = 0;
            bool ok = coop_vt_nonvcl_len <= coop_vt_cap;
            /* Tile 1 painted from the PEER's intra data on this picture -- the loading gate. */
            bool vt_peer_intra = false;
            if (ok) {
                memcpy(coop_vt_buf, coop_vt_nonvcl, coop_vt_nonvcl_len);
                out = coop_vt_nonvcl_len;
            }
            for (int side = 0; side < 2 && ok; ++side) {
                /* Side 1 reads the PEER's access unit when one arrived, ours otherwise.
                 * Both are walked identically -- a pane's slices are a pane's slices,
                 * whichever machine produced them. */
                /* CONCEAL TILE 1 INSTEAD OF WALKING A STALE PANE. One synthesised slice
                 * covers the whole tile -- the pane is a single slice of 60x68 CTBs, which is
                 * exactly tile 1's CTB count, so skip data built over the PANE's parameters
                 * from CTB 0 lands on tile 1 one-for-one once re-addressed to column 60.
                 * Falls through to the repeat below if the builder refuses. */
                if (side == 1 && vt_conceal && vt_mine != NULL && vt_mine_len > 0) {
                    size_t sn = coop_build_skip_slice(&coop_vt_src, vt_mine, vt_mine_len, 0,
                                                      coop_skip_scratch,
                                                      sizeof(coop_skip_scratch),
                                                      coop_vt_skip_pane,
                                                      sizeof(coop_vt_skip_pane));
                    if (sn > 0) {
#ifdef COOP_PC_DECODER
                        /* THE COMPOSITE'S POC, NOT OUR HOST'S. The skip data is built over our
                         * header, so it carries our host's POC; tile 0 has already been retimed
                         * to vt_this_poc. The catch-up builder rewrites its skip slice for this
                         * reason and this branch never did. A decoder that checks (FFmpeg with
                         * AV_EF_EXPLODE, the Windows client) rejects the whole picture and
                         * requests a keyframe -- measured 2026-09-23: 36-byte conceal slices at
                         * POC 206/102/28 beside tile-0 POCs 15/121/31; idr_sent 8 by +40s and 184
                         * over the session. With this rewrite: 0 rejects, idr_sent=0. */
                        const unsigned char *cs = coop_vt_skip_pane;
                        size_t csn = sn;
                        if (!vt_irap_pic) {
                            size_t pn = coop_poc_rewrite(&coop_vt_src, coop_vt_skip_pane, sn,
                                                         vt_this_poc, coop_poc_buf, coop_poc_cap);
                            if (pn > 0) { cs = coop_poc_buf; csn = pn; }
                        }
                        size_t tl = coop_slice_to_tile(&coop_vt_src, &coop_vt_comp,
                                                       cs, csn,
                                                       coop_vt_src.w_ctbs,
                                                       coop_vt_skip_tile,
                                                       sizeof(coop_vt_skip_tile));
#else
                        size_t tl = coop_slice_to_tile(&coop_vt_src, &coop_vt_comp,
                                                       coop_vt_skip_pane, sn,
                                                       coop_vt_src.w_ctbs,
                                                       coop_vt_skip_tile,
                                                       sizeof(coop_vt_skip_tile));
#endif
                        if (tl > 0 && out + 4 + tl <= coop_vt_cap) {
                            memcpy(coop_vt_buf + out, "\x00\x00\x00\x01", 4);
                            out += 4;
                            memcpy(coop_vt_buf + out, coop_vt_skip_tile, tl);
                            out += tl;
                            coop_vt_concealed++;
                            continue;
                        }
                    }
                }
                const unsigned char *srcbuf = (side && vt_have_peer) ? vt_peer_src : buffer;
                const size_t srclen = (side && vt_have_peer) ? vt_peer_len : length;
                size_t pos = 0;
                while (pos + 4 <= srclen && ok) {
                    size_t next = pos + 4;
                    while (next + 4 <= srclen &&
                           !(srcbuf[next] == 0 && srcbuf[next+1] == 0 &&
                             srcbuf[next+2] == 0 && srcbuf[next+3] == 1)) { next++; }
                    const size_t end = (next + 4 <= srclen) ? next : srclen;
                    const unsigned char *nal = srcbuf + pos + 4;
                    size_t nlen = end - pos - 4;
#ifdef COOP_PC_DECODER
                    /* #42: our IDR slice, already folded into this picture's TRAIL form. */
                    bool vt_nal_demoted = false;
                    if (!side && vt_demoted_len > 0 && nal == vt_mine) {
                        nal = coop_vt_demote_buf;
                        nlen = vt_demoted_len;
                        vt_nal_demoted = true;
                    }
#endif
                    if (nlen > 3 && ((nal[0] >> 1) & 0x3F) <= 31) {
                        bool first = false;
                        unsigned addr = 0;
                        const coop_poc_params_t *walkp =
                            (side && vt_have_peer) ? &coop_vt_peer_params : &coop_vt_src;
                        if (!coop_slice_address(walkp, nal, nlen, &first, &addr)) {
                            ok = false;
                            break;
                        }
                        /* THE PEER'S SLICES NEED THE SAME TWO CORRECTIONS THE HORIZONTAL
                         * PATH APPLIES, and omitting them is what made the right column
                         * wash out and block up while the left stayed clean:
                         *   1. POC -- the peer counts independently, and a picture whose
                         *      slices disagree on POC is reordered out of the composite.
                         *   2. nal_unit_type -- every VCL NAL of one picture must share it,
                         *      so a peer IDR against our TRAIL must be converted, not spliced
                         *      raw (coop_slice_idr_to_trail, confirmed on this TV).
                         * Both operate on the PANE's parameters, before re-addressing. */
                        if (side && vt_have_peer) {
                            const int pt = (nal[0] >> 1) & 0x3F;
                            const int mt = (vt_mine != NULL) ? (int) ((vt_mine[0] >> 1) & 0x3F) : -1;
                            /* Read BEFORE the conversion below rewrites an IDR to TRAIL_R. */
                            if (pt >= 16 && pt <= 23) { vt_peer_intra = true; }
                            if (mt == 1 && (pt == 19 || pt == 20)) {
#ifdef COOP_PC_DECODER
                                /* WINDOWS DIAGNOSTIC BUILD ONLY -- NEVER SHIP TO webOS: the same
                                 * swap at the HORIZONTAL site made the TV bootstrap come up BLACK
                                 * (parameter register §11, 2026-08-16); this vertical site was never
                                 * measured on the TV. Measured 2026-09-23 on an RTX 4070: a
                                 * picture whose converted peer slice carries the empty RPS beside
                                 * our one-entry RPS is silently DROPPED WHOLE by D3D11VA and NVDEC
                                 * (frame 2 == frame 1 in both tiles); with this RPS it decodes as
                                 * software does. It did NOT fix the peer pane on its own. */
                                size_t cn = coop_slice_idr_to_trail_keepref(walkp, nal, nlen, 0,
                                                                    coop_conv_buf, coop_conv_cap);
#else
                                size_t cn = coop_slice_idr_to_trail(walkp, nal, nlen, 0,
                                                                    coop_conv_buf, coop_conv_cap);
#endif
                                if (cn > 0) { nal = coop_conv_buf; nlen = cn; }
                            }
                            /* To the COMPOSITE's POC, not the primary's: with catch-up
                             * pictures the two no longer coincide. */
                            if (!vt_irap_pic) {
                                size_t pn = coop_poc_rewrite(walkp, nal, nlen, vt_this_poc,
                                                             coop_poc_buf, coop_poc_cap);
                                if (pn > 0) { nal = coop_poc_buf; nlen = pn; }
                            }
                        }
                        /* OUR OWN slices need retiming too now -- they used to pass
                         * through carrying the host's POC, which is no longer the
                         * composite's. Peer slices are already retimed above. */
#ifdef COOP_PC_DECODER
                        if (!side && !vt_irap_pic && !vt_nal_demoted) {
#else
                        if (!side && !vt_irap_pic) {
#endif
                            size_t on = coop_poc_rewrite(&coop_vt_src, nal, nlen, vt_this_poc,
                                                         coop_conv_buf, coop_conv_cap);
                            if (on > 0) { nal = coop_conv_buf; nlen = on; }
                        }
                        const unsigned prow = addr / walkp->w_ctbs;
                        const unsigned pcol = addr % walkp->w_ctbs;
                        const unsigned want = prow * coop_vt_comp.w_ctbs +
                                              (side ? coop_vt_src.w_ctbs : 0u) + pcol;
                        /* ONE-SHOT SLICE MAP. The panel shows foreign content wrapped hard
                         * against the LEFT EDGE of the peer's tile plus displaced blocks,
                         * which is an ADDRESSING signature -- and no counter can see it.
                         * Print the real per-slice mapping for a couple of pictures: the
                         * peer's geometry beside ours, every slice's address in and out, and
                         * whether the peer's own pane is even the width we assume. A pane
                         * WIDER than 60 CTBs makes pcol run past the tile and wrap into the
                         * next row, which is exactly what the photographs show. */
                        if (diag_log != NULL && coop_vt_slicemap < 24) {
                            coop_vt_slicemap++;
                            fprintf(diag_log,
                                    "[COOP-VT/MAP] side=%d t=%d first=%d addr=%u -> row=%u "
                                    "col=%u want=%u | peer %ux%u bits=%u | src %ux%u | "
                                    "comp %ux%u bits=%u | maxaddr=%u%s\n",
                                    side, (int) ((nal[0] >> 1) & 0x3F), (int) first, addr,
                                    prow, pcol, want,
                                    walkp->w_ctbs, walkp->h_ctbs, walkp->slice_addr_bits,
                                    coop_vt_src.w_ctbs, coop_vt_src.h_ctbs,
                                    coop_vt_comp.w_ctbs, coop_vt_comp.h_ctbs,
                                    coop_vt_comp.slice_addr_bits,
                                    coop_vt_comp.w_ctbs * coop_vt_comp.h_ctbs,
                                    (pcol >= coop_vt_src.w_ctbs ||
                                     want >= coop_vt_comp.w_ctbs * coop_vt_comp.h_ctbs)
                                        ? "  <<< OUT OF TILE" : "");
                            fflush(diag_log);
                        }
                        /* STATIC, never on the stack. A 1MB automatic array crashes the
                         * decode thread EVEN WHEN THIS BRANCH NEVER RUNS: the compiler
                         * reserves it in the function frame regardless, so the stack blows
                         * on every frame. That is what killed PLAIN streaming after one
                         * frame -- bisected against the previous commit, which reached
                         * ok=1244 while this build died at ok=1. */
                        static unsigned char tmp[1 << 20];
                        size_t tl = coop_slice_to_tile(walkp, &coop_vt_comp, nal,
                                                       nlen, want, tmp, sizeof(tmp));
                        if (tl == 0 || out + 4 + tl > coop_vt_cap) {
                            ok = false;
                            break;
                        }
                        memcpy(coop_vt_buf + out, "\x00\x00\x00\x01", 4);
                        out += 4;
                        memcpy(coop_vt_buf + out, tmp, tl);
                        out += tl;
                    }
                    pos = next;
                    if (next + 4 > srclen) { break; }
                }
            }
            if (ok && out > 0 && out <= buffer_size) {
                memcpy(buffer, coop_vt_buf, out);
                length = out;
                coop_vt_frames++;
                /* RELEASE THE LOADING DIALOG -- vertical never did. The flag was set only by the
                 * horizontal and legacy splice paths, both excluded while coop_vt_enabled, so a
                 * vertical session could leave "Connecting..." only through the 15s backstop in
                 * streaming.controller.c. Same anchored-not-populated rule as the horizontal
                 * site: a mirrored or concealed tile 1 never sets vt_peer_intra. */
                if (vt_peer_intra) {
                    coop_first_real_splice = true;
                }
                /* A composite went out. The sequencer anchors on this, and asks for a queue
                 * flush when it was a paired IRAP -- panes queued before that keyframe
                 * predict from pictures the composite never held. */
                if (vt_have_peer && coop_seq_on_paired(&coop_vt_seq, vt_irap_pic)) {
                    coop_coord_flush();
                }
            } else {
                /* COMPOSE REFUSED. Three things the HORIZONTAL path learned the hard way on
                 * 2026-08-05, none of which existed here. This path has fired ZERO times in
                 * 39000 frames, so this is a safety net rather than a fix -- but it is the
                 * exact failure that decayed horizontal's peer tile to magenta, and vertical
                 * had no protection against any part of it.
                 *
                 * 1. GIVE THE POC BACK. `++coop_vt_poc` above is already spent; emitting
                 *    nothing (or something else) for that number leaves a HOLE, and every
                 *    later slice's delta-coded RPS references it. Measured on horizontal:
                 *    POCs 1,2,5,9,10,11 emitted, ffmpeg answering "Could not find ref with
                 *    POC 4" then "POC 8" -- exactly the numbers consumed by dropped pictures.
                 *    Only roll back if ours is still the most recent number issued; a
                 *    catch-up picture emitted later in this same call has taken a higher one
                 *    and is genuinely on the wire.
                 *
                 * 2. DO NOT PASS OUR OWN PANE THROUGH. It is 1920 wide against the
                 *    composite's 3840, so it re-initialises the decoder to the pane's SPS --
                 *    the same reason horizontal gates untiled pictures at the feed. Dropping
                 *    is safe here for the same reason it is there: our half alone is not a
                 *    legal picture for this stream's geometry.
                 *
                 * 3. COUNT IT. The old code logged once and then set coop_vt_frames = 1 to
                 *    stay quiet -- so a session that started refusing MID-STREAM produced no
                 *    signal at all, which is the "invisible to instrumentation" complaint the
                 *    comment below this block already makes about the vertical path. */
                if (!vt_irap_pic && coop_vt_poc != 0 && vt_this_poc == coop_vt_poc) {
                    coop_vt_poc--;
                    coop_vt_poc_returned++;
                }
                coop_vt_refused++;
                if (diag_log != NULL && coop_vt_refused % 300 == 1) {
                    fprintf(diag_log, "[COOP-VT] compose REFUSED (#%u, %u POCs returned) -- "
                                      "picture dropped; our pane alone is 1920 wide and would "
                                      "re-init the decoder\n",
                            coop_vt_refused, coop_vt_poc_returned);
                    fflush(diag_log);
                }
                return DR_OK;            /* drop, do not emit wrong-geometry */
            }
            /* THE VERTICAL PATH HAD NO PERIODIC COUNTER LINE AT ALL. [COOP-HH] is printed
             * inside the horizontal branch, which this one excludes, so a whole session of
             * tiled composition reported nothing -- which is precisely why the peer-column
             * corruption was recorded as "invisible to instrumentation". used/missing were
             * being counted the entire time and never shown. */
            if (diag_log != NULL && coop_vt_frames > 0 && coop_vt_frames % 600 == 0) {
                /* `evicted` IS THE ONE THAT MATTERS. A gap starves the column and is at
                 * least visible as missing=; a peer running FASTER than the composite
                 * silently overruns the queue, which drops the OLDEST pane (coop_coord.c
                 * :178) and tears a hole in the peer's prediction chain with every drop.
                 * Nothing downstream can tell -- errors stay 0, missing stays 0 -- and the
                 * damage lands only where residual is large, so static text survives and
                 * detailed motion shreds. recv far above peer= is the signature. */
#ifdef COOP_PC_DECODER
                fprintf(diag_log, "[COOP-VT] %s pid=%d frames=%u peer=%u missing=%u "
                                  "repeated=%u mirrored=%u recv=%u evicted=%u "
                                  "idrreq=%u/%u mended=%u conceal=%u catchup=%u refused=%u pocback=%u broken=%d ready=%d "
                                  "cudrop=%u cubig=%u cuirap=%u demoted=%u demotefail=%u idrstale=%u\n",
                        coop_role_tag(), (int) getpid(), coop_vt_frames,
                        coop_vt_peer_used, coop_vt_peer_missing,
                        coop_vt_repeated, coop_vt_mirrored,
                        coop_coord_received(), coop_coord_replaced(),
                        coop_coord_idr_sent(), coop_vt_idrreq, coop_vt_mended, coop_vt_concealed,
                        coop_vt_catchup, coop_vt_refused, coop_vt_poc_returned,
                        (int) coop_vt_chain_broken, (int) coop_vt_ready,
                        coop_vt_cu_drop, coop_vt_cu_big, coop_vt_cu_irap,
                        coop_vt_demoted, coop_vt_demote_fail, coop_vt_idr_stale);
#else
                fprintf(diag_log, "[COOP-VT] %s pid=%d frames=%u peer=%u missing=%u "
                                  "repeated=%u mirrored=%u recv=%u evicted=%u "
                                  "idrreq=%u/%u mended=%u conceal=%u catchup=%u refused=%u pocback=%u broken=%d ready=%d\n",
                        coop_role_tag(), (int) getpid(), coop_vt_frames,
                        coop_vt_peer_used, coop_vt_peer_missing,
                        coop_vt_repeated, coop_vt_mirrored,
                        coop_coord_received(), coop_coord_replaced(),
                        coop_coord_idr_sent(), coop_vt_idrreq, coop_vt_mended, coop_vt_concealed,
                        coop_vt_catchup, coop_vt_refused, coop_vt_poc_returned,
                        (int) coop_vt_chain_broken, (int) coop_vt_ready);
#endif
                fflush(diag_log);
            }
        }
    }

    /* NOT when the tile path already composed this picture. Vertical needs coop_live for the
     * coordinator, so without this exclusion BOTH run: the horizontal splice re-parses the
     * tiled composite as if it were a plain pane and splices again, and the two paths race
     * for peer panes out of the same queue. On the panel that showed as a clean left column
     * and a BLACK right one. */
    if (coop_live && !coop_vt_enabled && coop_splice_buf != NULL &&
        (coop_coord_connected() || coop_hmirror_enabled)) {
        /* Shared by both composition paths: the conceal/stale/re-bootstrap logic after
         * the splice keys off whether THIS picture got a fresh pane. */
        bool hh_paired = false;
        /* `hh_paired` means A COMPOSITE WAS EMITTED -- the conceal paths set it too. For
         * hold-run length we need the narrower fact: did the PEER PANE actually change on
         * this composite. Only the real-splice site sets this. Measured 2026-08-16: keying
         * the hold instrument off peer-AU availability instead counted PANE TAKES, which
         * catch-up drains several of per composite, and produced 900 "holds" against 600
         * composites -- a hold shorter than one composite, i.e. meaningless. */
        bool hh_fresh_peer = false;
        coop_au_t mine;
        if (app_launch_coop_requested() && !(vdec_stream_format & VIDEO_FORMAT_MASK_H264) &&
            coop_au_parse(buffer, length, &mine) &&
            mine.slice_count == 1 && mine.slice[0] != NULL) {
            if (!coop_hh_ready && mine.non_vcl != NULL && mine.non_vcl_len > 0) {
                coop_hh_nonvcl_len = coop_hh_tiles
                        ? coop_nonvcl_tile_rows(mine.non_vcl, mine.non_vcl_len,
                                                coop_hh_nonvcl, sizeof(coop_hh_nonvcl))
                        : coop_nonvcl_double_height(mine.non_vcl, mine.non_vcl_len,
                                                    coop_hh_nonvcl, sizeof(coop_hh_nonvcl));
                if (coop_hh_nonvcl_len > 0 &&
                    coop_poc_params_parse(coop_hh_nonvcl, coop_hh_nonvcl_len, &coop_hh_params) &&
                    coop_hh_params.valid) {
                    coop_hh_addr = coop_bottom_half_addr(&coop_hh_params);
                    /* OUR PANE's own parameters, kept beside the composite's. Slice headers are
                     * parsed and rewritten against the parameters they were ENCODED against;
                     * only the final re-address uses the composite's. */
                    if (!coop_poc_params_parse(mine.non_vcl, mine.non_vcl_len, &coop_hh_src) ||
                        !coop_hh_src.valid) {
                        coop_hh_src = coop_hh_params;
                    }
                    coop_hh_peer_params = coop_hh_src;   /* until the peer's own arrive */
                    coop_hh_ready = true;
                    /* RE-ANCHOR THE PEER THE MOMENT WE CAN SPLICE. Everything it sent before
                     * this instant was queued against a composite that did not exist yet and
                     * was suppressed or evicted -- measured on a live session 2026-08-15:
                     * replaced=387 during the "suppressing picture #N before the FIRST
                     * composite" phase. With the stock infinite GOP (nvenc_gop_length = 0) the
                     * peer emits exactly ONE IRAP per session and it lands INSIDE that phase,
                     * so the one picture that could re-anchor the chain is precisely the one
                     * the composite could not use. Everything after it predicts from
                     * references the decoder never received.
                     *
                     * On the panel that is not whole-frame corruption -- it is a band of
                     * bright streaks sitting in the CTU rows that were damaged, permanently,
                     * because a static wallpaper never overwrites them and nothing intra ever
                     * lands there. Exactly the "edge outlines over black flat areas -- residual
                     * with no intra to sit on" this file already describes at the top.
                     *
                     * Marking the chain broken here runs the PROVEN recovery rather than a new
                     * one: request a keyframe (throttled in coop_coord), hold the tile bit-
                     * exact until an IRAP arrives, then clear and count the mend. It costs one
                     * keyframe once per session, which is why this is not the gop30 experiment
                     * that failed -- that made coop_slice_idr_to_trail() fire CONSTANTLY and
                     * flash the whole pane; this fires it once. */
                    coop_hh_chain_broken = true;
                    coop_hh_last_evicted = coop_coord_replaced();
                    coop_coord_request_peer_idr_keep();
                    if (diag_log != NULL) {
                        fprintf(diag_log, "[COOP-HH] composite %ux%u CTBs addr_bits=%u "
                                          "bottom_addr=%u; re-anchoring peer (evicted=%u "
                                          "before first composite)\n", coop_hh_params.w_ctbs,
                                coop_hh_params.h_ctbs, coop_hh_params.slice_addr_bits,
                                coop_hh_addr, coop_coord_replaced());
                        fflush(diag_log);
                    }
                }
            }
            unsigned long hh_age = 0;
            uint32_t hh_frame = 0;
            /* IS THIS PICTURE OF OURS AN IRAP? It decides which peer picture can pair
             * with it, because every VCL NAL of one picture must carry the same
             * nal_unit_type -- so an IRAP of ours can only be composed with an IRAP of
             * theirs. */
            const int mine_type = (mine.slice_len[0] > 0) ? ((mine.slice[0][0] >> 1) & 0x3F) : -1;
            const bool mine_irap = mine_type >= 16 && mine_type <= 23;
            /* This composite picture's POC, assigned ONCE below and used by EVERY branch that
             * emits -- paired, concealed and fallback alike. */
            unsigned hh_this_poc = 0;
            size_t hh_len = 0;
            if (coop_hh_ready) {
                /* THE COMPOSITE HAD NO KEYFRAME AT ALL, and that was the whole remaining
                 * corruption. Dumping what the decoder actually receives and running it
                 * through ffmpeg said so in one line: 401 pictures, every one TRAIL_R,
                 * "First slice in a frame missing" / "Error constructing the frame RPS" /
                 * "Could not find ref with POC ..." on all of them. With no IRAP to start
                 * from, a decoder synthesises the missing references as flat mid-grey and
                 * paints residual on top -- which is exactly how the TV looked: correct
                 * geometry, correct content in the right pane, but washed out to grey with
                 * only edges surviving. It reads like a colour-space bug and is not one.
                 *
                 * It happens because the two hosts IDR INDEPENDENTLY. Ours arrives at
                 * session start, the peer's at some other moment, the type check (rightly)
                 * refuses to build a half-IDR picture, and with idr_refresh_interval_sec=0
                 * our host never IDRs again -- so the stream never contains one.
                 *
                 * The coordinator already keeps the peer's most recent IDR in a sticky
                 * slot for precisely this. Use it: on OUR IRAP, pair with THAT rather than
                 * with whatever inter picture happens to be current. */
                /* THE ORDERING RULES LIVE IN coop_seq.c, and every side effect they ask for
                 * is performed here. That split is not decoration: the two defects this
                 * logic has had -- the clearing/gap livelock and an anchor that could never
                 * be met -- are pure ordering, invisible to any test that cannot replay a
                 * sequence of pictures, and reproducing them live costs a two-host session.
                 * tests/coop/test_coop_seq.c now replays both measured traces offline. */
                /* Bank a couple of panes before the first take, so ordinary arrival jitter
                 * cannot starve the composite. Concealment covers these few pictures and is
                 * bit-exact, so priming costs a held pane, not a corrupt one. */
                /* Declared and zeroed BEFORE the prime check: the skip below jumps to the
                 * label past their assignment, and a jump over an initialisation would leave
                 * the diag line there reading indeterminate values. */
                coop_seq_plan_t plan;
                coop_seq_verdict_t v;
                memset(&plan, 0, sizeof(plan));
                memset(&v, 0, sizeof(v));
                if (!coop_hh_primed) {
                    if (coop_coord_queued() >= COOP_HH_PRIME) {
                        coop_hh_primed = true;
                    } else {
                        goto coop_hh_take_done;
                    }
                }
                /* DRAIN THE PEER'S BACKLOG FIRST -- the horizontal counterpart of the
                 * vertical catch-up, same reasoning and same guards (see
                 * docs/coop-rate-decoupling.md). Emitted BEFORE the normal take so these
                 * carry the lower POCs and the normal picture then pairs with the freshest
                 * pane. Leaves COOP_VT_CATCHUP_KEEP in hand: draining to empty starves the
                 * normal picture into concealing, which on the vertical path showed as a
                 * ghost that movement paints over. */
                {
                    const unsigned q_now = coop_coord_queued();
                    if (q_now > coop_hh_cu_qmax) { coop_hh_cu_qmax = q_now; }
                }
                if (coop_vt_catchup_enabled && coop_hh_ready && !mine_irap &&
                    coop_hh_nonvcl_len > 0 && mine.slice[0] != NULL &&
                    coop_coord_queued() >= COOP_VT_CATCHUP_START) {
                    unsigned hmade = 0;
                    while (hmade < COOP_VT_CATCHUP_MAX &&
                           coop_coord_queued() > COOP_VT_CATCHUP_KEEP) {
                        coop_seq_plan_t hp;
                        coop_seq_verdict_t hv;
                        memset(&hp, 0, sizeof(hp));
                        memset(&hv, 0, sizeof(hv));
                        coop_hh_cu_gate++;
                        coop_seq_plan(&coop_hh_seq, false, &hp);
                        if (hp.kind != COOP_SEQ_TAKE_NEXT) { coop_hh_cu_plan++; break; }
#ifdef COOP_PC_DECODER
                        /* THE SAME RULE AS VERTICAL (issue #27): stop in FRONT of a peer IDR
                         * rather than take it and have the builder refuse it (why[4]) -- a
                         * refused pane was already off the queue, so the re-anchoring keyframe
                         * vanished. why[4] counts these stops too (stops = sum(why) - build); a
                         * CRA or other IRAP not led by VPS/SPS/PPS still reaches the refusal. */
                        if (coop_coord_next_is_idr(hp.min_frame)) { coop_hh_cu_why[4]++; break; }
#endif
                        uint32_t hf = 0;
                        unsigned hd = 0;
                        size_t hl = coop_coord_take_next(coop_pane_buf, coop_pane_cap,
                                                         hp.min_frame, &hf, &hd);
                        coop_seq_verdict(&coop_hh_seq, &hp, hl > 0, hf, hd, &hv);
                        if (hv.drop_lastpeer) {
                            coop_hh_lastpeer_len = 0;
                            if (hd > 0) { coop_hh_chain_broken = true; }
                        }
                        if (hv.request_idr_keep) { coop_coord_request_peer_idr_keep(); }
                        if (hl == 0 || !hv.accept) { coop_hh_cu_take++; break; }
                        /* THE BLIND SPOT THAT MADE ppocjump MEANINGLESS. Catch-up consumes
                         * panes through its own take, so without this the POC tracker skipped
                         * every one of them and read the gap as a peer fault. */
                        {
                            coop_au_t cu_peer;
                            if (coop_au_parse(coop_pane_buf, hl, &cu_peer)) {
                                coop_hh_observe_peer(&cu_peer, hf);
                            }
                        }
                        const unsigned hpoc = ++coop_hh_poc;
                        size_t hn = coop_hh_build_catchup(mine.slice[0], mine.slice_len[0],
                                                          coop_pane_buf, hl, hpoc,
                                                          coop_vt_catch_buf,
                                                          sizeof(coop_vt_catch_buf));
                        if (hn == 0) { coop_hh_cu_build++; coop_hh_poc--; break; }
                        if (coop_hh_lastpeer != NULL && hl <= coop_pane_cap) {
                            memcpy(coop_hh_lastpeer, coop_pane_buf, hl);
                            coop_hh_lastpeer_len = hl;
                        }
                        diag_feed(FEED_SITE_HH_CATCHUP, coop_vt_catch_buf, hn, flags);
                        coop_vt_catchup++;
                        hmade++;
                    }
                }
                coop_seq_plan(&coop_hh_seq, mine_irap, &plan);
                unsigned dropped = 0;
                bool got = false;
                if (plan.kind == COOP_SEQ_TAKE_IDR) {
                    if (plan.request_idr_clearing) {
                        coop_coord_request_peer_idr();      /* clearing: drop the stale one */
                    }
                    hh_len = coop_coord_take_idr(coop_pane_buf, coop_pane_cap);
                    got = hh_len > 0;
                } else {
                    hh_len = coop_coord_take_next(coop_pane_buf, coop_pane_cap,
                                                  plan.min_frame, &hh_frame, &dropped);
                    got = hh_len > 0;
                }
                coop_seq_verdict(&coop_hh_seq, &plan, got, hh_frame, dropped, &v);
                /* CARRY THE PEER'S FRAME NUMBER TO THE DUMP SITE, so a corrupt picture in
                 * /tmp/coop_composed.h265 can be matched to the peer frame it was built from.
                 * The open question is whether the mis-predicted blocks land exactly on the
                 * pictures where the peer's numbering JUMPED -- i.e. where we spliced a slice
                 * coded against a frame we never delivered. Nothing else in the log is
                 * per-picture, and `peergap` alone cannot say WHICH picture it happened on. */
                coop_hh_dbg_frame = hh_frame;
                coop_hh_dbg_dropped = dropped;
                coop_hh_dbg_gap = (unsigned) (hh_frame > plan.min_frame ? hh_frame - plan.min_frame : 0);
                if (coop_hh_dbg_gap > 0) {
                    coop_hh_skip_events++;
                    coop_hh_skip_frames += coop_hh_dbg_gap;
                    if (coop_hh_dbg_gap > coop_hh_skip_max) { coop_hh_skip_max = coop_hh_dbg_gap; }
                }
                coop_hh_dbg_accept = v.accept;
                if (!v.accept) {
                    hh_len = 0;
                }
                if (v.drop_lastpeer) {
                    coop_hh_lastpeer_len = 0;
                    /* A FRAME-NUMBER JUMP IS NOT NECESSARILY A BROKEN CHAIN. Apollo advances
                     * its frame counter for frames it never encoded (it skips when there is
                     * nothing new and the minimum-fps timer has not fired), so a hole in the
                     * numbering does NOT mean a picture is missing from the peer's prediction
                     * chain -- and holding the pane for one costs a keyframe round trip for
                     * nothing. Measured 2026-08-02 at 18Mbps: panes arrived essentially 1:1
                     * (recv 592 of 596) yet peergap=39 produced held=215, a hold every ~0.45s
                     * and far worse than the 8% it was meant to replace.
                     *
                     * Only a pane WE discarded is a real break: `dropped` counts panes
                     * take_next threw away as older than the anchor, which is the queue
                     * having overflowed. */
                    if (dropped > 0) {
                        coop_hh_chain_broken = true;
                    }
                }
                if (v.request_idr_keep) {
                    coop_coord_request_peer_idr_keep(); /* waiting: keep what arrives */
                }
                coop_hh_take_done:
                if (diag_log != NULL && v.drop_lastpeer && coop_hh_seq.peergap % 20 == 1) {
                    fprintf(diag_log, "[COOP-HH] peer frame gap: expected %u got %u"
                                      " (+%d) -- splicing it and re-anchoring there (#%u)\n",
                            plan.min_frame, hh_frame,
                            (int) (hh_frame - plan.min_frame), coop_hh_seq.peergap);
                    fflush(diag_log);
                }
            }
            coop_au_t peer;
            bool have_peer = hh_len > 0 && coop_au_parse(coop_pane_buf, hh_len, &peer) &&
                             peer.slice[0] != NULL;
            /* THE LOADING GATE WANTS "ANCHORED", NOT "POPULATED". Set below when the peer's
             * slice for THIS picture is intra. See coop_first_real_splice. */
            bool hh_peer_intra = false;
            if (coop_peer_dump != NULL && have_peer && coop_peer_dump_n < 400) {
                fwrite(coop_pane_buf, 1, hh_len, coop_peer_dump);
                if (++coop_peer_dump_n == 400) { fflush(coop_peer_dump); fclose(coop_peer_dump);
                                                 coop_peer_dump = NULL; }
            }
            /* THE PEER'S OWN PARAMETERS, exactly as the tile path does it (:2776). PC2's 3070
             * need not describe its pane bit-for-bit the way the 4070 does, and its slice
             * header is only interpretable against the SPS/PPS it was encoded with. Falls back
             * to ours, which is what horizontal used unconditionally until now. */
            if (have_peer && peer.non_vcl != NULL && peer.non_vcl_len > 0) {
                if (!coop_poc_params_parse(peer.non_vcl, peer.non_vcl_len,
                                           &coop_hh_peer_params) ||
                    !coop_hh_peer_params.valid) {
                    coop_hh_peer_params = coop_hh_src;
                }
            }
            /* DIVERGENCE DETECTORS -- READ ONLY, AND DELIBERATELY SO. Here because it is
             * the last point where the peer's slice is still EXACTLY as its encoder wrote
             * it: the retype, the IDR->TRAIL conversion and the POC rewrite all edit the
             * header below. The catch-up loop above calls this too -- see the function. */
            if (have_peer) {
                coop_hh_observe_peer(&peer, hh_frame);
            }
            /* WATCH THE EVICTION COUNTER AND MEND, ported from the tile path (:2807). An
             * eviction happens on the COORDINATOR thread, so there is no return value here to
             * test -- the only way to know the peer's chain was cut is that the counter moved.
             * Horizontal previously learned about breaks only from `dropped` on its own take,
             * which cannot see a pane discarded before it ever asked, and it had no way back:
             * with the stock infinite GOP the peer emits exactly ONE IRAP per session, so an
             * unmended break lasts until the stream ends. Requesting a keyframe is throttled
             * inside coop_coord, so asking every picture costs nothing. */
            const unsigned hh_ev_now = coop_coord_replaced();
            if (hh_ev_now != coop_hh_last_evicted) {
                coop_hh_chain_broken = true;
            }
            coop_hh_last_evicted = hh_ev_now;
            /* NO CONCEAL BUDGET. A HOLD IS NOT A BREAK -- the rule the vertical path already
             * learned at :3247, removed here 2026-08-16 and this is why.
             *
             * The budget's premise was that concealing "rots" the tile. That is true of the
             * REPEAT concealer (an inter slice re-shown, applying its residual twice) and FALSE
             * of the skip concealer, which is BIT-EXACT -- measured 2026-08-04, concealed[n] vs
             * control[n-1] = inf. And the skip concealer is the ONLY thing that ever spent this
             * budget: `coop_hh_conceal_since_anchor` is incremented in exactly one place (:4114,
             * beside `coop_hh_skipconceal++`), never by the IRAP conceal at :4026. So the budget
             * was spent exclusively by the one operation proven incapable of the damage it was
             * added to catch. Measured in tiles 2026-08-16: repeated=0 skipconceal=16821 -- the
             * rot does not happen on this path at all. Repeat damage keeps its OWN guard,
             * COOP_HH_REPEAT_BUDGET at :4457, counted in REPEATS. This one was redundant even
             * under its own rationale.
             *
             * WHAT IT COST, because a "safety net" that fires on healthy input is expensive:
             * 300 conceals expire in ~1.8s at peer 0 against a 165 composite (and 2.9s at peer
             * 60 -- nearly flat, which is why the flash rate looked independent of the peer's
             * rate). Expiry latched chain_broken, which then requested a peer keyframe EVERY
             * picture below and froze the tile at :3812 until one landed. Each forced IDR is
             * converted and repaints the whole tile at a coarse QP -- one soft frame refined
             * back to sharp by the following inters. That is the one-frame whole-pane flicker,
             * and it is the SAME fault the vertical path measured on 2026-08-03 at 3.5/s
             * ("very consistent flashing", every counter healthy while it happened).
             * Log evidence, 600 composites: skipconceal=488 canchor=1 peergap=1 idrreq=30/358
             * converted=9 held=9 -- 388 keyframe demands and 9 whole-tile repaints from ONE
             * expiry, with delivery perfectly healthy.
             *
             * The counters are deliberately LEFT IN PLACE as proof: `canchor` must now read 0
             * while `skipconceal` keeps climbing. If `canchor` ever climbs again, the trigger is
             * back. Real breaks are unaffected -- eviction (:3782), our take dropping panes
             * (:3740), the catch-up take dropping (:3678) -- and a peer IRAP still mends. */
            if (coop_hh_chain_broken) {
                coop_coord_request_peer_idr_keep();
                if (have_peer && peer.slice[0] != NULL) {
                    const int ft = (peer.slice[0][0] >> 1) & 0x3F;
                    if (ft >= 16 && ft <= 23) {
                        coop_hh_chain_broken = false;   /* an IRAP is self-contained: mended */
                        coop_hh_mended++;
                        /* A mend wipes the accumulated error, so the budget starts again. */
                        coop_hh_conceal_since_anchor = 0;
                    }
                }
            }
            /* Hold the pane frozen after a gap until the peer re-anchors. Its inter slices
             * reference a picture the composite never held, so splicing them paints pale
             * blocks that spread; only an IRAP (or the converted IDR below) is self-contained
             * enough to mend the chain. Concealment is bit-exact, so the cost is a held pane
             * for the keyframe round trip rather than corruption that lingers. */
            if (have_peer && coop_hh_chain_broken && coop_skipconceal_enabled) {
                const int pt = (peer.slice_len[0] > 0) ? ((peer.slice[0][0] >> 1) & 0x3F) : -1;
                if (pt >= 16 && pt <= 23) {
                    coop_hh_chain_broken = false;   /* an IRAP mends it */
                } else {
                    have_peer = false;              /* freeze instead of splicing the hole */
                    coop_hh_heldbroken++;
                }
            }
            /* HORIZONTAL SELF-MIRROR: OUR OWN LIVE PANE INTO BOTH HALVES.
             *
             * Vertical has had this since /tmp/coop-vmirror, and horizontal never did, so
             * every horizontal test needed the second PC. When PC2 was unavailable the
             * fallback was play_file_test.sh -- and a recording left armed silently voided
             * two hours of "live" results on 2026-08-01. A self-mirror is the honest
             * substitute: it composes from the REAL desktop, through the REAL encoder, at
             * the real frame rate, so everything the recording could not show (what the
             * encoder does in response to a change, seam behaviour under live motion) is
             * still there. Only the peer's independence is missing.
             *
             * coop_au_splice_halves() already re-addresses whatever it is handed as the
             * peer, so handing it our own access unit puts our pane in the bottom half at
             * coop_hh_addr. Both halves then share our POC and NAL type by construction, so
             * the POC rewrite is a no-op and neither the retype nor the IDR-conversion
             * branch can fire -- both require the two slices to DIFFER. Nothing writes back
             * into `mine`. */
            if (!have_peer && coop_hmirror_enabled && coop_hh_ready) {
                peer = mine;
                have_peer = true;
                coop_hh_mirrored++;
            }
            /* THE PEER RUNS SLOWER THAN WE DO AND ALWAYS WILL.
             *
             * Measured over one 119s run: our host produced 10800 pictures while the peer
             * delivered 7117 (SKYNET ~91fps against PC2 ~62fps -- both asked for 165 and
             * neither held it, and Apollo's minimum_fps_target overrides a lower request).
             * Every picture without a peer slice used to go out with the bottom half
             * simply NOT CODED: a legal-looking picture whose lower CTBs were never
             * present. That is the "extremely corrupted" bottom pane -- 43% of pictures
             * incomplete, and each one poisons the reference chain for the ones after it.
             *
             * Rate-matching cannot fix this. Two encoders on two machines never hold the
             * same rate, and a SINGLE missed picture corrupts everything downstream of it
             * until the next IDR. The composite must therefore be able to produce a
             * COMPLETE picture with no fresh peer slice at all.
             *
             * Repeating the peer's last slice does that: the picture is whole, so the
             * decoder stays in sync. It is not free -- an inter slice replayed against a
             * different reference applies its residual twice, which shows as the ghosting
             * this project has chased -- so it is a floor, not the finish. The proper
             * fix is a synthesised all-skip slice (coop_conceal.c); this is the part of
             * it that needs no entropy coding, and it is what makes the difference
             * between "a bit soft" and "unwatchable". */
            /* ================= THE COMPOSITE'S POC, ASSIGNED ONCE PER PICTURE =================
             * ASSIGNED HERE, NOT IN THE PAIRED BRANCH, AND THAT DISTINCTION IS THE WHOLE BUG
             * THE FIRST CUT OF THIS REBUILD SHIPPED WITH. Numbering only on the paired path
             * left the concealed and fallback pictures still carrying the HOST's count, so the
             * composite interleaved two sequences -- measured on a live capture:
             *     known-good:   1, 2, 3, 4, 5, 6 ...        (monotonic)
             *     first cut:  215,216, 3,217,218, 6 ...     (two sequences interleaved)
             * and ffmpeg answered with "Could not find ref with POC -42" / "Error constructing
             * the frame RPS". Every picture we emit must be numbered, or none of them can be.
             *
             * AFTER the catch-up block above, so inserted pictures keep the LOWER POCs and this
             * one stays the newest -- the order the tile path uses (:2676 then :2863).
             *
             * Retiming OUR slice HERE also means the concealment below inherits it for free:
             * the skip slice is built FROM this header, and the fallback paths emit this very
             * slice. An IRAP is left alone -- it carries no POC field and resets the count. */
            /* AN UNPAIRED IRAP OF OURS IS ABOUT TO STOP BEING AN IRAP.
             *
             * Converting it to TRAIL_R is what lets the peer's half be CONCEALED rather than
             * left uncoded, and an uncoded half is not the one-picture blemish the fallback
             * comment below claims: co-op runs infinite GOP with one reference frame, so
             * whatever the decoder leaves there becomes the reference for every later picture
             * and only clears where real residual is coded over it. Static content codes as
             * skip -- which copies the reference verbatim -- so it persists indefinitely.
             *
             * The decision has to be made HERE, before the POC is assigned, because a TRAIL
             * picture must NOT reset the count the way an IRAP does. Everything the splice
             * needs is required up front so the POC choice and the splice cannot disagree; if
             * the splice still fails, the rollback below restores IRAP semantics exactly. */
            const bool hh_unpaired_irap =
                    coop_hh_ready && mine_irap && !have_peer && coop_skipconceal_enabled &&
                    coop_hh_lastmine_len > 0 && coop_splice_buf != NULL &&
                    coop_hh_nonvcl_len > 0 && coop_conv_buf != NULL &&
                    mine.slice[0] != NULL && mine.slice_len[0] > 0;
            if (coop_hh_ready) {
                if (mine_irap && !hh_unpaired_irap) {
                    coop_hh_poc = 0;
                    coop_hh_poc_init = true;
                } else if (mine_irap) {
                    hh_this_poc = ++coop_hh_poc;
                    coop_hh_poc_pending = hh_this_poc;
                } else {
                    hh_this_poc = ++coop_hh_poc;
                    /* REMEMBER THAT WE TOOK ONE, so the feed gate can give it back if this
                     * picture never goes out. A consumed-but-unemitted POC is a HOLE, and the
                     * reference sets of later slices still point into it. See the rollback at
                     * the tiles feed gate; catch-up has done the same since it was written
                     * (coop_hh_poc-- on a failed build). */
                    coop_hh_poc_pending = hh_this_poc;
                    size_t on = coop_poc_rewrite(&coop_hh_src, mine.slice[0], mine.slice_len[0],
                                                 hh_this_poc, coop_hh_mine_buf,
                                                 sizeof(coop_hh_mine_buf));
                    if (on > 0) {
                        mine.slice[0] = coop_hh_mine_buf;
                        mine.slice_len[0] = on;
                        /* Keep this retimed inter slice as the header donor for the next
                         * unpaired IRAP. Retimed rather than raw only because it is the copy
                         * already proven to walk; its POC is rewritten again when used.
                         *
                         * REFRESHED RARELY, ON PURPOSE. Only the HEADER is ever used -- the
                         * payload is replaced by synthesised all-skip data -- so a donor from
                         * a second ago is worth exactly as much as one from this picture, and
                         * an all-skip slice codes no residual so even its QP hardly matters.
                         * Copying every picture cost a ~160KB memcpy per frame in the video
                         * hot path at this bitrate, for nothing. */
                        if (on <= sizeof(coop_hh_lastmine) &&
                            (coop_hh_lastmine_len == 0 || (coop_hh_poc & 63u) == 0u)) {
                            memcpy(coop_hh_lastmine, coop_hh_mine_buf, on);
                            coop_hh_lastmine_len = on;
                        }
                    } else {
                        /* Could not retime ours: emit the host's number for BOTH halves rather
                         * than a picture whose slices disagree, and say so once. */
                        hh_this_poc = 0;
                        if (diag_log != NULL && coop_hh_poc_fail++ == 0) {
                            fprintf(diag_log, "[COOP-HH] our POC rewrite REFUSED -- falling back "
                                              "to the host's numbering for this picture\n");
                            fflush(diag_log);
                        }
                    }
                }
            }
            /* OUR UNPAIRED IRAP: CONCEAL THE PEER'S HALF INSTEAD OF LEAVING IT UNCODED.
             *
             * This is the picture the fallback below used to emit as our slice alone. Three
             * rewrites make it legal, and none of them is optional:
             *
             *  - Our IDR becomes a TRAIL_R I-slice, because an IRAP picture has no references
             *    and so cannot carry a skip slice at all. slice_type is per SLICE while
             *    nal_unit_type is per PICTURE, so a TRAIL_R picture may hold our I-slice
             *    beside the peer's P-slice -- the same trick already used on the peer's IDRs,
             *    in the opposite direction.
             *  - It uses the KEEPREF conversion, not the empty-RPS one. The decoder derives
             *    DPB state from the FIRST slice, and ours is first: an empty RPS there evicts
             *    the very picture the concealment slice must copy from.
             *  - The concealment slice is built from our last INTER slice, not from this one.
             *    coop_build_skip_slice() takes its header from the slice passed in and an
             *    I-slice has no skip syntax, so passing our IDR returns 0 and conceals
             *    nothing. The donor is retimed to this picture's POC first, because a picture
             *    whose slices disagree on POC is reordered out of the composite.
             *
             * Failure at any step rolls the POC back to IRAP semantics and falls through, so
             * the worst case is exactly the behaviour this replaces. */
            if (hh_unpaired_irap) {
                size_t cn = coop_slice_idr_to_trail_keepref(&coop_hh_src, mine.slice[0],
                                                            mine.slice_len[0], hh_this_poc,
                                                            coop_conv_buf, coop_conv_cap);
                size_t dn = (cn > 0)
                        ? coop_poc_rewrite(&coop_hh_src, coop_hh_lastmine, coop_hh_lastmine_len,
                                           hh_this_poc, coop_hh_mine_buf,
                                           sizeof(coop_hh_mine_buf))
                        : 0;
                const unsigned char *hh_top = coop_conv_buf;
                size_t hh_topn = cn;
                size_t sn = 0;
                if (dn > 0) {
                    if (coop_hh_tiles) {
                        /* Same shape as the skipconceal path: build over the PANE's
                         * parameters, then re-home both halves, because a tiled PPS adds a
                         * field our pane's slices do not carry. */
                        size_t pn = coop_build_skip_slice(&coop_hh_src, coop_hh_mine_buf, dn, 0,
                                                          coop_skip_scratch,
                                                          sizeof(coop_skip_scratch),
                                                          coop_vt_skip_pane,
                                                          sizeof(coop_vt_skip_pane));
                        sn = (pn > 0) ? coop_slice_to_tile(&coop_hh_src, &coop_hh_params,
                                                           coop_vt_skip_pane, pn, coop_hh_addr,
                                                           coop_poc_buf, coop_poc_cap)
                                      : 0;
                        size_t tn = coop_slice_to_tile(&coop_hh_src, &coop_hh_params,
                                                       coop_conv_buf, cn, 0, coop_vt_skip_tile,
                                                       sizeof(coop_vt_skip_tile));
                        if (tn == 0) {
                            sn = 0;  /* refuse rather than mix a tiled and an untiled header */
                        } else {
                            hh_top = coop_vt_skip_tile;
                            hh_topn = tn;
                        }
                    } else {
                        sn = coop_build_skip_slice(&coop_hh_params, coop_hh_mine_buf, dn,
                                                   coop_hh_addr, coop_skip_scratch,
                                                   sizeof(coop_skip_scratch),
                                                   coop_poc_buf, coop_poc_cap);
                    }
                }
                if (cn > 0 && sn > 0) {
                    static const unsigned char sc[4] = {0, 0, 0, 1};
                    size_t need = coop_hh_nonvcl_len + sizeof(sc) + hh_topn + sizeof(sc) + sn;
                    if (need <= coop_splice_cap && need <= buffer_size) {
                        size_t o = 0;
                        memcpy(coop_splice_buf + o, coop_hh_nonvcl, coop_hh_nonvcl_len);
                        o += coop_hh_nonvcl_len;
                        memcpy(coop_splice_buf + o, sc, sizeof(sc)); o += sizeof(sc);
                        memcpy(coop_splice_buf + o, hh_top, hh_topn); o += hh_topn;
                        memcpy(coop_splice_buf + o, sc, sizeof(sc)); o += sizeof(sc);
                        memcpy(coop_splice_buf + o, coop_poc_buf, sn); o += sn;
                        memcpy(buffer, coop_splice_buf, o);
                        length = o;
                        coop_live_paired++;
                        hh_paired = true;
                        coop_hh_tiled_ok = coop_hh_tiles;
                        coop_hh_irapconceal++;
                        if (diag_log != NULL && coop_hh_irapconceal == 1) {
                            fprintf(diag_log, "[COOP-HH] unpaired IRAP CONCEALED instead of "
                                              "emitting our half alone (poc=%u, tiles=%d)\n",
                                    hh_this_poc, (int) coop_hh_tiles);
                            fflush(diag_log);
                        }
                        goto coop_hh_done;
                    }
                }
                /* Roll back to IRAP semantics: this picture stays an IRAP after all, and an
                 * IRAP resets the count, so no POC hole is left behind. */
                coop_hh_poc = 0;
                coop_hh_poc_init = true;
                coop_hh_poc_pending = 0;
                hh_this_poc = 0;
            }
            /* Never repeat a stale inter slice under OUR IRAP: an IRAP picture has no
             * references, so the bottom half must be intra too or the picture is illegal. */
            /* PREFERRED CONCEALMENT: a synthesised all-skip slice, which reproduces the
             * previous picture's bottom half rather than replaying the peer's last slice
             * against a reference it was not coded for. The repeat path below is kept as
             * the default until this has been seen on the TV. */
            if (!have_peer && !mine_irap && coop_hh_ready && coop_skipconceal_enabled &&
                coop_splice_buf != NULL && coop_hh_nonvcl_len > 0) {
                /* TILES: build the skip payload over OUR PANE's parameters and re-home BOTH
                 * halves with coop_slice_to_tile(), which is what the paired path does.
                 *
                 * Building it over the COMPOSITE cannot work in tiles mode and does not fail
                 * loudly: slice_header_end_bit_x() REFUSES tiled parameters outright
                 * (`if (p->tiles_enabled ...) return 0`, coop_poc.c), so
                 * coop_build_skip_slice() returns 0 and concealment simply never fires --
                 * leaving the REPEAT floor, whose replayed residual smears MOVING content
                 * while static areas stay clean. That is the artefact reported on the peer's
                 * pane on 2026-08-05, with repeated= climbing ~340 per sample.
                 *
                 * Our own half must be re-homed too, not passed through: it was encoded with
                 * tiles OFF, so its header is one field short of what the tiled PPS promises. */
                const unsigned char *hh_top = mine.slice[0];
                size_t hh_topn = mine.slice_len[0];
                size_t sn;
                if (coop_hh_tiles) {
                    size_t pn = coop_build_skip_slice(&coop_hh_src, mine.slice[0],
                                                      mine.slice_len[0], 0,
                                                      coop_skip_scratch, sizeof(coop_skip_scratch),
                                                      coop_vt_skip_pane, sizeof(coop_vt_skip_pane));
                    sn = (pn > 0) ? coop_slice_to_tile(&coop_hh_src, &coop_hh_params,
                                                       coop_vt_skip_pane, pn, coop_hh_addr,
                                                       coop_poc_buf, coop_poc_cap)
                                  : 0;
                    size_t tn = coop_slice_to_tile(&coop_hh_src, &coop_hh_params, mine.slice[0],
                                                   mine.slice_len[0], 0, coop_vt_skip_tile,
                                                   sizeof(coop_vt_skip_tile));
                    if (tn == 0) {
                        sn = 0;      /* refuse rather than mix a tiled and an untiled header */
                    } else {
                        hh_top = coop_vt_skip_tile;
                        hh_topn = tn;
                    }
                } else {
                    sn = coop_build_skip_slice(&coop_hh_params, mine.slice[0],
                                               mine.slice_len[0], coop_hh_addr,
                                               coop_skip_scratch, sizeof(coop_skip_scratch),
                                               coop_poc_buf, coop_poc_cap);
                }
                if (sn > 0) {
                    static const unsigned char sc[4] = {0, 0, 0, 1};
                    size_t need = coop_hh_nonvcl_len + sizeof(sc) + hh_topn
                                + sizeof(sc) + sn;
                    if (need <= coop_splice_cap && need <= buffer_size) {
                        size_t o = 0;
                        memcpy(coop_splice_buf + o, coop_hh_nonvcl, coop_hh_nonvcl_len);
                        o += coop_hh_nonvcl_len;
                        memcpy(coop_splice_buf + o, sc, sizeof(sc)); o += sizeof(sc);
                        memcpy(coop_splice_buf + o, hh_top, hh_topn);
                        o += hh_topn;
                        memcpy(coop_splice_buf + o, sc, sizeof(sc)); o += sizeof(sc);
                        memcpy(coop_splice_buf + o, coop_poc_buf, sn); o += sn;
                        memcpy(buffer, coop_splice_buf, o);
                        length = o;
                        coop_live_paired++;
                        hh_paired = true;
                        /* This picture really is tiled now, so SAY SO -- the feed gate below
                         * drops any picture in tiles mode that did not set this, and a
                         * concealed composite that never announced itself would be silently
                         * eaten exactly the way the suppressor ate the composites (29210d37). */
                        coop_hh_tiled_ok = coop_hh_tiles;
                        coop_hh_skipconceal++;
                        coop_hh_conceal_since_anchor++;
                        goto coop_hh_done;
                    }
                }
            }
            if (!have_peer && !mine_irap && coop_hh_ready && coop_hh_lastpeer_len > 0) {
                if (coop_au_parse(coop_hh_lastpeer, coop_hh_lastpeer_len, &peer) &&
                    peer.slice[0] != NULL) {
                    have_peer = true;
                    coop_hh_repeated++;
                }
            }
            if (have_peer) {
                /* ALL VCL NALs OF ONE PICTURE MUST SHARE nal_unit_type. The two hosts IDR
                 * independently, so splicing a TRAIL_R of ours with an IDR of theirs is a
                 * constant occurrence and produces a half-IDR picture -- illegal, and seen
                 * on the TV as the bottom pane glitching out. The full-height path learned
                 * this and refuses; the half-height path never checked. */
                const int my_t = (mine.slice_len[0] > 0) ? ((mine.slice[0][0] >> 1) & 0x3F) : -1;
                const int pe_t = (peer.slice_len[0] > 0) ? ((peer.slice[0][0] >> 1) & 0x3F) : -1;
                const bool pe_irap = pe_t >= 16 && pe_t <= 23;
                /* Carried to the splice site for the LOADING GATE: an IRAP here, or the
                 * IDR->TRAIL conversion below, means the peer's tile is about to be painted
                 * from self-contained data. Read it BEFORE the conversion, which rewrites the
                 * NAL type to TRAIL_R and would make an intra repaint look like an inter one. */
                hh_peer_intra = pe_irap;
                if (my_t != pe_t && mine_irap && pe_irap) {
                    /* BOTH ARE IRAPs BUT OF DIFFERENT SUBTYPES -- IDR_W_RADL against
                     * IDR_N_LP or CRA -- because the two encoders choose independently.
                     * Refusing that pairing is what left bootstrap stuck at boot=0 with
                     * typemis climbing: our keyframe and theirs were both intra and both
                     * ready, and the only thing wrong was a label.
                     *
                     * Relabelling the peer's slice to OUR subtype is safe precisely
                     * because both are IRAPs: neither has references, so the subtype only
                     * governs how leading pictures are handled, and the composite's
                     * leading-picture behaviour is ours. The write is in-place into a
                     * buffer we own (coop_pane_buf or coop_poc_buf), never the peer's.
                     *
                     * INTER types are deliberately NOT forced: TRAIL_R against TRAIL_N is
                     * a real difference (whether the picture may be referenced), and
                     * mislabelling that breaks the peer's own prediction chain. */
                    unsigned char *pslice = (unsigned char *) peer.slice[0];
                    pslice[0] = (unsigned char) ((my_t << 1) | (pslice[0] & 0x01));
                    coop_hh_retyped++;
                } else if (my_t == 1 && !mine_irap && (pe_t == 19 || pe_t == 20)) {
                    /* THE PEER IDR'd AND WE DID NOT -- the single most common mismatch, and
                     * until now it DROPPED the pane. It no longer has to: slice_type is per
                     * SLICE while nal_unit_type is per PICTURE, so a TRAIL_R picture may
                     * legally carry our P-slice AND the peer's I-slice.
                     * coop_slice_idr_to_trail() rewrites the peer's IDR header into a
                     * TRAIL_R I-slice with an EMPTY RPS, so it references nothing and can
                     * ride in this picture of ours. Confirmed on the TV's own decoder, not
                     * just ffmpeg (commit 88bf13a5).
                     *
                     * WHY THIS MATTERS MORE THAN A SAVED PANE: it is what lets the peer
                     * re-anchor ALONE. Recovery previously needed an IRAP from BOTH hosts
                     * at the same instant, which is why the sticky-IDR slot and the forced
                     * paired refresh exist. With the peer on a short GOP its own periodic
                     * IDR is enough.
                     *
                     * POC is passed as 0 deliberately: the conversion only has to make the
                     * field EXIST, and the POC discipline below then rewrites it to ours
                     * with the same proven code path every other peer slice takes.
                     * Restricted to my_t == 1 because that is the type the conversion
                     * emits; against our TRAIL_N it would still mismatch. */
#ifdef COOP_PC_DECODER
                    /* WINDOWS ONLY, as in rpsfix3 -- and UNTESTED in horizontal on Windows. This
                     * conversion is counted by `converted` on [COOP-HH], the counter register §11
                     * measured when its keepref swap turned the TV BLACK on 2026-08-16. */
                    size_t cn = coop_conv_buf == NULL ? 0 :
                        coop_slice_idr_to_trail_keepref(&coop_hh_peer_params, peer.slice[0],
                                                peer.slice_len[0], 0,
                                                coop_conv_buf, coop_conv_cap);
#else
                    size_t cn = coop_conv_buf == NULL ? 0 :
                        coop_slice_idr_to_trail(&coop_hh_peer_params, peer.slice[0],
                                                peer.slice_len[0], 0,
                                                coop_conv_buf, coop_conv_cap);
#endif
                    if (cn > 0 && (int) ((coop_conv_buf[0] >> 1) & 0x3F) == my_t) {
                        peer.slice[0] = coop_conv_buf;
                        peer.slice_len[0] = cn;
                        coop_hh_converted++;
                        /* A converted IDR is still an I-slice: the tile is repainted whole
                         * from intra data even though the NAL now says TRAIL_R. */
                        hh_peer_intra = true;
                    } else {
                        have_peer = false;
                        coop_hh_typemismatch++;
                    }
                } else if (my_t != pe_t) {
                    have_peer = false;
                    coop_hh_typemismatch++;
                }
            }
            if (have_peer) {
                /* THE COMPOSITE'S OWN POC, NOT THE PRIMARY'S -- the keystone ported from the
                 * tile path (:2834). Both halves are rewritten to it, which is what leaves
                 * room to insert a catch-up picture; riding the primary's count is why
                 * horizontal could never emit one. An IRAP resets it, as HEVC requires.
                 *
                 * Each half is retimed against the parameters IT was encoded with -- ours with
                 * coop_hh_src, the peer's with coop_hh_peer_params -- never the composite's. */
                /* Ours was already retimed above, for every branch. Only the peer's half is
                 * left, and it goes to the SAME number -- a picture whose slices disagree on
                 * POC is reordered out of the composite. */
                if (!mine_irap && hh_this_poc != 0) {
                    size_t rn = coop_poc_rewrite(&coop_hh_peer_params, peer.slice[0],
                                                 peer.slice_len[0], hh_this_poc,
                                                 coop_poc_buf, coop_poc_cap);
                    if (rn > 0) {
                        peer.slice[0] = coop_poc_buf;
                        peer.slice_len[0] = rn;
                        coop_poc_rewrites++;
                    }
                }
                size_t n = coop_hh_tiles
                        ? coop_au_splice_tile_rows(&mine, &peer, &coop_hh_src,
                                                   &coop_hh_peer_params, &coop_hh_params,
                                                   coop_hh_addr, coop_hh_nonvcl,
                                                   coop_hh_nonvcl_len, coop_splice_buf,
                                                   coop_splice_cap)
                        : coop_au_splice_halves(&mine, &peer, &coop_hh_params, coop_hh_addr,
                                                coop_hh_nonvcl, coop_hh_nonvcl_len,
                                                coop_splice_buf, coop_splice_cap);
                if (n > 0) {
                    memcpy(buffer, coop_splice_buf, n);
                    length = n;
                    coop_live_paired++;
                    /* RELEASE THE LOADING DIALOG ON AN *ANCHORED* TILE, NOT MERELY A POPULATED
                     * ONE. Releasing on any real peer slice let the dialog lift while the tile
                     * was still guaranteed to show artefacts: coop_seq deliberately splices
                     * across a hole ("costs artefacts in the bottom pane until the peer's next
                     * IDR", coop_seq.c) because refusing is worse -- so the FIRST real slice is
                     * often an inter slice predicting from a picture the composite never held.
                     * An intra slice cannot do that. 0c2e41b1 moved this off concealed pictures;
                     * this moves it off unanchored ones. The 15s backstop in
                     * streaming.controller.c still applies, so a composite that never anchors
                     * cannot trap the user behind the dialog. */
                    if (hh_peer_intra) {
                        coop_first_real_splice = true;
                    }
                    hh_paired = true;
                    hh_fresh_peer = true;            /* the peer pane CHANGED on this composite */
                    coop_hh_tiled_ok = coop_hh_tiles;   /* only this path emits tiles */
                    /* Bootstrap means a paired *IRAP*, and the anchor is the first pane
                     * afterwards rather than the keyframe -- both rules, and the evidence
                     * for them, in coop_seq_on_paired(). */
                    if (coop_seq_on_paired(&coop_hh_seq, mine_irap)) {
                        coop_coord_flush();            /* queued panes predate this keyframe */
                    }
                    /* Keep the ORIGINAL peer AU (pre-POC-rewrite) so a repeat re-derives
                     * its POC from scratch; re-rewriting an already-rewritten slice would
                     * compound the edit. */
                    if (hh_len > 0 && hh_len <= sizeof(coop_hh_lastpeer)) {
                        memcpy(coop_hh_lastpeer, coop_pane_buf, hh_len);
                        coop_hh_lastpeer_len = hh_len;
                    }
                } else {
                    coop_live_fallback++;
                    coop_fb_nopane++;
                }
            } else {
                coop_live_fallback++;
                coop_fb_nopane++;
            }
            coop_hh_done:;
            /* HOLD-RUN: HOW MANY CONSECUTIVE COMPOSITES SHOWED THE SAME PEER TILE.
             *
             * This is the judder itself. No existing counter can express it: `paired`,
             * `concealed` and `repeated` all count PICTURES, so a peer frame held twice and
             * one held six times each count exactly once. A hold is CORRECT
             * ([[coop-two-free-running-monitors]]) -- only a VISIBLE hold is a bug -- so
             * log the DISTRIBUTION plus a timestamped line per hold longer than the running
             * mean, which is what an eye tracking smooth motion actually catches.
             *
             * ANCHORED HERE, AT `coop_hh_done`, BECAUSE THIS RUNS EXACTLY ONCE PER
             * COMPOSITE. Two earlier placements were wrong and both looked plausible: the
             * first went into the v5 splice path, which a horizontal TILES session never
             * runs on the primary (zero lines through a visibly stuttering session); the
             * second sat where the peer AU is parsed, which CATCH-UP re-enters several
             * times per composite, and measured 900 "holds" against 600 composites -- a
             * hold shorter than one composite, which is not a quantity that exists.
             *
             * TWO SIGNALS, AND BOTH ARE NEEDED:
             *   `hh_paired`     -- this decode unit EMITTED a composite. Gate on it, or the
             *                      run also counts pictures that reached this label by an
             *                      early exit without emitting anything. Measured: those are
             *                      ~half of all passes (87.9/s against 44.1 composites/s),
             *                      so an ungated run reports roughly double the true hold.
             *   `hh_fresh_peer` -- the peer pane actually CHANGED (real splice only; the
             *                      conceal paths set `hh_paired` too). This resets the run.
             *
             * `paired=` is printed alongside so THE UNIT IS CHECKABLE FROM THE LOG ITSELF:
             * with this gating, sum(hold lengths) == paired exactly. If it ever doesn't, the
             * instrument is measuring something other than composites -- which it has done
             * twice, in both cases producing a number that looked entirely plausible. */
            if (hh_paired && hh_fresh_peer) {
                if (coop_hold_run > 0) {
                    unsigned hb = coop_hold_run < 8 ? coop_hold_run : 8;
                    coop_hold_hist[hb]++;
                    coop_hold_n++;
                    coop_hold_sum += coop_hold_run;
                    const double hmean = (double) coop_hold_sum / (double) coop_hold_n;
                    if (coop_hold_n > 20 && coop_hold_logged < COOP_HOLD_MAX &&
                        diag_log != NULL && (double) coop_hold_run >= hmean + 1.0) {
                        fprintf(diag_log, "[COOP-HOLD] %s pid=%d t=%.3f held=%u mean=%.2f n=%u\n",
                                coop_role_tag(), (int) getpid(),
                                (double) SDL_GetTicks() / 1000.0, coop_hold_run, hmean,
                                coop_hold_n);
                        fflush(diag_log);
                        coop_hold_logged++;
                    }
                    if (coop_hold_n % 300 == 0 && diag_log != NULL) {
                        fprintf(diag_log,
                                "[COOP-HOLD] hh %s pid=%d n=%u mean=%.2f paired=%u  "
                                "1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u >=8:%u\n",
                                coop_role_tag(), (int) getpid(),
                                coop_hold_n, (double) coop_hold_sum / (double) coop_hold_n,
                                coop_live_paired,
                                coop_hold_hist[1], coop_hold_hist[2], coop_hold_hist[3],
                                coop_hold_hist[4], coop_hold_hist[5], coop_hold_hist[6],
                                coop_hold_hist[7], coop_hold_hist[8]);
                        fflush(diag_log);
                    }
                }
                coop_hold_run = 1;
            } else if (hh_paired && coop_hold_run > 0) {
                /* An emitted composite that did NOT change the peer pane extends the hold.
                 * The `hh_paired` gate is what keeps the unit in composites: a picture that
                 * reached this label without emitting must not lengthen anything. */
                coop_hold_run++;
            }
            /* EMIT ON A TIME INTERVAL, NOT EVERY 600 PICTURES, AND CARRY THE RATES.
             *
             * The old "% 600 == 0" trigger made every frame-rate number this project
             * produced unusable, and it did so silently. Sampling the last line at two wall
             * clock instants quantises the delta to 600 pictures, so over a 25s window the
             * fps estimate had a granularity of 600/25 = +-24fps -- "72fps" really meant
             * "somewhere in 48-96". Worse, the counters are snapshotted when the LINE IS
             * WRITTEN, up to a full 600-picture interval (~8-10s) away from when the sample
             * was taken, so the divisor was wrong too. A measured "exactly 60.0fps" and a
             * measured "72fps" were consistent with the same underlying rate.
             *
             * Emitting on a fixed wall-clock interval and printing dt with the deltas makes
             * the rate exact and SELF-CONTAINED: no external sampling, nothing to divide by
             * the wrong number. Ratios were always safe (paired and recv come from the same
             * snapshot); absolute rates never were. */
            coop_emit_rate(diag_log);
            if (diag_log != NULL && (coop_live_paired + coop_live_fallback) % 600 == 0) {
                fprintf(diag_log, "[COOP-HH] %s pid=%d paired=%u fallback=%u repeated=%u typemis=%u "
                                  "idrunpaired=%u retyped=%u converted=%u refresh=%u reparam=%u skipconceal=%u "
                                  "irapconceal=%u canchor=%u "
                                  "peergap=%u stale=%u recv=%u idrreq=%u/%u mirrored=%u held=%u catchup=%u mended=%u "
                                  "blocked=%u pocback=%u suppressed=%u "
                                  "cu[gate=%u qmax=%u plan=%u take=%u build=%u why=%u/%u/%u/%u/%u/%u/%u/%u] "
                                  "refbad=%u ppocjump=%u ready=%d boot=%d\n",
                        coop_role_tag(), (int) getpid(),
                        coop_live_paired, coop_live_fallback, coop_hh_repeated,
                        coop_hh_typemismatch, coop_hh_seq.idr_unpaired, coop_hh_retyped,
                        coop_hh_converted,
                        coop_hh_refreshes, coop_hh_reparam, coop_hh_skipconceal,
                        coop_hh_irapconceal, coop_hh_conceal_anchors,
                        coop_hh_seq.peergap, coop_hh_seq.stale_dropped,
                        coop_coord_received(), coop_coord_idr_sent(),
                        coop_coord_idr_suppressed(), coop_hh_mirrored, coop_hh_heldbroken, coop_vt_catchup,
                        coop_hh_mended,
                        coop_hh_untiled_blocked, coop_hh_poc_returned, coop_hh_suppressed,
                        coop_hh_cu_gate, coop_hh_cu_qmax, coop_hh_cu_plan, coop_hh_cu_take,
                        coop_hh_cu_build,
                        coop_hh_cu_why[1], coop_hh_cu_why[2], coop_hh_cu_why[3],
                        coop_hh_cu_why[4], coop_hh_cu_why[5], coop_hh_cu_why[6],
                        coop_hh_cu_why[7], coop_hh_cu_why[8],
                        coop_hh_refbad, coop_hh_ppocjump, (int) coop_hh_ready,
                        (int) coop_hh_seq.bootstrapped);
                fflush(diag_log);
            }
            /* THE LETTERBOX LIVES HERE, and it is the same bug as the corruption.
             *
             * Falling through with the buffer untouched submits OUR OWN access unit --
             * parameter sets included, and ours say pic_height 1088. That is the FIRST
             * SPS the decoder ever sees, because composition cannot start until a peer
             * slice has arrived, and the webOS pipeline sizes its video plane from the
             * first SPS and does not resize on a later one. So the plane is built for
             * 3840x1080, the 32:9 picture is fitted into the 16:9 panel, and the whole
             * composite is drawn into a 540-tall band with black above and below --
             * exactly what the screen showed. Two earlier attempts fixed the numbers
             * REPORTED to SS4S, which was never the input the plane was sized from.
             *
             * So before the first complete composite, submit NOTHING. Dropping here is
             * safe in a way it never is later: no picture has been decoded yet, so there
             * is no reference chain to break, and the first thing the decoder receives is
             * a 2176-row SPS. After bootstrap this branch means we could not build a whole
             * picture even by repeating the peer -- rare -- and our slice alone is still
             * the least-bad option, because by then the geometry is already established. */
            /* BOUND THE DRIFT, BECAUSE REPEAT-CONCEALMENT DECAYS WITHOUT LIMIT.
             *
             * Repeating the peer's last slice keeps every picture complete, but each
             * repeat re-applies that slice's residual against a reference it was not
             * coded for, and the error compounds. Over a 2-minute run with repeated=2076
             * of 8344 pictures the bottom pane decayed all the way to black with only
             * coloured edges left -- the residual still being drawn onto a reference that
             * had lost its content entirely. Short runs looked fine, which is exactly why
             * this needed a soak to see.
             *
             * Nothing in the stream clears it on its own: the hosts run with
             * idr_refresh_interval_sec=0, so after the initial keyframe there is never
             * another. So ask for one.
             *
             * ASK THE PEER, AND DO NOT RE-BOOTSTRAP. The old version dropped back to
             * un-bootstrapped so the "suppress, both hosts IDR, resume on a paired IRAP"
             * path would rebuild -- and it FIRED EXACTLY ONCE PER SESSION. The gate was
             * `bootstrapped`, which coop_seq_force_refresh() then cleared, and only a paired
             * IRAP sets it again; our host has no reason to IRAP, so the flag stayed false
             * for the rest of the session and the budget never triggered again. Measured
             * 2026-08-01: repeated=4884 of 9462 pictures with refresh=1 and boot=0, the
             * bottom pane decaying to coloured edges exactly as this comment predicts.
             *
             * A paired IRAP is not needed any more: coop_slice_idr_to_trail() folds the
             * PEER'S OWN IDR into whatever TRAIL picture we are sending, so one keyframe
             * from the peer alone re-intras the whole bottom pane. Gate on
             * `ever_bootstrapped` -- true for the rest of the session once composition has
             * started -- and just make the request. It is rate limited in coop_coord
             * (120 ms -- coop_coord.c:127; three comments said 500 ms until 2026-09-05,
             * which under-stated the ceiling by 4x at 8.3 forced IDRs/sec), so a run of
             * repeats cannot turn into an IDR storm.
             *
             * KEEP the retained peer AU: clearing it leaves the next paneless picture with
             * nothing to repeat, which sends the bottom half out uncoded. Repeat a little
             * longer and let the incoming IDR fix it properly.
             *
             * The budget is in REPEATS, not pictures, because repeats are what cause the
             * damage: a session where the peer keeps up never refreshes at all. */
            if (coop_hh_seq.ever_bootstrapped &&
                coop_hh_repeated - coop_hh_repeat_mark >= COOP_HH_REPEAT_BUDGET) {
                coop_hh_repeat_mark = coop_hh_repeated;
                coop_hh_refreshes++;
                coop_coord_request_peer_idr_keep();
                if (diag_log != NULL) {
                    fprintf(diag_log, "[COOP-HH] %u repeats since the last peer keyframe -- "
                                      "requesting one (#%u)\n",
                            (unsigned) COOP_HH_REPEAT_BUDGET, coop_hh_refreshes);
                    fflush(diag_log);
                }
            }
            /* A FALLBACK MUST NEVER CARRY OUR OWN PARAMETER SETS.
             *
             * Falling through to `goto coop_no_splice` submits our access unit verbatim,
             * and on any picture that carries parameter sets those say pic_height 1088.
             * Mid-stream that re-declares the picture size to the webOS pipeline and the
             * whole composite gets drawn squashed into a fraction of the screen -- the
             * "letterbox" coming BACK after bootstrap, which was misread as a side effect
             * of an unrelated change. Pre-bootstrap this is handled by suppressing
             * entirely; after bootstrap the picture still has to go out, so substitute the
             * HEIGHT-DOUBLED parameter sets and keep only our slice. The bottom half is
             * uncoded for that one picture, which is bad but local; a resolution change is
             * not local, it disfigures the rest of the session. */
            if (coop_hh_seq.ever_bootstrapped && !hh_paired && coop_hh_nonvcl_len > 0 &&
                mine.non_vcl != NULL && mine.non_vcl_len > 0 &&
                coop_splice_buf != NULL) {
                static const unsigned char sc[4] = {0, 0, 0, 1};
                size_t need = coop_hh_nonvcl_len + sizeof(sc) + mine.slice_len[0];
                if (need <= coop_splice_cap && need <= buffer_size) {
                    memcpy(coop_splice_buf, coop_hh_nonvcl, coop_hh_nonvcl_len);
                    memcpy(coop_splice_buf + coop_hh_nonvcl_len, sc, sizeof(sc));
                    memcpy(coop_splice_buf + coop_hh_nonvcl_len + sizeof(sc),
                           mine.slice[0], mine.slice_len[0]);
                    memcpy(buffer, coop_splice_buf, need);
                    length = need;
                    coop_hh_reparam++;
                }
            }
            /* Nothing at all goes out until the FIRST composite: a spliced inter picture
             * before that point is unusable (no keyframe to predict from) and feeding it
             * only teaches the decoder a geometry we then have to correct.
             *
             * `ever_bootstrapped`, NOT `bootstrapped`. Once one composite has been emitted
             * the plane geometry is established, so a later gap must fall through to the
             * reparam branch above -- our slice with the height-doubled parameter sets --
             * instead of dropping the picture. Gating this on `bootstrapped` made every gap
             * and forced refresh re-enter drop-everything mid-session: 5521 pictures
             * suppressed in one measured run (~29%), which the user saw as BOTH panes
             * stuttering, the primary's own mouse movement included. */
            /* IN TILES MODE, AN UNPAIRED PICTURE IS NOT MERELY STALE -- IT IS ILLEGAL.
             * A tiled picture must cover every CTB, so emitting our half alone leaves tile 1
             * uncoded; and our slice was encoded with tiles OFF, so its header is one field
             * short of what the composite's PPS promises. That is exactly what the first live
             * tiles run produced: the IRAP went out with a single VCL NAL and ffmpeg answered
             * `alignment_bit_equal_to_one=0` then `First slice in a frame missing`. The
             * untiled path can fall back on our half because a one-slice picture is legal
             * there; this one cannot, so suppress and wait for a pair.
             *
             * `!hh_paired` IS THE WHOLE POINT AND IT WAS MISSING -- THIS IS WHY TILES WAS BLACK.
             * The clause read `|| coop_hh_tiles`, which is true for EVERY picture in tiles mode,
             * so the successfully tiled composite built ~30 lines above was thrown away right
             * after being built. The counters said everything was fine because they are
             * incremented at the splice (paired=7200 mirrored=7200 boot=1 ready=1) -- pairing
             * really was working; the picture just never reached SS4S_PlayerVideoFeed. Measured
             * 2026-08-04 with a state print AT the feed site: it never executed once, the
             * /tmp/coop-dump-out capture stayed 0 bytes for the same reason, and the panel was
             * black. The untiled path was unaffected only because its clause goes false once
             * `ever_bootstrapped` is set, whatever the pairing.
             * So: suppress before the first composite (both modes), and in tiles mode suppress
             * an UNPAIRED picture -- never a paired one. */
            if (!coop_hh_seq.ever_bootstrapped || (coop_hh_tiles && !hh_paired)) {
                coop_hh_suppressed++;
                /* GIVE THE POC BACK -- THIS PATH IS THE DOMINANT HOLE SOURCE.
                 *
                 * It `return`s a few lines below, so it never reaches the tiles feed gate
                 * where the other rollback lives. Every suppressed picture had already
                 * taken `hh_this_poc = ++coop_hh_poc`, so it left a gap that every later
                 * slice's delta-coded RPS still points into. Measured 2026-08-05 on a
                 * decayed session: emitted POCs 1,2,3,4,9,10 with 5-8 simply absent, a run
                 * of "Could not find ref with POC 94..99", and 395 of 400 captured pictures
                 * failing "Error constructing the frame RPS" -- while blocked=0 and
                 * pocback=0 said the two known suppression paths had never fired. The
                 * decoder cannot build a reference list, so the PEER's tile predicts from
                 * whatever it has and decays: artefacts around fine detail and text first,
                 * spreading from there. That is the leak the user reported. */
                if (coop_hh_poc_pending != 0 && coop_hh_poc_pending == coop_hh_poc) {
                    coop_hh_poc--;
                    coop_hh_poc_returned++;
                }
                coop_hh_poc_pending = 0;
                if (diag_log != NULL && coop_hh_suppressed % 120 == 1) {
                    fprintf(diag_log, "[COOP-HH] suppressing picture #%u before the FIRST "
                                      "composite (emitting our own AU would give the decoder "
                                      "a 1088-row SPS and a keyframe-less stream)\n",
                            coop_hh_suppressed);
                    fflush(diag_log);
                }
                /* BOTH SIDES HAVE TO IRAP AT ONCE, and neither will on its own: our host
                 * has idr_refresh_interval_sec=0, and the peer only IDRs when asked. So
                 * drive it -- ask the peer for a keyframe and return DR_NEED_IDR so our
                 * own host sends one too. Throttled to roughly twice a second: requesting
                 * on every picture makes both hosts stream nothing but IDRs and they still
                 * never line up. */
                if (coop_hh_suppressed % 30 == 1) {
                    coop_coord_request_peer_idr_keep();
                    return DR_NEED_IDR;
                }
                return DR_OK;
            }
            goto coop_no_splice;
        }
        /* H.264 IS NOW ALLOWED HERE. The old exclusion was only ever about PARSING -- H.264's
         * AUD (9) and SEI (6) alias onto HEVC VCL types 4 and 3 -- and coop_au_parse is codec
         * aware since coop_splice_set_h264(). This is the PROVEN path: coop_au_splice() copies
         * A's parameter sets, A's slice 0 and B's slice 1 VERBATIM, with no header rewriting at
         * all, which is why it is the one to use. AVC needs exactly one extra step, below: the
         * peer's slice header re-emitted for the vendors' differing frame_num width. */
        /* H.264 DOES NOT COME HERE, and the reason is design, not parsing. This branch is the
         * FULL-HEIGHT two-slice splice: it assumes each host encoded the whole composite and
         * takes slice 0 from one and slice 1 from the other verbatim. AVC co-op is HALF-HEIGHT
         * -- each host encodes only its own 1088 rows and coop_avc_compose() re-homes the peer's
         * slices into the bottom half -- so a two-slice AVC picture here is two slices of ONE
         * host's own pane, and splicing them cross-host mixes each host's top half with the
         * other's bottom half. Measured 2026-08-30: with slices=2 armed at half height this
         * branch fired (tsok=89) alongside the real path and mis-spliced every picture. */
        if (coop_au_parse(buffer, length, &mine) && mine.slice_count == 2) {
            unsigned long age = 0;
            uint32_t pane_frame = 0;
            size_t pane_len = coop_coord_take_latest(coop_pane_buf, coop_pane_cap,
                                                     &pane_frame, NULL, &age);
            /* BOOTSTRAP: until one picture has been spliced IRAP-with-IRAP, the
             * other PC's pane cannot become visible. Its inter slices predict from
             * the decoder's picture buffer, which holds OUR host's frames -- on a
             * near-static desktop the residual is ~0, so the pane reconstructs as
             * our own content continuing and nothing appears to change. Pair an
             * IRAP of ours with the other host's stored IDR once, and from then on
             * each pane predicts from its own half of the previous composite. */
            /* slice_type[] holds the NAL UNIT TYPE (coop_splice.h:32): HEVC IRAP is 16..23,
             * an H.264 IDR is 5. */
            bool mine_irap = mine.slice_type[0] >= 16 && mine.slice_type[0] <= 23;
            if (!coop_bootstrapped) {
                if (mine_irap) {
                    size_t idr_len = coop_coord_take_idr(coop_pane_buf, coop_pane_cap);
                    coop_au_t idr_au;
                    if (idr_len > 0 && coop_au_parse(coop_pane_buf, idr_len, &idr_au) &&
                        idr_au.slice[1] != NULL) {
                        pane_len = idr_len;
                        age = 0;
                    }
                } else if (coop_idr_requests < 30 &&
                           (coop_live_paired + coop_live_fallback) % 60 == 0) {
                    /* Ask our host for a keyframe so an IRAP of ours exists to pair
                     * with. Bounded: a wedged bootstrap must not spam IDR requests
                     * for the life of the session. */
                    LiRequestIdrFrame();
                    coop_idr_requests++;
                }
            }
            bool paired_this = false;
            /* `paired_this` is ALSO set by the repeat-pane path below ("do not also
             * conceal"), so it is not a freshness signal -- an instrument keyed off it
             * reports a hold of 1 forever. This one is set only where real peer content is
             * spliced. Same distinction as `hh_fresh_peer` in the tiles path. */
            bool fresh_pane_this = false;
            coop_au_t theirs;
            if (pane_len > 0 && coop_au_parse(coop_pane_buf, pane_len, &theirs) &&
                theirs.slice[1] != NULL) {
                /* Align the incoming pane's picture-order count with ours before
                 * splicing. The two hosts count pictures independently and at
                 * different effective frame rates, so measured across a real
                 * composite 0 of 119 pictures had matching POCs; the decoder could
                 * not resolve the spliced slice's references and discarded it,
                 * leaving the pane stuck on its bootstrap IDR. IDR panes carry no
                 * POC field and need no rewrite. */
                if (!coop_poc_params.valid) {
                    coop_poc_params_parse(buffer, length, &coop_poc_params);
                }
                /* Splice ONLY when POC alignment is guaranteed. Apollo sends the
                 * parameter sets periodically, so early pictures were being spliced
                 * before the SPS/PPS had been seen -- measured 27 of 196 pictures
                 * going out misaligned (poc_fix=133 vs paired=161), which is a
                 * glitch source. A single-pane picture is always better than a
                 * picture the decoder may discard. */
                bool poc_ok = false;
                unsigned my_poc = 0;
                if (coop_poc_params.valid && coop_poc_buf != NULL) {
                    if (coop_poc_read(&coop_poc_params, mine.slice[0], mine.slice_len[0], &my_poc)) {
                        size_t rn = coop_poc_rewrite(&coop_poc_params, theirs.slice[1],
                                                     theirs.slice_len[1], my_poc,
                                                     coop_poc_buf, coop_poc_cap);
                        if (rn > 0) {
                            theirs.slice[1] = coop_poc_buf;
                            theirs.slice_len[1] = rn;
                            coop_poc_rewrites++;
                            poc_ok = true;
                        } else {
                            coop_poc_failed++;
                        }
                    } else {
                        /* Ours carries no POC field (IDR). That is only spliceable
                         * with a pane that likewise has none -- an IDR pair. */
                        poc_ok = !coop_poc_read(&coop_poc_params, theirs.slice[1],
                                                theirs.slice_len[1], NULL);
                    }
                }
                if (!poc_ok) {
                    coop_live_fallback++;
                    coop_poc_unaligned++;
                    goto coop_no_splice;
                }
                size_t n = coop_au_splice(&mine, &theirs, coop_splice_buf, coop_splice_cap);
                if (n > 0) {
                    memcpy(buffer, coop_splice_buf, n);
                    length = n;
                    coop_live_paired++;
                    /* Same anchored-not-populated rule as the tile path above. NOTE
                     * coop_au_t.slice_type[] holds the NAL UNIT TYPE despite its name
                     * (coop_splice.h:32), so IRAP is 16..23 -- not slice_type 2. */
                    if (theirs.slice_type[1] >= 16 && theirs.slice_type[1] <= 23) {
                        coop_first_real_splice = true;
                    }
                    paired_this = true;
                    fresh_pane_this = true;          /* the peer pane CHANGED on this composite */
                    if (coop_lastpane != NULL && theirs.slice_len[1] <= coop_poc_cap) {
                        memcpy(coop_lastpane, theirs.slice[1], theirs.slice_len[1]);
                        coop_lastpane_len = theirs.slice_len[1];
                        coop_lastpane_type = theirs.slice_type[1];
                    }
                    if (!coop_bootstrapped && mine_irap) {
                        coop_bootstrapped = true;
                        if (diag_log != NULL) {
                            fprintf(diag_log, "[COOP-LIVE] BOOTSTRAPPED on IRAP type=%d\n",
                                    mine.slice_type[0]);
                            fflush(diag_log);
                        }
                    }
                } else {
                    coop_live_fallback++;
                    coop_fb_mismatch++;
                }
            } else {
                coop_live_fallback++;   /* no pane this frame: show ours alone */
                if (pane_len == 0) { coop_fb_nopane++; } else { coop_fb_mismatch++; }
            }
            coop_no_splice:;
            /* REPEAT THE LAST PANE rather than fall back to our own half. Its
             * references then resolve to the previous composite, whose bottom half
             * is already the other host's content, so that pane keeps its own
             * prediction chain. The motion in the repeated slice is applied twice
             * (mild drift), which is far better than overwriting the pane -- and
             * blanking is not an option because this decoder renders uncoded CTUs
             * BLACK rather than concealing them. */
            if (coop_repeat_pane && !paired_this && coop_bootstrapped && coop_lastpane_len > 0 &&
                coop_poc_params.valid && coop_poc_buf != NULL && mine.slice_count == 2 &&
                coop_lastpane_type == mine.slice_type[0]) {
                unsigned my_poc2 = 0;
                if (coop_poc_read(&coop_poc_params, mine.slice[0], mine.slice_len[0], &my_poc2)) {
                    size_t rn = coop_poc_rewrite(&coop_poc_params, coop_lastpane, coop_lastpane_len,
                                                 my_poc2, coop_poc_buf, coop_poc_cap);
                    if (rn > 0) {
                        coop_au_t rep;
                        memset(&rep, 0, sizeof(rep));
                        rep.slice[1] = coop_poc_buf;
                        rep.slice_len[1] = rn;
                        rep.slice_type[1] = coop_lastpane_type;
                        size_t n2 = coop_au_splice(&mine, &rep, coop_splice_buf, coop_splice_cap);
                        if (n2 > 0 && n2 <= buffer_size) {
                            memcpy(buffer, coop_splice_buf, n2);
                            length = n2;
                            coop_repeated++;
                            paired_this = true;   /* do not also conceal */
                        }
                    }
                }
            }
            /* OUR OWN IRAP KILLS THE OTHER PANE, so the pane must RE-BOOTSTRAP.
             * A picture cannot be half-IDR, so when our slice 0 is an IRAP and no
             * IDR pane was there to pair with it, we emit our own slice 1 as well
             * -- an intra slice that overwrites the other host's half of the
             * reference picture. Its next inter slice then predicts from our
             * content and the pane never recovers on its own: measured offline as
             * a permanent collapse from 45dB to 8dB at the first IRAP, with the
             * other host's later IDRs uselessly refused for type mismatch.
             * Recovery needs an IDR from BOTH hosts in the same picture, which is
             * exactly what the bootstrap path already builds -- so re-arm it and
             * ask the other host (through the secondary) for a keyframe. Bounded,
             * because a wedged pane must not spam IDR requests for a whole
             * session; each re-arm also refills our own bounded request budget. */
            if (mine_irap && !paired_this && coop_bootstrapped && coop_rebootstraps < COOP_MAX_REBOOTSTRAPS) {
                coop_bootstrapped = false;
                coop_lastpane_len = 0;
                coop_lastpane_type = -1;
                coop_idr_requests = 0;
                coop_stale_run = 0;
                coop_conceal_since_boot = 0;
                coop_rebootstraps++;
                coop_coord_request_peer_idr();
                if (diag_log != NULL) {
                    fprintf(diag_log, "[COOP-LIVE] our IRAP type=%d wiped the pane -- "
                                      "re-bootstrapping (#%u)\n", mine.slice_type[0], coop_rebootstraps);
                    fflush(diag_log);
                }
            }
            /* SECOND TRIGGER: A PANE THAT HAS SIMPLY GONE STALE.
             *
             * The re-arm above only fires when OUR host emits an IRAP, and with
             * `idr_refresh_interval_sec=0` (Apollo's default, and what both hosts run)
             * we essentially never do -- measured `reboot=0` across every session,
             * i.e. the recovery path has NEVER ONCE RUN in production. So when the
             * pane feed stalls, the composite keeps concealing from an ever-older
             * picture and the bottom half degrades until it is a frozen still. That
             * is the "stream degraded over time" the user sees, and it is fatal for
             * the artifact-free requirement: nothing else clears it.
             *
             * A long run of CONSECUTIVE unpaired pictures is the signature. Isolated
             * fallbacks are normal -- if the peer delivers at half our rate we simply
             * alternate, so runs stay 1-2 long -- whereas COOP_STALE_PICTURES in a row
             * means the pane feed has actually stopped. Recovering costs one paired
             * IRAP, which is far cheaper than a permanently broken pane. */
            /* HOW LONG EACH PEER FRAME WAS HELD ON SCREEN -- the judder itself, as data.
             *
             * The composite emits one picture per PRIMARY frame, so a peer running at a
             * different rate has each of its frames HELD for several composites. With a
             * peer at 60 against a 165 composite the ratio is 2.75, so holds alternate
             * 2,3,3,2... and the leftover phase drift inserts a longer one now and then.
             * That irregularity IS the stutter the user sees, and no counter here could
             * express it: `paired`/`concealed` count pictures, and a frame held 2 times
             * and a frame held 6 times both count exactly once.
             *
             * A hold is CORRECT ([[coop-two-free-running-monitors]]) -- only a VISIBLE
             * hold is a bug -- so this logs the DISTRIBUTION plus a timestamped line for
             * each hold longer than the running mean, which is what an eye tracking
             * smooth motion actually catches. Timestamps so a PERIOD can be read off
             * them, same as [COOP-LATE]. */
            if (fresh_pane_this) {
                if (coop_hold_run > 0) {
                    unsigned b = coop_hold_run < 8 ? coop_hold_run : 8;
                    coop_hold_hist[b]++;
                    coop_hold_n++;
                    coop_hold_sum += coop_hold_run;
                    const double hold_mean = (double) coop_hold_sum / (double) coop_hold_n;
                    if (coop_hold_n > 20 && coop_hold_logged < COOP_HOLD_MAX &&
                        diag_log != NULL && (double) coop_hold_run >= hold_mean + 1.0) {
                        fprintf(diag_log, "[COOP-HOLD] %s pid=%d t=%.3f held=%u mean=%.2f n=%u\n",
                                coop_role_tag(), (int) getpid(),
                                (double) SDL_GetTicks() / 1000.0, coop_hold_run,
                                hold_mean, coop_hold_n);
                        fflush(diag_log);
                        coop_hold_logged++;
                    }
                    if (coop_hold_n % 300 == 0 && diag_log != NULL) {
                        fprintf(diag_log,
                                "[COOP-HOLD] v5 %s pid=%d n=%u mean=%.2f paired=%u  "
                                "1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u >=8:%u\n",
                                coop_role_tag(), (int) getpid(),
                                coop_hold_n, (double) coop_hold_sum / (double) coop_hold_n,
                                coop_live_paired,
                                coop_hold_hist[1], coop_hold_hist[2], coop_hold_hist[3],
                                coop_hold_hist[4], coop_hold_hist[5], coop_hold_hist[6],
                                coop_hold_hist[7], coop_hold_hist[8]);
                        fflush(diag_log);
                    }
                }
                coop_hold_run = 1;
            } else if (paired_this && coop_hold_run > 0) {
                /* Gated on `paired_this` for the same reason as the tiles path: the unit has
                 * to be EMITTED COMPOSITES, so a picture that produced none must not extend
                 * the hold. sum(hold lengths) == paired is the check. */
                coop_hold_run++;
            }
            if (!paired_this) {
                coop_stale_run++;
            } else {
                coop_stale_run = 0;
            }
            /* THIRD TRIGGER, and the one that actually fires in practice: ACCUMULATED
             * CONCEALMENT. Measured on a live v5 run, the pane feed does not stall --
             * `staleRun` never exceeded 1, because the peer delivers steadily at about
             * a third of our rate, so paired and unpaired pictures simply alternate.
             * The stall trigger above can therefore never fire, and neither can the
             * IRAP one (`idr_refresh_interval_sec=0`, so we never IRAP): `reboot=0`
             * every session. Meanwhile 60% of pictures are concealed and each one adds
             * prediction error that nothing removes.
             *
             * So trigger on the DAMAGE rather than on a stall: once enough pictures
             * have been concealed since the last clean start, force a paired IRAP.
             * That is what `idr_refresh_interval_sec` does for an ordinary stream, and
             * it bounds ghosting instead of letting it grow without limit. */
            if (coop_conceal_since_boot >= COOP_CONCEAL_BUDGET && coop_bootstrapped &&
                coop_rebootstraps < COOP_MAX_REBOOTSTRAPS) {
                coop_bootstrapped = false;
                coop_lastpane_len = 0;
                coop_lastpane_type = -1;
                coop_idr_requests = 0;
                coop_stale_run = 0;
                coop_conceal_since_boot = 0;
                coop_rebootstraps++;
                coop_coord_request_peer_idr();
                if (diag_log != NULL) {
                    fprintf(diag_log, "[COOP-LIVE] concealed %d since bootstrap -- "
                                      "re-bootstrapping to clear drift (#%u)\n",
                            COOP_CONCEAL_BUDGET, coop_rebootstraps);
                    fflush(diag_log);
                }
            }
            if (coop_stale_run >= COOP_STALE_PICTURES && coop_bootstrapped &&
                coop_rebootstraps < COOP_MAX_REBOOTSTRAPS) {
                coop_bootstrapped = false;
                coop_lastpane_len = 0;
                coop_lastpane_type = -1;
                coop_idr_requests = 0;
                coop_stale_run = 0;
                coop_conceal_since_boot = 0;
                coop_rebootstraps++;
                coop_coord_request_peer_idr();
                if (diag_log != NULL) {
                    fprintf(diag_log, "[COOP-LIVE] pane stale for %d pictures -- "
                                      "re-bootstrapping (#%u)\n", COOP_STALE_PICTURES,
                            coop_rebootstraps);
                    fflush(diag_log);
                }
            }
            /* CONCEALMENT: on a fallback we would otherwise emit OUR slice 1, which
             * writes our host's bottom half into the decoder's picture buffer -- and
             * the other PC's next inter slice then predicts from it, breaking that
             * pane's chain several times a second (observed: the pane decays to
             * black mush). Emitting slice 0 alone leaves the bottom CTUs uncoded so
             * the decoder conceals them from the co-located previous picture, which
             * is the content we want kept. Gated: an incomplete picture is a real
             * risk, so the sentinel-ABSENT path stays the safe one, latched at setup
             * (never a syscall on the 165fps path). */
            /* Only AFTER bootstrap: before the pane has taken over there is no
             * other-PC content to preserve, so blanking the bottom CTUs just makes
             * the picture worse -- observed as a black bottom half with clipping at
             * the seam. Until then, fall back to our own complete picture. */
            if (coop_conceal && coop_bootstrapped && !paired_this && mine.slice_count == 2 &&
                coop_splice_buf != NULL) {
                static const unsigned char sc[4] = {0, 0, 0, 1};
                size_t need = mine.non_vcl_len + sizeof(sc) + mine.slice_len[0];
                if (need <= coop_splice_cap && need <= buffer_size) {
                    size_t n2 = 0;
                    if (mine.non_vcl_len > 0) {
                        memcpy(coop_splice_buf, mine.non_vcl, mine.non_vcl_len);
                        n2 = mine.non_vcl_len;
                    }
                    memcpy(coop_splice_buf + n2, sc, sizeof(sc));
                    n2 += sizeof(sc);
                    memcpy(coop_splice_buf + n2, mine.slice[0], mine.slice_len[0]);
                    n2 += mine.slice_len[0];
                    memcpy(buffer, coop_splice_buf, n2);
                    length = n2;
                    coop_concealed++;
                    coop_conceal_since_boot++;
                }
            }
            /* Dump EVERY picture fed while co-op is live -- paired, concealed and
             * plain fallback alike. Dumping only the paired ones left gaps in the
             * sequence, so the offline decode could not show what the decoder
             * really sees (reference resolution depends on the pictures in
             * between). Decode this rather than trusting the TV, which conceals
             * bad slices silently. */
            if (coop_spliced_dump == NULL && coop_spliced_frames == 0 &&
                access("/tmp/coop-dump-spliced", F_OK) == 0) {
                coop_spliced_dump = fopen("/tmp/coop_spliced.h265", "wb");
            }
            if (coop_spliced_dump != NULL) {
                if (coop_spliced_frames < 200) {
                    fwrite(buffer, 1, length, coop_spliced_dump);
                    coop_spliced_frames++;
                } else {
                    fflush(coop_spliced_dump);
                    fclose(coop_spliced_dump);
                    coop_spliced_dump = NULL;
                }
            }
            if (diag_log != NULL && ((coop_live_paired + coop_live_fallback) % 300) == 0) {
                uint32_t childst = 0;
                if (coop_coord_child_exited(&childst)) {
                    fprintf(diag_log, "[COOP-LIVE] SECONDARY GONE: status=0x%08lx\n",
                            (unsigned long) childst);
                }
                fprintf(diag_log, "[COOP-LIVE] %s pid=%d v5 recv=%u replaced=%u fb_nopane=%u fb_mismatch=%u concealed=%u poc_fix=%u poc_fail=%u poc_unaligned=%u repeated=%u reboot=%u staleRun=%u concealRun=%u\n",
                        coop_role_tag(), (int) getpid(),
                        coop_coord_received(), coop_coord_replaced(),
                        coop_fb_nopane, coop_fb_mismatch, coop_concealed,
                        coop_poc_rewrites, coop_poc_failed, coop_poc_unaligned,
                        coop_repeated, coop_rebootstraps, coop_stale_run, coop_conceal_since_boot);
                fprintf(diag_log, "[COOP-LIVE] %s pid=%d paired=%u fallback=%u paneAge=%lums\n",
                        coop_role_tag(), (int) getpid(),
                        coop_live_paired, coop_live_fallback, age);
                fflush(diag_log);
            }
        }
    }

    /* Splice-path proof (gated, latched at setup): parse this access unit and
     * rebuild it from its own two slices, then feed THAT. The result must be
     * byte-identical, so a correct picture proves parse -> splice -> feed works
     * live on real host data -- the whole co-op path except the second source.
     * Any corruption here would appear before PC2 is ever involved. */
    if (coop_splice_test && coop_splice_buf != NULL) {
        coop_au_t au;
        if (coop_au_parse(buffer, length, &au) && au.slice_count == 2) {
            size_t n = coop_au_splice(&au, &au, coop_splice_buf, coop_splice_cap);
            if (n > 0) {
                coop_splice_ok++;
                if (n == length && memcmp(coop_splice_buf, buffer, n) == 0) {
                    coop_splice_identical++;
                }
                memcpy(buffer, coop_splice_buf, n);
                length = n;
            } else {
                coop_splice_fail++;
            }
        } else {
            coop_splice_fail++;
        }
        if (diag_log != NULL && (coop_splice_ok + coop_splice_fail) % 300 == 0) {
            fprintf(diag_log, "[COOP-SPLICE] spliced=%u identical=%u failed=%u\n",
                    coop_splice_ok, coop_splice_identical, coop_splice_fail);
            fflush(diag_log);
        }
    }

    /* DUMP WHAT THE DECODER ACTUALLY GETS. The existing /tmp/coop-dump writes the buffer
     * BEFORE composition, i.e. our own untouched stream, which can never show a fault in
     * the composite. This one is at the feed call, so the file is byte-for-byte what was
     * submitted -- decode it with ffmpeg and the reference-resolution errors that the TV
     * can only express as a grey picture get named exactly. */
    /* A DUMP THAT STARTS AT SESSION START CANNOT CONTAIN THE FAULT. 400 AUs is 2.4s at
     * 165fps, and the test pattern is not placed on the hosts until several seconds in, so
     * the window has to be positionable -- put a frame number in /tmp/coop-dump-out.
     *
     * AND IT HAS TO OPEN ON AN IRAP. The composite emits one IRAP, at session start: our host
     * runs an infinite GOP and the peer's IDRs are converted to TRAIL. Start a dump anywhere
     * else and ffmpeg has no keyframe to begin from, so the capture decodes to garbage and
     * says nothing. Ask our host for a keyframe as the window opens and begin on the IRAP
     * that comes back. This is the only way to see a PER-FRAME artefact: stills cannot
     * resolve one and the TV's own probe advances every 3s. */
    if (coop_dump_out != NULL) {
        coop_dump_out_seen++;
        if (coop_dump_out_seen < coop_dump_out_start) {
            /* before the window: nothing to do */
        } else if (coop_dump_out_seen == coop_dump_out_start) {
            LiRequestIdrFrame();
        } else if (!coop_dump_out_open) {
            /* Wait for the keyframe we asked for; VCL type 16..23 is an IRAP. */
            coop_au_t da;
            if (coop_au_parse(buffer, length, &da) && da.slice[0] != NULL) {
                /* IRAP test must follow the codec: an H.264 IDR is nal_unit_type 5, an HEVC IRAP is
                 * 16..23. Typing an AVC NAL the HEVC way opened this on the wrong pictures. */
                const bool h264_dump = (vdec_stream_format & VIDEO_FORMAT_MASK_H264) != 0;
                const int dt = h264_dump ? (da.slice[0][0] & 0x1F)
                                        : ((da.slice[0][0] >> 1) & 0x3F);
                if (h264_dump ? (dt == 5) : (dt >= 16 && dt <= 23)) {
                    coop_dump_out_open = true;
                    if (diag_log != NULL) {
                        fprintf(diag_log, "[COOP-DUMP] window opened on IRAP at n=%u\n",
                                coop_dump_out_seen);
                        fflush(diag_log);
                    }
                }
            }
        }
        /* THIS SITE NO LONGER WRITES -- IT ONLY OPENS THE WINDOW. Two live fwrite() calls for
         * the same picture (here and again at the feed) put EVERY access unit in the file
         * TWICE, and ffmpeg reads that back as `Duplicate POC in a sequence` +
         * `First slice in a frame missing` on a stream the TV had just rendered perfectly.
         * A capture that indicts a healthy stream is worse than no capture. The write lives
         * at the feed, which is the only point every emission passes through. */
        if (coop_dump_out_open && coop_dump_out_frames < 400 && diag_log != NULL) {
            /* One line per DUMPED picture, indexed by the same n ffmpeg will report, so the
             * frames that decode corrupt can be looked up directly instead of guessed at. */
            fprintf(diag_log, "[COOP-AU] n=%u peer_frame=%u gap=%u dropped=%u accept=%d\n",
                    coop_dump_out_frames, coop_hh_dbg_frame, coop_hh_dbg_gap,
                    coop_hh_dbg_dropped, (int) coop_hh_dbg_accept);
        }
    }
    /* IN TILES MODE, FEED ONLY WHAT THE TILED PATH BUILT. Every other branch produces a
     * picture carrying our PANE's SPS (34 CTB rows), and letting one through re-initialises the
     * decoder away from the 68-row composite -- which is why the first live tiles runs rendered
     * BLACK with `First slice in a frame missing` on all 400 pictures, even though the tiling
     * primitives decode cleanly offline. Dropping the picture is safe here in a way it is not on
     * the untiled path: there, our half alone is a legal one-slice picture; here it is not. */
    /* HEVC ONLY. The AVC path never sets coop_hh_tiled_ok -- it does not use tiles at all -- so a
     * stale /tmp/coop-hh-tiles would silently drop EVERY H.264 picture here. */
    if (coop_hh_tiles && !coop_hh_tiled_ok && app_launch_coop_requested() &&
        !(vdec_stream_format & VIDEO_FORMAT_MASK_H264)) {
        coop_hh_untiled_blocked++;
        /* GIVE THE POC BACK. Dropping the picture is right, but keeping its number is not:
         * the emitted sequence then has a hole, and every later slice's delta-coded RPS
         * references it. Only roll back if ours is still the most recent number issued --
         * a catch-up picture emitted later in this same call has taken a higher one and is
         * genuinely on the wire. */
        if (coop_hh_poc_pending != 0 && coop_hh_poc_pending == coop_hh_poc) {
            coop_hh_poc--;
            coop_hh_poc_returned++;
        }
        coop_hh_poc_pending = 0;
        if (diag_log != NULL && coop_hh_untiled_blocked % 300 == 1) {
            fprintf(diag_log, "[COOP-HH] tiles: blocked an UNTILED picture (#%u, %u POCs "
                              "returned) -- it would re-init the decoder to the pane's SPS\n",
                    coop_hh_untiled_blocked, coop_hh_poc_returned);
            fflush(diag_log);
        }
        return DR_OK;
    }
    /* CAPTURE WHAT IS ACTUALLY FED, not what an earlier branch thought it would feed.
     * The /tmp/coop-dump-out hook above sits earlier in this function and does NOT see every
     * emission -- measured: a self-mirror run climbed to mirrored=600 while the capture file
     * stayed 0 bytes. A record of the bitstream is only useful if it is the bitstream the
     * decoder got, so mirror the write here, at the one point everything passes through. */
    /* STATE AT THE WRITE SITE, not inferred from the source. A capture that stays at 0 bytes
     * has exactly three possible causes and they are indistinguishable from outside: the handle
     * is NULL, the window never opened, or this line is never reached. Print all three. */
    coop_dump_out_reach++;
    if (diag_log != NULL && coop_dump_out_reach % 300 == 1) {
        fprintf(diag_log, "[COOP-DUMP] at feed: reach=%u handle=%d open=%d frames=%u "
                          "seen=%u start=%u\n",
                coop_dump_out_reach, coop_dump_out != NULL ? 1 : 0, (int) coop_dump_out_open,
                coop_dump_out_frames, coop_dump_out_seen, coop_dump_out_start);
        fflush(diag_log);
    }
    if (coop_dump_out != NULL && coop_dump_out_open && coop_dump_out_frames < 400) {
        fwrite(buffer, 1, length, coop_dump_out);
        if (++coop_dump_out_frames == 400) {
            fflush(coop_dump_out); fclose(coop_dump_out); coop_dump_out = NULL;
        }
    }
    /* Rolling capture: every emitted picture, with its IRAP flag so the dump can
     * start somewhere decodable. */
    /* INDEPENDENT OF THE RING. The ring is only allocated when /tmp/coop-dump-ring is set, and
     * netcap must not inherit that condition -- it is armed by its own sentinel. */
    if (coop_netcap_armed && coop_cap_buf == NULL) {
        coop_au_t rn;
        bool nc_irap = false;
        if (coop_au_parse(buffer, length, &rn) && rn.slice[0] != NULL) {
            const bool h264_nc = (vdec_stream_format & VIDEO_FORMAT_MASK_H264) != 0;
            const int nt = h264_nc ? (rn.slice[0][0] & 0x1F) : ((rn.slice[0][0] >> 1) & 0x3F);
            nc_irap = h264_nc ? (nt == 5) : (nt >= 16 && nt <= 23);
        }
        coop_netcap_feed(buffer, length, decodeUnit->frameNumber, coop_hh_dbg_frame, nc_irap, 0);
    }
    if (coop_cap_buf != NULL) {
        coop_au_t ra;
        bool ra_irap = false;
        if (coop_au_parse(buffer, length, &ra) && ra.slice[0] != NULL) {
            /* IRAP test must follow the codec: an H.264 IDR is nal_unit_type 5, an HEVC IRAP is
             * 16..23. Typing an AVC NAL the HEVC way opened this on the wrong pictures. */
            const bool h264_cap = (vdec_stream_format & VIDEO_FORMAT_MASK_H264) != 0;
            const int rt = h264_cap ? (ra.slice[0][0] & 0x1F)
                                    : ((ra.slice[0][0] >> 1) & 0x3F);
            ra_irap = h264_cap ? (rt == 5) : (rt >= 16 && rt <= 23);
        }
        coop_cap_store(buffer, length, ra_irap);
        /* NETCAP tees here too, so it sees exactly the bytes the ring and the decoder see. */
        coop_netcap_feed(buffer, length, decodeUnit->frameNumber, coop_hh_dbg_frame, ra_irap, 0);
        /* Poll the trigger ~once a second. NEVER per picture: the hot path must
         * not stat() the filesystem at 165fps. */
        if (++coop_cap_poll >= 120u) {
            coop_cap_poll = 0;
            /* TRIGGER ON THE FILE'S MTIME, NOT ON DELETING IT. The app runs as a
             * different uid from the shell that arms the sentinel, and /tmp is
             * sticky -- so unlink() silently fails and the flush re-fires on every
             * poll. Measured: one `touch` produced seven dumps. Comparing mtime
             * needs no write permission at all, and `touch` remains the gesture. */
            struct stat ds;
            if (stat("/tmp/coop-dump-now", &ds) == 0 && ds.st_mtime != coop_cap_trigger_mtime) {
                coop_cap_trigger_mtime = ds.st_mtime;
                /* ASK BOTH HOSTS FOR A KEYFRAME AND WAIT FOR IT. Flushing here writes
                 * whatever is in the ring, which mid-session contains no IRAP and is
                 * therefore undecodable -- the exact dead end that made a dump look
                 * "broken" when the file was fine and simply had no keyframe to start
                 * from. Both sides are needed: a composite picture is only an IRAP when
                 * both tiles are. */
                coop_cap_want = true;
                coop_cap_want_polls = 0;
                LiRequestIdrFrame();
                coop_coord_request_peer_idr();
                if (diag_log != NULL) {
                    fprintf(diag_log, "[COOP-DUMP] ring: dump requested -- asked both hosts "
                                      "for a keyframe, waiting (%u AUs held, %u after an IRAP)\n",
                            coop_cap_count, coop_cap_after_irap());
                    fflush(diag_log);
                }
            }
            if (coop_cap_want) {
                coop_cap_want_polls++;
                const unsigned after = coop_cap_after_irap();
                if (after >= COOP_CAP_AFTER_IRAP) {
                    coop_cap_want = false;
                    coop_cap_flush();
                } else if (coop_cap_want_polls >= COOP_CAP_WAIT_POLLS) {
                    /* Out of patience: flush anyway. If no keyframe ever arrived the
                     * flush SAYS SO, which is the useful outcome -- it means the
                     * composite cannot produce a mid-session IRAP, and that is a
                     * finding about the stream rather than a broken capture. */
                    coop_cap_want = false;
                    if (diag_log != NULL) {
                        fprintf(diag_log, "[COOP-DUMP] ring: gave up waiting after %u polls "
                                          "(%u AUs after an IRAP)\n",
                                coop_cap_want_polls, after);
                        fflush(diag_log);
                    }
                    coop_cap_flush();
                }
            }
        }
    }
    /* This picture is going out, so the POC it took is SPENT -- nothing may hand it back.
     * Clearing here rather than at the gate keeps the rule in one place: the number is
     * returnable right up until the moment it reaches the decoder. */
    coop_hh_poc_pending = 0;
    /* coop_solo_active is latched once at stream setup: this is the per-frame hot
     * path at up to 165 fps, so it must not stat() the filesystem. */
    SS4S_VideoFeedResult result = !coop_solo_active
            ? diag_feed(FEED_SITE_ORDINARY, buffer, length, flags)
            : SS4S_VIDEO_FEED_OK;
    if (result == SS4S_VIDEO_FEED_OK) {
        if (decodeUnit->frameType == FRAME_TYPE_IDR) {
            frames_since_idr = 0;
        } else {
            frames_since_idr++;
        }
        const int idr_interval = app_configuration ? app_configuration->idr_refresh_interval_sec : 0;
        const bool hevc_stream = vdec_stream_format == VIDEO_FORMAT_H265 ||
                                 vdec_stream_format == VIDEO_FORMAT_H265_MAIN10;
        if (hevc_stream && idr_interval >= 2 && vdec_stream_target_fps > 0 &&
            frames_since_idr >= vdec_stream_target_fps * idr_interval) {
            LiRequestIdrFrame();
            frames_since_idr = 0;
        }
        if (vdec_stream_info.width == 0 || vdec_stream_info.height == 0) {
            stream_info_parse_size(decodeUnit, &vdec_stream_info);
        }
        vdec_temp_stats.totalSubmitTime += LiGetMillis() - decodeUnit->enqueueTimeMs;
        vdec_temp_stats.submittedFrames++;
        /* Bytes are counted inside diag_feed() now, at every site and by what was ACTUALLY
         * fed. Counting decodeUnit->fullLength here measured the primary AU only, so the
         * co-op composite's peer half — and every catch-up picture — was missing from the
         * delivered-bitrate figure the decode-budget warning was drawn from. */
        diag_feed_ok++;
        /* THE ONLY PLACE BOOTSTRAP MAY BE CLAIMED: the picture is now actually in the decoder. */

#if HAVE_STARFISH_DIRECT
        /* Mirror the same access unit into the second starfish pipeline (no-op unless
         * mirror mode armed) — proves two live decoders by putting a picture on screen. */
        coop_mirror_feed(buffer, length, decodeUnit->frameType == FRAME_TYPE_IDR);
#endif
        /* First frame (always the IDR) — log immediately so we capture it even on short sessions */
        if (diag_feed_ok == 1 && diag_log) {
            fprintf(diag_log, "[FIRST] frame=%d size=%d type=%s\n",
                    decodeUnit->frameNumber, decodeUnit->fullLength,
                    decodeUnit->frameType == FRAME_TYPE_IDR ? "IDR" : "P");
            fflush(diag_log);
        }
        /* Any frame large enough to risk a 512 KB socket-buffer burst overflow */
        if (decodeUnit->fullLength > 100 * 1024 && diag_log) {
            fprintf(diag_log, "[LARGE] frame=%d size=%d\n",
                    decodeUnit->frameNumber, decodeUnit->fullLength);
        }
        /* UDP drop counters once per second — lets us see exactly when drops spike */
        if (diag_log && ticksms - diag_last_snmp_ms >= 1000) {
            diag_snmp_snap(ticksms);
            diag_wifi_snap("WIFI", ticksms);
            diag_last_snmp_ms = ticksms;
        }
        if (diag_log && ticksms - diag_last_flush_ms >= 2000) {
            fprintf(diag_log, "[%lu] ok=%u idr_sent=%u idr_throttled=%u errors=%u frame=%d size=%d\n",
                    ticksms, diag_feed_ok, diag_idr_sent, diag_idr_throttled, diag_feed_errors,
                    decodeUnit->frameNumber, decodeUnit->fullLength);
            /* [LATENCY] -- THE SIGNAL FOR THE "STREAM IS FALLING BEHIND" WARNING.
             *
             * WHY THIS EXISTS. On 2026-09-09 the user measured TWENTY-ONE SECONDS of
             * mouse-to-screen latency with a game open, and EVERY counter we had read
             * healthy throughout: `ok=` advancing at a clean 120/s, errors=0, queued=0,
             * evicted=0, avcbroken=0. `ok=` counts feeds ACCEPTED, not frames PRESENTED,
             * so a pipeline that buffers instead of rejecting is invisible to it. The
             * latency scaled with CONTENT COMPLEXITY (still desktop fine, game terrible)
             * and disappeared at net_fps=60, i.e. it is a rate/complexity budget problem.
             *
             * WHAT THE TWO NUMBERS MEAN, and why both are needed:
             *   host_ms  = frameHostProcessingLatency, the HOST's own capture->encode time
             *              for the frame (protocol field, Limelight.h:155, 1/10 ms units;
             *              zero when the host does not supply it or the frame was repeated).
             *              This is the one that should rise when the host's encoder cannot
             *              keep up with the requested rate at that content complexity.
             *   sub_ms   = LiGetMillis() - enqueueTimeMs at feed: how long the AU sat in
             *              OUR pipeline between arrival and being fed. This rises instead
             *              if the DECODER applies backpressure and our feed loop stalls.
             * They separate host-side backlog from client-side backlog, which is exactly the
             * distinction we could not make while debugging this.
             *
             * NOT GATED ON THE STATS OVERLAY. vdec_stat_submit() only samples rtt and
             * decoder latency `if (streaming_stats_shown())` and returns early otherwise --
             * so the app measured latency only while somebody was looking at it. These two
             * fields are accumulated unconditionally (:3843 and :6696), so logging them here
             * costs nothing and works on an unattended run.
             *
             * NOTE SS4S_PlayerGetVideoLatency() IS USELESS ON THIS PLATFORM: it reads
             * player->stats.video, fed by SS4S_VideoStatsBeginFrame/EndFrame, and those have
             * NO CALLERS in the webos5 module -- so it always returns false and the overlay
             * shows "not available". Do not reach for it as the decoder-side signal. */
            uint32_t lat_frames = vdec_temp_stats.submittedFrames;
            if (lat_frames > 0) {
                /* DELIVERED BITRATE IS THE ONLY THING THAT PREDICTS THE RUNAWAY LATENCY.
                 * host_ms and sub_ms are logged beside it because they are the two backlogs we
                 * CAN see -- and they are logged precisely so a future reader can watch them stay
                 * flat while the fault rages: measured 4.3/1.1 at 16.5 s of real latency and
                 * 3.6/2.4 at 1.9 s. They exonerate the host and our own feed path; they do not
                 * detect the fault, which lives after the feed inside the TV's pipeline. */
                const double mbps = (double) diag_bytes_since_flush * 8.0 /
                                    ((double) (ticksms - diag_last_flush_ms) * 1000.0);
                /* THE DECODER'S OWN LATENCY, AND WHY IT IS SUDDENLY AVAILABLE.
                 *
                 * The record said SS4S_PlayerGetVideoLatency() was dead on webos5 because
                 * SS4S_VideoStatsBeginFrame/EndFrame have no callers in that module. True, and
                 * the wrong conclusion: the counter is ALSO fed by ReportFrame, which does have
                 * a caller — ndl_video.c:90 → library.c:133 → SS4S_StatsCounterReportFrame() —
                 * and that is exactly what player.c:114 reads back.
                 *
                 * The real reason nobody ever saw a value: vdec_stat_submit() returns early at
                 * `if (!show_stats) return;`, so the getter was only ever called while the stats
                 * OVERLAY was on screen. Same defect the comment above this block complains
                 * about for host_ms/sub_ms — the app measured latency only when someone was
                 * looking at it.
                 *
                 * WHAT IT MEANS. ndl_video.c:88 reads NDL_DirectVideoGetRenderBufferLength()
                 * every feed and reports bufLen * (now - lastFrameTime). So this is roughly
                 * DECODER QUEUE DEPTH x FEED INTERVAL, in ms. It is the first signal we have
                 * from AFTER the feed, which is the only place the runaway latency can still
                 * be hiding. dec_ms=-1 means the counter had no samples in the window.
                 *
                 * GRADE IT HONESTLY: whether it separates a STOPPED pipeline from a BACKLOGGED
                 * one is the open question this build exists to answer, not an assumption. A
                 * product of two terms can rise for either reason. */
                int lat_us = 0;
                const double dec_ms = SS4S_PlayerGetVideoLatency(player, 0, &lat_us)
                                          ? (double) lat_us / 1000.0 : -1.0;
                fprintf(diag_log, "[LATENCY] host_ms=%.1f sub_ms=%.1f dec_ms=%.1f frames=%u mbps=%.1f%s\n",
                        (double) vdec_temp_stats.totalCaptureLatency / (double) lat_frames / 10.0,
                        (double) vdec_temp_stats.totalSubmitTime / (double) lat_frames,
                        dec_ms, lat_frames, mbps,
                        /* DESCRIPTIVE, NOT CAUSAL. The old text asserted "expect SECONDS of
                         * latency ... lower the frame rate", which is an unproven causal claim
                         * and a bad user-facing recommendation: the 20 s reading that motivated
                         * it was retracted on 2026-09-10 (it was a FROZEN panel, not a queue).
                         * Keep the threshold as a marker; do not tell anyone what it means. */
                        mbps > COOP_DECODE_BUDGET_MBPS ? "  [over-budget-marker]" : "");
            }
            /* Per-site feed accounting. `ord` is the only site whose result was ever read
             * before this build; nr/rk/er are NOT_READY / REQUEST_KEYFRAME / ERROR. A nonzero
             * count at any catch-up site is the finding — it means pictures the composite
             * counted as delivered were refused by the decoder. */
            fprintf(diag_log, "[FEED]");
            for (int fs = 0; fs < FEED_SITE__COUNT; ++fs) {
                if (diag_feed_n[fs] == 0) { continue; }
                fprintf(diag_log, " %s[n=%u nr=%u rk=%u er=%u mb=%.1f]",
                        feed_site_name[fs], diag_feed_n[fs], diag_feed_notready[fs],
                        diag_feed_reqkey[fs], diag_feed_err[fs],
                        (double) diag_feed_bytes[fs] / 1048576.0);
            }
            fprintf(diag_log, "\n");
            /* [COOP-AGE] — the DURATION trace passover 07 §3 asked for first. Every other co-op
             * gauge is a count; the reported symptom is in seconds, so counts cannot settle it.
             *
             *   peer_ms   complete-arrival -> composed age of the peer pane (last/mean/max).
             *             Passover 07 §2B: if peer pictures wait for the primary's callback,
             *             this is where it shows.
             *   hold_ms   how long the peer half has been held/concealed (open run, and worst).
             *   broke_ms  uninterrupted broken-chain episode (open, and worst) -- §2A.
             *   recov     ms since the last peer-IDR request was ADMITTED, and since the last
             *             usable anchor was RECEIVED. A request that is admitted but never
             *             answered shows as req climbing while anchor does not.
             *
             * NOT A HEARTBEAT, and the difference matters: this prints from the feed path, so if
             * the primary stops delivering, this line stops too. A stall then shows only as a
             * GAP in the flush timestamps. A real heartbeat needs a timer thread and is not
             * built. */
            if (app_launch_coop_requested()) {
                const unsigned long nowm = coop_mono_ms();
                fprintf(diag_log, "[COOP-AGE] peer_ms[last=%lu mean=%lu max=%lu n=%u]"
                                  " hold_ms[open=%lu max=%lu] broke_ms[open=%lu max=%lu]"
                                  " recov[req=%lu anchor=%lu]\n",
                        coop_age_peer_ms_last,
                        coop_age_peer_n ? coop_age_peer_ms_sum / coop_age_peer_n : 0ul,
                        coop_age_peer_ms_max, coop_age_peer_n,
                        coop_hold_started_ms ? nowm - coop_hold_started_ms : 0ul, coop_hold_ms_max,
                        coop_broken_started_ms ? nowm - coop_broken_started_ms : 0ul,
                        coop_broken_ms_max,
                        coop_recov_request_ms ? nowm - coop_recov_request_ms : 0ul,
                        coop_recov_anchor_ms ? nowm - coop_recov_anchor_ms : 0ul);
            }
            diag_bytes_since_flush = 0;
            fflush(diag_log);
            diag_last_flush_ms = ticksms;
        }
        return DR_OK;
    } else if (result == SS4S_VIDEO_FEED_REQUEST_KEYFRAME) {
        unsigned long now = SDL_GetTicks();
        if (now - lastIdrRequestMs >= IDR_REQUEST_MIN_INTERVAL_MS) {
            lastIdrRequestMs = now;
            diag_idr_sent++;
            if (diag_log) {
                fprintf(diag_log, "[%lu] IDR_SENT frame=%d size=%d ok_so_far=%u\n",
                        now, decodeUnit->frameNumber, decodeUnit->fullLength, diag_feed_ok);
                fflush(diag_log);
            }
            return DR_NEED_IDR;
        }
        diag_idr_throttled++;
        if (diag_log) {
            fprintf(diag_log, "[%lu] IDR_THROTTLED frame=%d size=%d\n",
                    now, decodeUnit->frameNumber, decodeUnit->fullLength);
            fflush(diag_log);
        }
        return DR_OK;
    } else if (result == SS4S_VIDEO_FEED_NOT_READY) {
        /* Pipeline buffer temporarily full — drop the frame and keep streaming.
         * Calling session_interrupt() here was the root cause of post-33.31.68 stalls:
         * every transient buffer-full condition killed the session instead of self-healing. */
        if (diag_log) {
            fprintf(diag_log, "[%lu] NOT_READY_DROP frame=%d size=%d ok=%u\n",
                    (unsigned long) SDL_GetTicks(), decodeUnit->frameNumber,
                    decodeUnit->fullLength, diag_feed_ok);
            fflush(diag_log);
        }
        return DR_OK;
    } else {
        diag_feed_errors++;
        if (diag_log) {
            fprintf(diag_log, "[%lu] FEED_ERROR result=%d frame=%d size=%d ok=%u idr_sent=%u\n",
                    (unsigned long) SDL_GetTicks(), result, decodeUnit->frameNumber,
                    decodeUnit->fullLength, diag_feed_ok, diag_idr_sent);
            fflush(diag_log);
        }
        /* Startup grace. A starfish pipeline only accepts frames once its
         * LOADCOMPLETED callback has arrived and play() has run; frames that land
         * in that window are rejected. The SMP backend feeds immediately after a
         * successful open, so on this TV it died after three errors with ok=0 --
         * "decoder reported error" before it ever had a chance to start. Drop
         * early failures instead of killing the session; a genuinely broken
         * decoder still fails once the grace period expires. */
        if (diag_feed_ok == 0 && SDL_GetTicks() - vdec_setup_ms < VDEC_FEED_GRACE_MS) {
            return DR_OK;
        }
        commons_log_error("Session", "Video feed error %d", result);
        session_interrupt(session, false, STREAMING_INTERRUPT_DECODER);
        return DR_OK;
    }
}

void vdec_stat_submit(const struct VIDEO_STATS *src, unsigned long now) {
    struct VIDEO_STATS *dst = &vdec_summary_stats;
    memcpy(dst, src, sizeof(struct VIDEO_STATS));
    unsigned long delta = now - dst->measurementStartTimestamp;
    if (delta <= 0) { return; }
    dst->totalFps = (float) dst->totalFrames / ((float) delta / 1000);
    dst->receivedFps = (float) dst->receivedFrames / ((float) delta / 1000);
    dst->decodedFps = (float) dst->submittedFrames / ((float) delta / 1000);
    dst->currentBitrateKbps = (uint32_t) ((dst->receivedBytes * 8) / (delta / 1000.0f));
    if (diag_logs_active()) {
        if (vdec_totals.first_frame_ms == 0) {
            vdec_totals.first_frame_ms = now;
        }
        vdec_totals.frames += src->totalFrames;
        vdec_totals.received += src->receivedFrames;
        vdec_totals.dropped += src->networkDroppedFrames;
        vdec_totals.submitted += src->submittedFrames;
        vdec_totals.bytes += src->receivedBytes;
        if (vdec_totals.worst_fps < 0 || dst->receivedFps < vdec_totals.worst_fps) {
            vdec_totals.worst_fps = dst->receivedFps;
        }
        uint32_t rtt = 0, rtt_var = 0;
        LiGetEstimatedRttInfo(&rtt, &rtt_var);
        vdec_totals.rtt_last = rtt;
        vdec_totals.rtt_var_last = rtt_var;
        if (rtt > vdec_totals.rtt_max) {
            vdec_totals.rtt_max = rtt;
        }
        int latency_us = 0;
        const bool have_latency = player != NULL && SS4S_PlayerGetVideoLatency(player, 0, &latency_us);
        if (have_latency) {
            vdec_totals.latency_sum_ms += latency_us / 1000.0;
            vdec_totals.latency_samples++;
        }
        if (now - vdec_totals.last_log_ms >= 10000) {
            vdec_totals.last_log_ms = now;
            /* SINCE THE PREVIOUS LINE, not the overlay's last ~2 s window: that sampled one window in five, and a
             * stream that lost 15% overall showed "0 lost" on every line (VEGA, 2026-09-30). */
            const uint64_t span_frames = vdec_totals.frames - vdec_totals.logged_frames;
            const uint64_t span_dropped = vdec_totals.dropped - vdec_totals.logged_dropped;
            vdec_totals.logged_frames = vdec_totals.frames;
            vdec_totals.logged_dropped = vdec_totals.dropped;
            /* A DECODER BUSY NEARLY ALL THE TIME TAKES NO MORE FRAMES: the extra ones are dropped in THIS device before
             * decoding, and moonlight-common-c counts them as network losses (VEGA, 2026-09-30: 4K at 32 ms a frame
             * capped at ~30 fps and "lost" 15% with no drop at the NIC or in the OS; the same stream at 1080p, 8 ms a
             * frame: 0 lost). Say so, or a reader chases the network. */
            const double busy = have_latency ? latency_us / 1000.0 * dst->receivedFps / 1000.0 : 0.0;
            char limit_hint[160] = "";
            if (span_dropped > 0 && busy >= 0.75 && latency_us > 0) {   /* 0.85 missed a 0.81 start window */
                snprintf(limit_hint, sizeof(limit_hint), " -- the decoder is the limit (%.0f ms a frame = at most "
                         "~%.0f fps): these were most likely dropped in this device, not on the network",
                         latency_us / 1000.0, 1000000.0 / latency_us);
            }
            commons_log_info("Stats", "%s %dx%d: %.1f fps received, %.1f decoded, %u kbps, %llu of %llu frames lost "
                                      "on the network since the last line, RTT %u ms (+/-%u), decoder latency %s%.1f ms%s",
                             vdec_stream_info.format ? vdec_stream_info.format : "?", vdec_stream_info.width,
                             vdec_stream_info.height, dst->receivedFps, dst->decodedFps,
                             (unsigned) (src->receivedBytes * 8 / (delta > 0 ? delta : 1)),   /* bits per ms = kbps */
                             (unsigned long long) span_dropped, (unsigned long long) span_frames, rtt, rtt_var,
                             have_latency ? "" : "n/a ",
                             have_latency ? latency_us / 1000.0 : 0.0, limit_hint);
        }
    }
    const bool show_stats = streaming_stats_shown();
    if (show_stats) {
        LiGetEstimatedRttInfo(&dst->rtt, &dst->rttVariance);
    }
    if (!show_stats) {
        return;
    }
    int latencyUs = 0;
    if (SS4S_PlayerGetVideoLatency(player, 0, &latencyUs)) {
        dst->avgDecoderLatency = (float) latencyUs / 1000.0f;
        vdec_stream_info.has_decoder_latency = true;
    } else {
        dst->avgDecoderLatency = 0;
    }
    app_bus_post(session->app, (bus_actionfunc) streaming_refresh_stats, NULL);
}

void vdec_stats_log_totals(void) {
    if (!diag_logs_active()) {
        return;
    }
    if (vdec_totals.start_ms == 0) {
        commons_log_info("Stats", "Stream totals: the video decoder was never set up");
        return;
    }
    const unsigned long now = SDL_GetTicks();
    const double secs = (now - vdec_totals.start_ms) / 1000.0;
    const double video_secs = vdec_totals.first_frame_ms ? (now - vdec_totals.first_frame_ms) / 1000.0 : 0;
    const double avg_latency_ms = vdec_totals.latency_samples ? vdec_totals.latency_sum_ms / vdec_totals.latency_samples
                                                              : 0.0;
    const double avg_fps = video_secs > 0 ? (double) vdec_totals.received / video_secs : 0.0;
    char limit_hint[160] = "";   /* see the per-window Stats line: a saturated decoder, not the network */
    if (vdec_totals.dropped > 0 && avg_latency_ms > 0 && avg_latency_ms * avg_fps / 1000.0 >= 0.75) {
        snprintf(limit_hint, sizeof(limit_hint), " -- the decoder was the limit (%.0f ms a frame = at most ~%.0f fps): "
                 "the losses were most likely in this device, not on the network", avg_latency_ms, 1000.0 / avg_latency_ms);
    }
    commons_log_info("Stats", "Stream totals: %.0f s since the decoder opened, first frame after %lu ms; %llu frames "
                              "(%llu received, %llu lost on the network = %.2f%%, %llu decoded); average %.0f kbps; "
                              "lowest received fps in a window %.1f; RTT last %u ms (+/-%u), max %u ms; decoder "
                              "latency average %s%.1f ms%s",
                     secs, vdec_totals.first_frame_ms ? vdec_totals.first_frame_ms - vdec_totals.start_ms : 0,
                     (unsigned long long) vdec_totals.frames, (unsigned long long) vdec_totals.received,
                     (unsigned long long) vdec_totals.dropped,
                     vdec_totals.frames ? 100.0 * (double) vdec_totals.dropped / (double) vdec_totals.frames : 0.0,
                     (unsigned long long) vdec_totals.submitted,
                     video_secs > 0 ? (double) vdec_totals.bytes * 8 / 1000 / video_secs : 0.0,
                     vdec_totals.worst_fps < 0 ? 0.0f : vdec_totals.worst_fps, vdec_totals.rtt_last,
                     vdec_totals.rtt_var_last, vdec_totals.rtt_max, vdec_totals.latency_samples ? "" : "n/a ",
                     vdec_totals.latency_samples ? vdec_totals.latency_sum_ms / vdec_totals.latency_samples : 0.0,
                     limit_hint);
    vdec_totals.start_ms = 0;   /* a later stream that fails before its decoder opens must not report these */
}

void stream_info_parse_size(PDECODE_UNIT decodeUnit, struct VIDEO_INFO *info) {
    if (decodeUnit->frameType != FRAME_TYPE_IDR) { return; }
    if (vdec_stream_format & VIDEO_FORMAT_MASK_AV1) {
        return;
    }
    for (PLENTRY entry = decodeUnit->bufferList; entry != NULL; entry = entry->next) {
        if (entry->bufferType != BUFFER_TYPE_SPS) { continue; }
        sps_dimension_t dimension;
        if (vdec_stream_format & VIDEO_FORMAT_MASK_H264) {
            sps_parse_dimension_h264((const unsigned char *) &entry->data[4], &dimension);
        } else if (vdec_stream_format & VIDEO_FORMAT_MASK_H265) {
            sps_parse_dimension_hevc((const unsigned char *) &entry->data[4], &dimension);
        } else {
            info->width = info->height = -1;
            return;
        }
        info->width = dimension.width;
        info->height = dimension.height;
        /* This parses the INCOMING unit's SPS, which is the host's -- it still says
         * 1088 because the height is doubled later, during composition. Reporting the
         * source height sizes the video plane at 3840x1080 (32:9), which the TV then
         * letterboxes into a 540-tall band: the composite is correct but drawn small
         * with black above and below. Report what will actually be displayed. */
        if (coop_vt_enabled) {
            /* VERTICAL: the panes sit SIDE BY SIDE, so it is the WIDTH that doubles. The
             * incoming SPS is the host's 1920-wide pane; sizing the plane from that while
             * feeding it a 3840-wide tiled composite produces no output at all -- the
             * stream simply never opens. Same reasoning as the height case below, other
             * axis. */
            info->width = dimension.width * 2;
        } else if (app_launch_coop_requested() && coop_live) {
            info->height = dimension.height * 2;
        }
        return;
    }
}

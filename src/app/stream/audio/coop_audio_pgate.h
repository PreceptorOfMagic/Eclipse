/*
 * Co-op audio PASSIVE GATE: play a sound both hosts make once, by matching waveforms and closing a gate -- no spectral
 * analysis of the audio. The default filter for "play shared sounds once" since 2026-10-06; the design is the user's (10-05):
 *
 *   "keep a buffer ... compare whatever PC is sending audio soonest against the slower (the leader is master, the
 *    second is slave); when the slave has a wave pattern the same as the master we simply close the gate."
 *
 * What the measurements of 10-04/10-05 added to that (coop-audio-mix memory, scratchpad/coop-audio-live-20261005,
 * scratchpad/coop-audio-align):
 *   - The LEADER is not a property of a PC or of the session -- not even of one moment: in run 3 the treble was a copy
 *     at +12 ms (PC2 later) while the bass was a copy at -180 ms (VEGA later). So every offset is checked in its own
 *     direction, per band and per channel, every block.
 *   - A cutscene is not one event: narrator lines arrive at their own offsets and the gate closes per line, and it
 *     closes AHEAD of the copy, using the earlier copy's onset (the stream's own lag is the time budget).
 *   - Each player's positional background is not a copy and sits in the same audio: one gate per BAND (six, summing
 *     back to the input exactly) and per CHANNEL (the two hosts' stereo images differed by up to 3 dB).
 *   - Separately coded copies never match perfectly, least of all in the treble (0.84-0.93 correlation for an exact
 *     copy): each band's threshold is relative to the best match that band has actually achieved.
 *   - The hosts' levels differ (PC2 +4.6 dB for identical audio): the gate learns that from clean matches and
 *     balances the two streams, so closing a band always costs the same (one copy instead of two).
 *   - The two hosts' audio clocks drift (0.1-5 ppm): each offset is tracked to the sample.
 *
 * Legacy streaming latency: one block (5 ms) on BOTH streams. The complete-block API instead
 * consumes a block the caller has already collected and returns that same block without another hold.
 * Pure C99, no platform headers; same calling convention as coop_audio_dedupe / coop_audio_gate.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COOP_PGATE_BANDS 6          /* < 250, 250-600, 600-1500, 1500-3500, 3500-8000, > 8000 Hz */
#define COOP_PGATE_MAX_OFFSETS 16

typedef struct coop_pgate coop_pgate_t;

typedef struct {
    int offsets;                                        /* known offsets in the bank */
    int offset_samples[COOP_PGATE_MAX_OFFSETS];         /* > 0: peer is the later copy; < 0: self is */
    float balance_db;                                   /* learned level of peer relative to self for the same audio */
    bool balance_applied;
    float ceiling[COOP_PGATE_BANDS];                    /* learned best match (share) per band */
    unsigned blocks, searches, admitted, evicted, balance_samples;
    unsigned drift_steps, recentres, recentres_while_active;   /* offset changes: +-1 sample drift, jumps */
    unsigned pred_confirmed, pred_failed;
    unsigned dropouts;
    unsigned fast_tracks;
    unsigned env_lines, env_forced;
    unsigned twin_splits;                                     /* a lag beat its working pitch-twin neighbour: own slot */
    unsigned side_switches, sticky_holds;                     /* band conflicts: taken from the holder / refused */
    unsigned sentinel_skips;                                  /* idle offsets not fully checked this block */                           /* envelope-line detections; bands closed on an envelope match */                                     /* offset jumps by fast tracking (speed-varied lines) */                                        /* blocks where a stream fell to digital silence */                     /* predictions per offset that the copy did/did not confirm */
} coop_pgate_stats_t;

/** rate 8000..96000, channels 1..2, max_lag_ms 5..1000 = the largest offset between copies to look for. */
coop_pgate_t *coop_pgate_create(int rate, int channels, int max_lag_ms);

/** QUALITY. FAST: decide on the newest 5 ms, predicting a copy's onset from the earlier copy (5 ms delay).
 * ACCURATE (user's pick 2026-10-10, offline "H"): decide with COOP_PGATE_ACCURATE_LOOKAHEAD_MS of the copy's future in
 * view and no predictions -- onsets caught as they start, far less switching -- at that much more delay. Look-ahead
 * holds blocks back, which only the streaming entry (coop_pgate_process) can do: a gate first driven through
 * coop_pgate_process_ready_block* runs FAST whatever was asked for. Settled on the first process call. */
enum { COOP_PGATE_FAST = 0, COOP_PGATE_ACCURATE = 1 };
#define COOP_PGATE_ACCURATE_LOOKAHEAD_MS 40   /* the default and the evaluated value; the setting allows 0..100 */
/** lookahead_ms is used for ACCURATE only, rounded up to whole 5 ms blocks and clamped to 0..100 (0 = no look-ahead,
 * still no predictions). */
coop_pgate_t *coop_pgate_create_quality(int rate, int channels, int max_lag_ms, int quality, int lookahead_ms);
/** The quality actually running (ACCURATE = predictions off): only once the streaming entry has started it. */
int coop_pgate_quality(const coop_pgate_t *g);
void coop_pgate_destroy(coop_pgate_t *g);

/** In place, interleaved int16, `frames` per stream; both come back delayed by coop_pgate_latency_frames().
 * Do not mix streaming and complete-block calls within an epoch. Invalid calls leave buffers/state unchanged. */
void coop_pgate_process(coop_pgate_t *g, int16_t *self, int16_t *peer, int frames);

/** Legacy streaming delay only; not additional latency imposed by process_ready_block(). */
/* Shared-content restore (0 = off, clamped to 0..6 dB): while a band is closed on a copy, lift the other
 * stream's same band by up to db so shared content keeps two copies' loudness. Call on the gate's own thread. */
void coop_pgate_set_restore_db(coop_pgate_t *g, float db);
int coop_pgate_latency_frames(const coop_pgate_t *g);

/** Required caller-complete quantum ((rate / 1000) * 5 frames); zero for NULL. */
int coop_pgate_block_frames(const coop_pgate_t *g);

/** Process exactly one complete interleaved block and return that SAME input-indexed block in place.
 * The caller owns collection/playout time. This adds no held block, but still requires the full input block.
 * Returns false without consuming/modifying input on wrong size, NULL, or a legacy-streaming epoch.
 * Neither API is reentrant. Run on the audio worker, not the device callback. */
bool coop_pgate_process_ready_block(coop_pgate_t *g, int16_t *self, int16_t *peer, int frames);

enum {
    COOP_PGATE_VALID_SELF = 1u,
    COOP_PGATE_VALID_PEER = 2u,
    COOP_PGATE_VALID_BOTH = 3u
};
/** As above, with source validity for this entire block. Mark missing/concealed samples invalid.
 * Invalid input remains in the audio path but cannot confirm waveform/envelope matches or onset predictions.
 * An invalid current source passes unchanged; the other source can still match valid past samples.
 * Complete-block processing uses explicit validity instead of the legacy current-digital-silence heuristic.
 * Blocks overlapping invalid history are excluded from matching; epoch reset clears the validity history.
 * Unknown mask bits reject the call unchanged. The simpler ready API marks both sources valid. */
bool coop_pgate_process_ready_block_valid(coop_pgate_t *g, int16_t *self, int16_t *peer, int frames, unsigned valid_streams);

/** Begin a new source epoch: clear histories, matches, gains and sample indices; preserve the test hook.
 * No allocation; clears retained sample arrays, so call only between worker blocks, never concurrently.
 * Returns false only for NULL. A successful reset allows either API on its next valid processing call. */
bool coop_pgate_reset(coop_pgate_t *g);

void coop_pgate_get_stats(const coop_pgate_t *g, coop_pgate_stats_t *out);

/* ---- the band split, exposed so tests can split known components exactly as the gate does ---- */
#define COOP_PGATE_XOVERS (COOP_PGATE_BANDS - 1)
typedef struct {
    float b0[COOP_PGATE_XOVERS], b1[COOP_PGATE_XOVERS], b2[COOP_PGATE_XOVERS], a1[COOP_PGATE_XOVERS],
          a2[COOP_PGATE_XOVERS];
    float z1[COOP_PGATE_XOVERS], z2[COOP_PGATE_XOVERS];
} coop_pgate_xover_t;
void coop_pgate_xover_init(coop_pgate_xover_t *x, int rate);
/** One sample in; the COOP_PGATE_BANDS outputs sum back to the input exactly. */
void coop_pgate_xover_run(coop_pgate_xover_t *x, float in, float *bands);

/** TEST HOOK: once per block, the absolute input index of the block's first sample, its length, and each stream's
 *  per-channel, per-band GATE gain at the start and end of the block (linear ramp between them). The level balance
 *  is NOT included (see balance_db): the hook describes echo removal only. */
typedef void (*coop_pgate_hook_t)(void *ctx, unsigned long long start, int frames,
                                  const float g_start[2][2][COOP_PGATE_BANDS], const float g_end[2][2][COOP_PGATE_BANDS],
                                  const unsigned char why[2][2][COOP_PGATE_BANDS]);
/* why: 0 open; 1 closed on a match this block; 2 closed by the hold after a match; 3 closed by a PREDICTION still
 * waiting for the copy to arrive */
void coop_pgate_set_hook(coop_pgate_t *g, coop_pgate_hook_t hook, void *ctx);

#ifdef __cplusplus
}
#endif

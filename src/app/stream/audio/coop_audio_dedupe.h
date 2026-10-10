/*
 * Co-op audio duplicate suppression: play a sound both hosts make ONCE, not twice slightly apart.
 *
 * THE PROBLEM (user, 2026-10-03): in a co-op game both PCs often play the SAME sound -- narration, cutscene audio,
 * music -- each from its own copy of the game. The two copies arrive tens to hundreds of milliseconds apart (each
 * game's own timing, plus two separate streams), so the mix plays them as an echo.
 *
 * WHAT THIS DOES. Of the two streams, the one whose copy arrives LATER has the shared content turned down; the
 * earlier copy is kept, so nothing is delayed to line the copies up. Each player's OWN sounds (their gunfire,
 * footsteps) differ between the two games and are left alone.
 *
 * HOW, AND WHY NOT SUBTRACTION. An echo canceller would subtract the earlier stream from the later one -- but the
 * earlier stream also carries ITS player's own sounds, and subtracting those injects an inverted, delayed copy of
 * player 1's gunfire into player 2's audio. So this never subtracts; it only applies per-frequency GAINS <= 1 to the
 * later stream:
 *   1. a delay estimator (GCC-PHAT cross-correlation on a 12 kHz mono copy, +/- max_lag, ~10x per second) finds how
 *      far apart the shared content is and which stream is later; a full-rate correlation at that delay says how much
 *      the streams currently share (rho), which gates everything below;
 *   2. both streams go through the same short STFT (512 samples, hop 256, sqrt-Hann): per frequency bin, the share of
 *      the later stream that is coherent with the earlier stream at that delay is estimated (magnitude-squared
 *      coherence, smoothed over a few frames) and that share is removed by a gain, floored at -26 dB;
 *   3. with nothing shared (rho low), the gains return to exactly 1 and the stream passes through unchanged.
 * Both streams take the same path, so both are delayed by the same 512 samples (10.7 ms at 48 kHz) and stay aligned
 * with each other.
 *
 * Pure C99, no SDL/platform headers: the mixer calls it, and the test program feeds it WAV files offline.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coop_dedupe coop_dedupe_t;

typedef struct {
    int lag_samples;        /* > 0: the second stream (peer) is later; < 0: the first (self) is later */
    float rho;              /* correlation of the two streams at that lag, 0..1 */
    float amount;           /* how strongly suppression is currently applied, 0..1 */
    int later;              /* 0 none, 1 self is being suppressed, 2 peer is being suppressed */
    unsigned estimates;     /* delay estimates run so far */
    unsigned lag_changes;   /* times the chosen lag moved */
} coop_dedupe_stats_t;

/** rate in Hz (8000..96000), channels 1..2, max_lag_ms = the largest offset between copies to look for. */
coop_dedupe_t *coop_dedupe_create(int rate, int channels, int max_lag_ms);
void coop_dedupe_destroy(coop_dedupe_t *dd);

/** In place: self and peer are interleaved int16 blocks of `frames` frames each, as the mixer would sum them.
 *  On return both are the processed streams, each delayed by coop_dedupe_latency_frames(). */
void coop_dedupe_process(coop_dedupe_t *dd, int16_t *self, int16_t *peer, int frames);

/** The constant delay both streams get (frames). */
int coop_dedupe_latency_frames(void);

void coop_dedupe_get_stats(const coop_dedupe_t *dd, coop_dedupe_stats_t *out);

#ifdef __cplusplus
}
#endif

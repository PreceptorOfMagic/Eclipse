/*
 * Co-op audio echo gate: play a sound both hosts make ONCE, even when the two copies arrive at SEVERAL different
 * offsets at the same time. PROTOTYPE (2026-10-04) -- offline only, not wired into the mixer yet.
 *
 * WHY A SECOND DESIGN. coop_audio_dedupe tracks ONE delay. A real capture (Halo co-op, same mission section played
 * twice, 2026-10-04) showed two or three duplicate layers at once, switching within 0.5 s:
 *   - +60 ms: the stream-path difference, under everything the games keep in sync (music, ambience);
 *   - +272/+294/+353 ms: cutscene-timeline audio, which each PC starts on its own timing, different every run.
 * A single-delay filter keeps re-locking between them, and the user heard it "go in and out".
 *
 * WHAT THIS DOES. It keeps a BANK of recently seen offsets and checks every one of them continuously, frame by
 * frame. Per frequency bin, whichever offset explains the later stream's content is removed by a GAIN (never by
 * subtraction, for the reason in coop_audio_dedupe.h). Offsets already in the bank re-engage within a few frames, so
 * only a brand-new offset leaks -- the time it takes to find it (~100-200 ms). Nothing is delayed to line the copies
 * up; both streams pass through the same short STFT and share its fixed delay.
 *
 * SAFETY RULES, each from a failure this has to avoid:
 *   - a bin is never cut in BOTH streams at once (periodic sound can make two opposite offsets look plausible; cutting
 *     both would delete the sound entirely rather than once);
 *   - an offset enters the bank only after it is found twice at the same place, or once with a very strong match --
 *     so two players firing the same gun at an unrelated moment cannot create an offset;
 *   - an offset that stops explaining anything leaves the bank after GT_EXPIRE_S.
 *
 * Pure C99, no SDL/platform headers, same calling convention as coop_audio_dedupe.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COOP_GATE_MAX_LAGS 10

typedef struct coop_gate coop_gate_t;

typedef struct {
    int count;                                  /* offsets in the bank */
    int lag_samples[COOP_GATE_MAX_LAGS];        /* > 0: peer later; < 0: self later */
    float amount[COOP_GATE_MAX_LAGS];           /* how strongly that offset is being removed, 0..1 */
    float activity[COOP_GATE_MAX_LAGS];         /* energy-weighted coherence at that offset this frame, 0..1 */
    float self_cut_db, peer_cut_db;             /* energy removed from each stream over the last frame */
    unsigned searches, admitted, expired;
} coop_gate_stats_t;

/** rate 8000..96000, channels 1..2, max_lag_ms 5..1000 = the largest offset between copies to look for. */
coop_gate_t *coop_gate_create(int rate, int channels, int max_lag_ms);
void coop_gate_destroy(coop_gate_t *g);

/** In place, interleaved int16, `frames` per stream; both come back delayed by coop_gate_latency_frames(). */
void coop_gate_process(coop_gate_t *g, int16_t *self, int16_t *peer, int frames);

int coop_gate_latency_frames(void);

void coop_gate_get_stats(const coop_gate_t *g, coop_gate_stats_t *out);

/** TEST HOOK (offline evaluation only): called once per STFT hop with the absolute input index of the frame's first
 *  sample and the per-bin amplitude gains (GT_N/2+1 each) applied to self and peer, so a test can apply the SAME
 *  gains to known components (echo-only, own-only) and measure exactly what was removed. */
typedef void (*coop_gate_gain_hook_t)(void *ctx, unsigned long long frame_start, const float *gain_self,
                                      const float *gain_peer, int bins);
void coop_gate_set_gain_hook(coop_gate_t *g, coop_gate_gain_hook_t hook, void *ctx);

#ifdef __cplusplus
}
#endif

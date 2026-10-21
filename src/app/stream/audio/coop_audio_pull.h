#pragma once
#include "coop_audio_spsc.h"

/* Decode-ready -> post-DSP handoff ages per source in 1 ms buckets; the last bucket
 * collects everything >= COOP_AUDIO_AGE_BUCKETS-1 ms. A maximum alone cannot tell a
 * rare missed device period from a standing offset; percentiles can. */
#define COOP_AUDIO_AGE_BUCKETS 64

typedef struct {
    uint64_t output_frames, consumed[2], missing[2], concealed[2], discontinuities[2];
    uint64_t timed[2], untimed[2], age_max_us[2], epoch[2];
    uint64_t age_hist[2][COOP_AUDIO_AGE_BUCKETS];
    /* Produced-ahead mode only (coop_audio_pull_set_ahead): source frames cut from a queue grown too deep, and device
     * frames the callback found nothing rendered for (played as silence). */
    uint64_t trimmed[2], device_short;
} coop_audio_pull_stats_t;

/* Both callbacks run ONLY on the device thread. No locks, allocation, I/O, or
 * calls into the player are allowed. Mix may alter both inputs and writes out.
 * Observe sees the unmodified inputs alongside output at block production time;
 * stats() instead describes frames actually handed to the device callback.
 * In produced-ahead mode both run on the worker instead, and the per-source
 * stats describe frames when they are mixed (the device takes them up to
 * `ahead` frames later). */
typedef void (*coop_audio_pull_mix_fn)(void *ctx, int16_t *self, int16_t *peer,
    int16_t *out, unsigned frames, unsigned valid, unsigned new_epoch);
typedef void (*coop_audio_pull_observe_fn)(void *ctx, const int16_t *self, const int16_t *peer,
    const int16_t *out, unsigned frames, uint64_t block_index);

typedef struct {
    coop_audio_spsc_t source[2];
    unsigned channels, block, offset;
    coop_audio_frame_t *stamp[2];
    int16_t *raw[2], *work[2], *mixed;
    unsigned real[2];
    uint64_t next_index[2], produced;
    bool indexed[2];
    coop_audio_pull_stats_t stats;
    coop_audio_pull_mix_fn mix;
    coop_audio_pull_observe_fn observe;
    /* Optional same-clock reading at handoff after DSP; NULL uses the supplied
     * callback-entry timestamp (component fixtures / explicitly weaker scope). */
    uint64_t (*now_us)(void *ctx);
    void *ctx;
    /* PRODUCED AHEAD (coop_audio_pull_set_ahead; ahead 0 = the callback renders). A worker mixes complete blocks
     * into `ready`, keeping `ahead` frames there; the device callback only copies them out. */
    unsigned ahead, trim_above, trim_to;
    coop_audio_spsc_t ready;
    coop_audio_frame_t *drain;
    unsigned drain_cap;
    atomic_ullong device_frames, device_short;
} coop_audio_pull_t;

bool coop_audio_pull_init(coop_audio_pull_t *p, unsigned channels, unsigned block,
    unsigned capacity, coop_audio_pull_mix_fn mix, coop_audio_pull_observe_fn observe, void *ctx);
/* After device Close has joined its callback and both producers have stopped. */
void coop_audio_pull_destroy(coop_audio_pull_t *p);
/* Device callback only. Timestamp is same-machine decode clock, NOT DAC time. */
unsigned coop_audio_pull_render(coop_audio_pull_t *p, int16_t *pcm, unsigned frames,
    uint64_t callback_begin_us);

/* PRODUCED-AHEAD MODE, for a device whose callback cannot also carry the mix and the gate (the SHIELD TV, 2026-10-22:
 * the gate costs ~1.2 M cycles per 5 ms block, and rendering it inside the callback overran 9% of the device's
 * deadlines; each overrun played silence that stayed behind as backlog). Call once after init and before the device
 * starts. A source queue deeper than `trim_above` frames is cut back to `trim_to` -- its oldest frames dropped, so the
 * next block reads as a discontinuity and cannot confirm a gate match -- since nothing else bounds the backlog a slow
 * or stalled device leaves behind. */
bool coop_audio_pull_set_ahead(coop_audio_pull_t *p, unsigned ahead_frames, unsigned trim_above, unsigned trim_to);

/* Produced-ahead worker only: trim deep sources, then render blocks until `ahead` frames wait for the device. The mix
 * and observe callbacks run here, not on the device thread. Timestamp as for render. Returns blocks rendered. */
unsigned coop_audio_pull_produce(coop_audio_pull_t *p, uint64_t now_us);

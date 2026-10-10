#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

/* One producer and one consumer; each owns its cursor. PCM and identity are
 * published together. No mutex, allocation, overwrite or retry loop in push/pop.
 * Lifecycle operations require both owners stopped. Capacity excludes one empty
 * slot used to distinguish full from empty, including across cursor wrap. */
typedef struct {
    int16_t pcm[2];
    uint32_t flags;
    uint64_t index, ready_us, epoch;
} coop_audio_frame_t;

typedef struct {
    coop_audio_frame_t *frames;
    unsigned slots;
    atomic_uint read, write;
} coop_audio_spsc_t;

bool coop_audio_spsc_init(coop_audio_spsc_t *q, unsigned capacity);
void coop_audio_spsc_destroy(coop_audio_spsc_t *q);
/* Producer-only. A false result retains the entire packet with the caller. */
bool coop_audio_spsc_push(coop_audio_spsc_t *q, const int16_t *pcm, unsigned frames,
                          unsigned channels, uint64_t index, uint64_t ready_us,
                          uint64_t epoch, unsigned flags);
/* Consumer-only. Stops at an epoch boundary. Returns real frames; the caller
 * decides how to represent a missing tail and never mistakes it for evidence. */
unsigned coop_audio_spsc_pop(coop_audio_spsc_t *q, coop_audio_frame_t *out, unsigned frames);
/* Owner-side approximate depth for telemetry, never a synchronization promise. */
unsigned coop_audio_spsc_count(const coop_audio_spsc_t *q);

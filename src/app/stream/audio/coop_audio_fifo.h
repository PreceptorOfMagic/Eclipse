#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Indexed PCM storage, not a playback clock. The caller serializes access.
 * Full queues reject the incoming block; accepted samples are never overwritten.
 * ready_us is a local decode-completion time, not a host capture timestamp. */
typedef struct {
    uint64_t index, ready_us;
    uint32_t flags;
} coop_audio_stamp_t;

typedef struct {
    int16_t *pcm;
    coop_audio_stamp_t *stamp;
    unsigned capacity, channels, head, count;
    uint64_t accepted, consumed, rejected, discarded, missing, max_age_us;
} coop_audio_fifo_t;

bool coop_audio_fifo_init(coop_audio_fifo_t *q, unsigned capacity, unsigned channels);
void coop_audio_fifo_destroy(coop_audio_fifo_t *q);
void coop_audio_fifo_clear(coop_audio_fifo_t *q);
bool coop_audio_fifo_push(coop_audio_fifo_t *q, const int16_t *pcm, unsigned frames,
                          uint64_t first_index, uint64_t ready_us, uint32_t flags);
/* Peek stops at a packet/validity boundary so IPC can preserve its metadata. */
unsigned coop_audio_fifo_peek(const coop_audio_fifo_t *q, int16_t *pcm, unsigned frames,
                              coop_audio_stamp_t *first);
/* Like peek, but one run may span packets: contiguous indices and equal flags only, and *first carries the
 * OLDEST packet's ready_us. A sender that moves one packet per round trip can never catch up on a backlog. */
unsigned coop_audio_fifo_peek_run(const coop_audio_fifo_t *q, int16_t *pcm, unsigned frames,
                                  coop_audio_stamp_t *first);
void coop_audio_fifo_consume(coop_audio_fifo_t *q, unsigned frames, uint64_t now_us);
/* Output-clock use: take all available PCM up to frames, zero only the missing
 * tail. There is no priming threshold or whole-buffer reset after a short read. */
unsigned coop_audio_fifo_render(coop_audio_fifo_t *q, int16_t *pcm, unsigned frames,
                                uint64_t now_us);

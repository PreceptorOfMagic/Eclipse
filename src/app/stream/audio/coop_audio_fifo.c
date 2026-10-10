#include "coop_audio_fifo.h"

#include <stdlib.h>
#include <string.h>

bool coop_audio_fifo_init(coop_audio_fifo_t *q, unsigned capacity, unsigned channels) {
    if (!q || !capacity || !channels || channels > 2) return false;
    memset(q, 0, sizeof(*q));
    q->pcm = calloc(capacity, channels * sizeof(*q->pcm));
    q->stamp = calloc(capacity, sizeof(*q->stamp));
    if (!q->pcm || !q->stamp) {
        coop_audio_fifo_destroy(q);
        return false;
    }
    q->capacity = capacity;
    q->channels = channels;
    return true;
}

void coop_audio_fifo_destroy(coop_audio_fifo_t *q) {
    if (!q) return;
    free(q->pcm);
    free(q->stamp);
    memset(q, 0, sizeof(*q));
}

void coop_audio_fifo_clear(coop_audio_fifo_t *q) {
    q->discarded += q->count;
    q->head = q->count = 0;
}

bool coop_audio_fifo_push(coop_audio_fifo_t *q, const int16_t *pcm, unsigned frames,
                          uint64_t first_index, uint64_t ready_us, uint32_t flags) {
    if (!q || !q->pcm || !pcm || frames > q->capacity - q->count) {
        if (q) q->rejected += frames;
        return false;
    }
    unsigned pos = (q->head + q->count) % q->capacity;
    for (unsigned i = 0; i < frames; ++i) {
        memcpy(q->pcm + pos * q->channels, pcm + i * q->channels,
               q->channels * sizeof(*pcm));
        q->stamp[pos] = (coop_audio_stamp_t) {first_index + i, ready_us, flags};
        pos = (pos + 1) % q->capacity;
    }
    q->count += frames;
    q->accepted += frames;
    return true;
}

static unsigned fifo_peek(const coop_audio_fifo_t *q, int16_t *pcm, unsigned frames,
                          coop_audio_stamp_t *first, bool split_packets) {
    if (!q->count || !frames) return 0;
    const coop_audio_stamp_t start = q->stamp[q->head];
    if (first) *first = start;
    unsigned take = q->count < frames ? q->count : frames;
    for (unsigned i = 0; i < take; ++i) {
        unsigned pos = (q->head + i) % q->capacity;
        const coop_audio_stamp_t *s = &q->stamp[pos];
        if (s->index != start.index + i || s->flags != start.flags ||
            (split_packets && s->ready_us != start.ready_us))
            return i;
        memcpy(pcm + i * q->channels, q->pcm + pos * q->channels,
               q->channels * sizeof(*pcm));
    }
    return take;
}

unsigned coop_audio_fifo_peek(const coop_audio_fifo_t *q, int16_t *pcm, unsigned frames,
                              coop_audio_stamp_t *first) {
    return fifo_peek(q, pcm, frames, first, true);
}

unsigned coop_audio_fifo_peek_run(const coop_audio_fifo_t *q, int16_t *pcm, unsigned frames,
                                  coop_audio_stamp_t *first) {
    return fifo_peek(q, pcm, frames, first, false);
}

void coop_audio_fifo_consume(coop_audio_fifo_t *q, unsigned frames, uint64_t now_us) {
    if (frames > q->count) frames = q->count;
    if (frames) {
        uint64_t ready = q->stamp[q->head].ready_us;
        if (ready && now_us >= ready && now_us - ready > q->max_age_us)
            q->max_age_us = now_us - ready;
        q->head = (q->head + frames) % q->capacity;
        q->count -= frames;
        q->consumed += frames;
    }
}

unsigned coop_audio_fifo_render(coop_audio_fifo_t *q, int16_t *pcm, unsigned frames,
                                uint64_t now_us) {
    unsigned take = q->count < frames ? q->count : frames;
    for (unsigned i = 0; i < take; ++i) {
        unsigned pos = (q->head + i) % q->capacity;
        memcpy(pcm + i * q->channels, q->pcm + pos * q->channels,
               q->channels * sizeof(*pcm));
    }
    memset(pcm + take * q->channels, 0, (frames - take) * q->channels * sizeof(*pcm));
    q->missing += frames - take;
    coop_audio_fifo_consume(q, take, now_us);
    return take;
}

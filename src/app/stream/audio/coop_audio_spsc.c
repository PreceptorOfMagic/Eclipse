#include "coop_audio_spsc.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

bool coop_audio_spsc_init(coop_audio_spsc_t *q, unsigned capacity) {
    if (!q || !capacity || capacity == UINT_MAX) return false;
    memset(q, 0, sizeof(*q));
    atomic_init(&q->read, 0);
    atomic_init(&q->write, 0);
    if (!atomic_is_lock_free(&q->read) || !atomic_is_lock_free(&q->write)) return false;
    q->frames = calloc((size_t)capacity + 1, sizeof(*q->frames));
    if (!q->frames) return false;
    q->slots = capacity + 1;
    return true;
}

void coop_audio_spsc_destroy(coop_audio_spsc_t *q) {
    if (!q) return;
    free(q->frames);
    memset(q, 0, sizeof(*q));
}

static unsigned distance(unsigned read, unsigned write, unsigned slots) {
    return write >= read ? write - read : slots - read + write;
}

unsigned coop_audio_spsc_count(const coop_audio_spsc_t *q) {
    if (!q || !q->frames) return 0;
    unsigned read = atomic_load_explicit(&q->read, memory_order_acquire);
    unsigned write = atomic_load_explicit(&q->write, memory_order_acquire);
    return distance(read, write, q->slots);
}

bool coop_audio_spsc_push(coop_audio_spsc_t *q, const int16_t *pcm, unsigned frames,
                          unsigned channels, uint64_t index, uint64_t ready_us,
                          uint64_t epoch, unsigned flags) {
    if (!q || !q->frames || !pcm || !channels || channels > 2 || !epoch ||
        index > UINT64_MAX - frames) return false;
    unsigned write = atomic_load_explicit(&q->write, memory_order_relaxed);
    unsigned read = atomic_load_explicit(&q->read, memory_order_acquire);
    if (frames > q->slots - 1 - distance(read, write, q->slots)) return false;
    for (unsigned i = 0; i < frames; ++i) {
        q->frames[write] = (coop_audio_frame_t) {
            .pcm = {pcm[(size_t)i * channels], channels == 2 ? pcm[(size_t)i * channels + 1] : 0},
            .flags = flags, .index = index + i, .ready_us = ready_us, .epoch = epoch
        };
        if (++write == q->slots) write = 0;
    }
    atomic_store_explicit(&q->write, write, memory_order_release);
    return true;
}

unsigned coop_audio_spsc_pop(coop_audio_spsc_t *q, coop_audio_frame_t *out, unsigned frames) {
    if (!q || !q->frames || !out) return 0;
    unsigned read = atomic_load_explicit(&q->read, memory_order_relaxed);
    unsigned write = atomic_load_explicit(&q->write, memory_order_acquire);
    unsigned count = distance(read, write, q->slots);
    if (frames > count) frames = count;
    unsigned take = 0;
    for (; take < frames; ++take) {
        if (take && q->frames[read].epoch != out[0].epoch) break;
        out[take] = q->frames[read];
        if (++read == q->slots) read = 0;
    }
    atomic_store_explicit(&q->read, read, memory_order_release);
    return take;
}

#include "coop_audio_output.h"
#include <stdlib.h>
#include <string.h>

bool coop_output_init(coop_output_t *out, unsigned rate, unsigned channels, unsigned block_frames,
                      uint64_t generation) {
    if (!out || !rate || !channels || channels > 2 || !block_frames || !generation) return false;
    memset(out, 0, sizeof(*out));
    out->pcm = calloc(block_frames, channels * sizeof(*out->pcm));
    if (!out->pcm) return false;
    out->rate = rate; out->channels = channels; out->block_frames = block_frames;
    out->generation = generation;
    return true;
}

void coop_output_destroy(coop_output_t *out) {
    if (!out) return;
    free(out->pcm);
    memset(out, 0, sizeof(*out));
}

int coop_output_step(coop_output_t *out, const coop_output_clock_t *clock,
                     coop_output_render_fn render, coop_output_feed_fn feed, void *ctx) {
    if (!out || !out->pcm || !clock || !render || !feed) return COOP_OUTPUT_ERROR;
    if (out->failed || clock->generation != out->generation || clock->rate != out->rate || !clock->quantum_frames) {
        out->failed = true;
        return COOP_OUTPUT_ERROR;
    }
    const unsigned target = clock->quantum_frames > out->block_frames ? clock->quantum_frames : out->block_frames;
    if (!out->pending && clock->queued_frames >= target) return COOP_OUTPUT_IDLE;
    if (!out->pending) {
        render(ctx, out->pcm, out->block_frames);
        out->pending = true;
    }
    int result = feed(ctx, out->pcm, out->block_frames);
    if (result == COOP_OUTPUT_FEED_OK) {
        out->submitted_frames += out->block_frames;
        out->pending = false;
        return COOP_OUTPUT_PROGRESS;
    }
    out->retries++;
    if (result == COOP_OUTPUT_FEED_ERROR) {
        out->failed = true;
        return COOP_OUTPUT_ERROR;
    }
    return COOP_OUTPUT_RETRY;
}

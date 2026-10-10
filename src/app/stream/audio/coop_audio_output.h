#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Device-independent output owner. One call either idles, renders one complete
 * block, or retries that exact block. The caller supplies a coherent backend
 * snapshot and owns threading/lifetimes. This is not a timer clock. */
typedef struct {
    uint32_t rate, quantum_frames;
    uint64_t generation, queued_frames;
} coop_output_clock_t;

typedef struct {
    unsigned block_frames, channels, rate;
    uint64_t generation, submitted_frames, retries;
    int16_t *pcm;
    bool pending, failed;
} coop_output_t;

enum { COOP_OUTPUT_ERROR=-1, COOP_OUTPUT_IDLE=0, COOP_OUTPUT_PROGRESS=1, COOP_OUTPUT_RETRY=2 };
enum { COOP_OUTPUT_FEED_ERROR=-1, COOP_OUTPUT_FEED_OK=0, COOP_OUTPUT_FEED_WAIT=1 };
typedef void (*coop_output_render_fn)(void *ctx, int16_t *pcm, unsigned frames);
typedef int (*coop_output_feed_fn)(void *ctx, const int16_t *pcm, unsigned frames);

bool coop_output_init(coop_output_t *out, unsigned rate, unsigned channels, unsigned block_frames,
                      uint64_t generation);
void coop_output_destroy(coop_output_t *out);
int coop_output_step(coop_output_t *out, const coop_output_clock_t *clock,
                     coop_output_render_fn render, coop_output_feed_fn feed, void *ctx);

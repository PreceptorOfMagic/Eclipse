/* coop_audio_fifo_peek_run: what the secondary sends per IPC message. A run spans packets (different ready_us)
 * but never an index gap (a rejected block) or a flags change (PLC), and reports the OLDEST packet's stamp.
 * Build: cc -I src/app/stream/audio tests/coop/test_coop_audio_fifo_run.c src/app/stream/audio/coop_audio_fifo.c */
#include "coop_audio_fifo.h"

#include <assert.h>
#include <stdio.h>

#define PKT 240

static void push(coop_audio_fifo_t *q, uint64_t index, uint64_t ready, uint32_t flags) {
    int16_t pcm[PKT * 2];
    for (unsigned i = 0; i < PKT * 2; ++i) pcm[i] = (int16_t) (index + i / 2);
    assert(coop_audio_fifo_push(q, pcm, PKT, index, ready, flags));
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    coop_audio_fifo_t q;
    int16_t out[960 * 2];
    coop_audio_stamp_t first;
    assert(coop_audio_fifo_init(&q, 24000, 2));

    /* Five contiguous packets decoded at different times: peek keeps one packet, a run takes up to the cap. */
    for (unsigned p = 0; p < 5; ++p) push(&q, 1000 + p * PKT, 5000 + p * 5000, 0);
    assert(coop_audio_fifo_peek(&q, out, 960, &first) == PKT);
    assert(coop_audio_fifo_peek_run(&q, out, 960, &first) == 960);
    assert(first.index == 1000 && first.ready_us == 5000);   /* the oldest packet's stamp */
    for (unsigned i = 0; i < 960; ++i) assert(out[i * 2] == (int16_t) (1000 + i) && out[i * 2 + 1] == out[i * 2]);
    coop_audio_fifo_consume(&q, 960, 30000);
    assert(coop_audio_fifo_peek_run(&q, out, 960, &first) == PKT && first.index == 1960);
    coop_audio_fifo_consume(&q, PKT, 30000);
    assert(q.count == 0);

    /* An index gap (the block between was rejected) ends the run, so the receiver still sees the hole. */
    push(&q, 0, 1, 0);
    push(&q, PKT, 2, 0);
    push(&q, 3 * PKT, 3, 0);
    assert(coop_audio_fifo_peek_run(&q, out, 960, &first) == 2 * PKT);
    coop_audio_fifo_consume(&q, 2 * PKT, 10);
    assert(coop_audio_fifo_peek_run(&q, out, 960, &first) == PKT && first.index == 3 * PKT);
    coop_audio_fifo_consume(&q, PKT, 10);

    /* A flags change (concealed audio) ends the run, so PLC frames keep their own message. */
    push(&q, 0, 1, 0);
    push(&q, PKT, 2, 1);
    push(&q, 2 * PKT, 3, 1);
    push(&q, 3 * PKT, 4, 0);
    assert(coop_audio_fifo_peek_run(&q, out, 960, &first) == PKT && first.flags == 0);
    coop_audio_fifo_consume(&q, PKT, 10);
    assert(coop_audio_fifo_peek_run(&q, out, 960, &first) == 2 * PKT && first.flags == 1 && first.ready_us == 2);
    coop_audio_fifo_consume(&q, 2 * PKT, 10);
    assert(coop_audio_fifo_peek_run(&q, out, 960, &first) == PKT && first.flags == 0);
    coop_audio_fifo_consume(&q, PKT, 10);

    /* Wrap-around: a run that crosses the end of the storage is still contiguous. */
    coop_audio_fifo_t w;
    assert(coop_audio_fifo_init(&w, 4 * PKT, 2));
    for (unsigned p = 0; p < 3; ++p) push(&w, p * PKT, p + 1, 0);
    coop_audio_fifo_consume(&w, 3 * PKT, 10);
    for (unsigned p = 3; p < 6; ++p) push(&w, p * PKT, p + 1, 0);   /* occupies slots 3, 0, 1 */
    assert(coop_audio_fifo_peek_run(&w, out, 960, &first) == 3 * PKT && first.index == 3 * PKT);
    for (unsigned i = 0; i < 3 * PKT; ++i) assert(out[i * 2] == (int16_t) (3 * PKT + i));
    coop_audio_fifo_destroy(&w);

    coop_audio_fifo_destroy(&q);
    puts("fifo run peek PASS");
    return 0;
}

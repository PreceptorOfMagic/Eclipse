/* Real local-socket coordinator contract, not live audio acceptance.
 * Compile this TU with the normal app header paths and section GC; it includes
 * production coordinator code to exercise its actual private receive state.
 * Link coop_msg_channel.c, coop_audio_fifo.c and coop_audio_pgate.c, -pthread -lm.
 * No app, host connection, sentinel, or output device is opened by this test.
 */
#ifdef NDEBUG
#error "Transport contract assertions must be enabled"
#endif
#include <assert.h>
#include <signal.h>

/* The included production logger must not contaminate a real session log. */
#undef AURORA_DIAG_LOG
#undef AURORA_SECONDARY_LOG
#define AURORA_DIAG_LOG "/dev/null"
#define AURORA_SECONDARY_LOG "/dev/null"
#include "../../src/app/stream/audio/coop_audio.c"

const char *coop_secondary_mode(void) { return NULL; }

static coop_msg_channel_t sender = COOP_MSG_CHANNEL_INITIALIZER;
static int sender_fd = -1;

static void connect_pair(int type) {
    int fd[2];
    assert(socketpair(AF_UNIX, type, 0, fd) == 0);
    assert(coop_msg_set_nonblocking(fd[0]));
    assert(coop_msg_set_nonblocking(fd[1]));
    const size_t limit = sizeof(coop_audio_hdr_t) + COOP_AUDIO_MAX_FRAMES * sizeof(int16_t);
    assert(coop_msg_channel_attach(&sender, fd[0], limit));
    assert(coop_msg_channel_attach(&coord_channel, fd[1], limit));
    sender_fd = fd[0];
    coord_peer_fd = fd[1];
    peer_format_ok = peer_format_logged = false;
    peer_pcm_pending = peer_ack_pending = false;
}

static void disconnect_pair(void) {
    coop_msg_channel_detach(&sender);
    if (sender_fd >= 0) close(sender_fd);
    sender_fd = -1;
    if (coord_peer_fd >= 0) coord_disconnect();
}

static void settle(void) {
    /* Bounded synchronous local I/O; no sleeps or live app scheduling. */
    for (unsigned i = 0; i < 16; ++i) {
        assert(coop_msg_channel_flush(&sender) != COOP_MSG_ERROR);
        coop_audio_drain();
        if (coord_peer_fd < 0) break;
    }
}

static void config(uint64_t epoch) {
    const coop_audio_hdr_t h = {.magic=COOP_AUDIO_MAGIC, .type=COOP_AUDIO_MSG_CONFIG,
        .rate=48000, .channels=1, .epoch=epoch};
    assert(coop_msg_channel_send(&sender, &h, sizeof(h)) == COOP_MSG_ACCEPTED);
    settle();
    assert(peer_format_ok && peer_epoch == epoch);
}

static void pcm(uint64_t first, unsigned frames, int base) {
    unsigned char message[sizeof(coop_audio_hdr_t) + 16 * sizeof(int16_t)];
    assert(frames <= 16);
    const coop_audio_hdr_t h = {.magic=COOP_AUDIO_MAGIC, .type=COOP_AUDIO_MSG_PCM,
        .frames=(uint16_t)frames, .rate=48000, .channels=1,
        .ready_us=audio_time_us(), .first_frame=first, .epoch=peer_epoch};
    memcpy(message, &h, sizeof(h));
    for (unsigned i = 0; i < frames; ++i) {
        const int16_t value = (int16_t)(base + (int)first + (int)i + 1);
        memcpy(message + sizeof(h) + i * sizeof(value), &value, sizeof(value));
    }
    assert(coop_msg_channel_send(&sender, message, sizeof(h) + frames * sizeof(int16_t)) == COOP_MSG_ACCEPTED);
    settle();
}

static void ack(uint64_t epoch, uint64_t end) {
    coop_audio_hdr_t h;
    size_t length = 0;
    assert(coop_msg_channel_receive(&sender, &h, sizeof(h), &length) == COOP_MSG_RECV_MESSAGE);
    assert(length == sizeof(h) && h.magic == COOP_AUDIO_MAGIC && h.type == COOP_AUDIO_MSG_ACK);
    assert(h.epoch == epoch && h.first_frame == end);
}

static void no_ack(void) {
    coop_audio_hdr_t h;
    size_t length = 0;
    assert(coop_msg_channel_receive(&sender, &h, sizeof(h), &length) == COOP_MSG_RECV_IDLE);
}

static void take(unsigned frames, int first_value) {
    int16_t out[16];
    assert(frames <= 16);
    assert(coop_audio_fifo_render(&input_fifo[1], out, frames, audio_time_us()) == frames);
    for (unsigned i = 0; i < frames; ++i) assert(out[i] == first_value + (int)i);
    timing_consumed_frames += frames;
}

static void exercise(int type) {
    coord_rate = 48000;
    coord_channels = 1;
    output_clocked = true;
    ring_head = ring_tail = 0;
    peer_epoch = peer_duplicates = 0;
    timing_have_sequence = false;
    timing_next_frame = timing_received_frames = timing_consumed_frames = 0;
    timing_link_discarded_frames = timing_gap_frames = timing_overflow_frames = 0;
    timing_plc_frames = timing_invalid_timestamp_frames = 0;
    stat_received = stat_dropped = 0;
    assert(coop_audio_fifo_init(&input_fifo[1], 8, 1));
    gate = coop_pgate_create(48000, 1, 1000);
    assert(gate);
    int16_t a[240] = {0}, b[240] = {0};
    assert(coop_pgate_process_ready_block(gate, a, b, 240));
    coop_pgate_stats_t stats;

    connect_pair(type);
    config(101);
    coop_pgate_get_stats(gate, &stats);
    assert(stats.blocks == 1); /* Initial config preserves fresh epoch state. */
    pcm(0, 4, 0);
    assert(input_fifo[1].count == 4 && timing_received_frames == 4);
    /* Lose the real ACK in the old socket. Reconnect/retransmit identical PCM. */
    disconnect_pair();
    connect_pair(type);
    config(101);
    pcm(0, 4, 0);
    ack(101, 4);
    assert(peer_duplicates == 4 && input_fifo[1].count == 4 && timing_received_frames == 4);
    pcm(4, 4, 0);
    ack(101, 8);
    pcm(8, 3, 0);
    assert(peer_pcm_pending && input_fifo[1].count == 8 && timing_received_frames == 8);
    no_ack();
    assert(input_fifo[1].rejected == 0 && timing_overflow_frames == 0);
    take(4, 1);
    settle();
    ack(101, 11);
    assert(!peer_pcm_pending && input_fifo[1].count == 7);
    take(7, 5); /* Every unique original frame arrived exactly once, in order. */
    pcm(11, 2, 0);
    ack(101, 13);

    config(202);
    assert(input_fifo[1].count == 0 && input_fifo[1].discarded == 2);
    assert(timing_link_discarded_frames == 2 && !timing_have_sequence);
    coop_pgate_get_stats(gate, &stats);
    assert(stats.blocks == 0); /* A replacement source clears prior gate epoch. */
    pcm(0, 4, 1000);
    ack(202, 4);
    const unsigned dropped = stat_dropped;
    pcm(2, 4, 2000); /* Partial overlap crosses accepted end: reject link/data. */
    assert(coord_peer_fd < 0 && stat_dropped == dropped + 1);
    assert(input_fifo[1].count == 4 && timing_received_frames == 17);
    take(4, 1001);
    assert(input_fifo[1].accepted == 17 && input_fifo[1].consumed == 15);
    assert(input_fifo[1].accepted == input_fifo[1].consumed + input_fifo[1].discarded + input_fifo[1].count);
    assert(!input_fifo[1].rejected && !input_fifo[1].missing && !timing_gap_frames);
    assert(!timing_invalid_timestamp_frames && !timing_overflow_frames);
    disconnect_pair();
    coop_audio_fifo_destroy(&input_fifo[1]);
    coop_pgate_destroy(gate);
    gate = NULL;
    output_clocked = false;
    printf("CA4S %s: exclusive ACK, lost ACK/reconnect dedup, full-queue pending, epoch reset, overlap rejection and indexed conservation passed\n",
           type == SOCK_SEQPACKET ? "SEQPACKET" : "STREAM");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    exercise(SOCK_SEQPACKET);
    exercise(SOCK_STREAM);
    return 0;
}

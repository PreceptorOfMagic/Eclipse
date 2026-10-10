/* Drive the actual RtpAudioQueue.c with scripted RTP audio and check what a stream's
 * FIRST packets become. No sockets or decoder. Build with -DEXPECT_UPSTREAM_START to run
 * the same scripts against the pre-fix behaviour (whole first FEC block skipped). */
/* Platform.h defines NDEBUG unless LC_DEBUG is set, which would silently disable every
 * assert below; force it so the fixture can fail whatever flags it is built with. */
#ifndef LC_DEBUG
#define LC_DEBUG
#endif
#ifndef RTPAQ_SOURCE
#define RTPAQ_SOURCE "../../core/moonlight-common-c/src/RtpAudioQueue.c"
#endif
#include RTPAQ_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

int AudioPacketDuration = 5;
int AppVersionQuad[4] = {7, 1, 431, -1};   /* Sunshine-family host: FEC-capable queue */
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
static uint64_t clock_ms = 1000;
uint64_t PltGetMillis(void) { return clock_ms; }
static void quiet_log(const char *fmt, ...) { (void) fmt; }

#define HDR 8   /* custom header room, as AudioStream asks for QUEUED_AUDIO_PACKET_HEADER */
static RTP_AUDIO_QUEUE q;
static int delivered[256], ndelivered, lost;

static void take(uint16_t seq) { assert(ndelivered < 256); delivered[ndelivered++] = seq; }

static void drain(void) {
    uint16_t len;
    uint8_t *p;
    while ((p = (uint8_t *) RtpaGetQueuedPacket(&q, HDR, &len)) != NULL) {
        if (len == 0) lost++;
        else take(((PRTP_PACKET) (p + HDR))->sequenceNumber);
        free(p);
    }
}

/* Host-order fields, as AudioStream byte-swaps before RtpaAddPacket. */
static void data(uint16_t seq) {
    uint8_t buf[sizeof(RTP_PACKET) + 20] = {0};
    PRTP_PACKET p = (PRTP_PACKET) buf;
    p->header = 0x80; p->packetType = 97; p->sequenceNumber = seq;
    p->timestamp = 1000u + (uint32_t) seq * 5u; p->ssrc = 0;
    const int rc = RtpaAddPacket(&q, p, sizeof(buf));
    if (RTPQ_HANDLE_NOW(rc)) take(seq);
    else if (RTPQ_PACKET_READY(rc)) drain();
}

static void fec(uint16_t base, uint8_t index) {
    uint8_t buf[sizeof(RTP_PACKET) + sizeof(AUDIO_FEC_HEADER) + 20] = {0};
    PRTP_PACKET p = (PRTP_PACKET) buf;
    PAUDIO_FEC_HEADER h = (PAUDIO_FEC_HEADER) (p + 1);
    p->header = 0x80; p->packetType = 127; p->sequenceNumber = (uint16_t) (base + RTPA_DATA_SHARDS + index);
    h->fecShardIndex = index; h->payloadType = 97;
    h->baseSequenceNumber = BE16(base); h->baseTimestamp = BE32(1000u + (uint32_t) base * 5u); h->ssrc = 0;
    const int rc = RtpaAddPacket(&q, p, sizeof(buf));
    if (RTPQ_PACKET_READY(rc)) drain();
}

static void reset(void) {
    RtpaCleanupQueue(&q);
    RtpaInitializeQueue(&q);
    assert(!q.incompatibleServer);
    ndelivered = lost = 0;
}

static void expect_run(uint16_t first, int count) {
    if (ndelivered != count || (count && delivered[0] != first)) {
        printf("expected %d from %u, got %d (lost %d):", count, first, ndelivered, lost);
        for (int i = 0; i < ndelivered; ++i) printf(" %d", delivered[i]);
        printf("\n");
    }
    assert(ndelivered == count);
    for (int i = 0; i < count; ++i) assert(delivered[i] == (uint16_t) (first + i));
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    ListenerCallbacks.logMessage = quiet_log;
    RtpaInitializeQueue(&q);

    /* A: stream's first packet is the first data shard of block 0 (Apollo starts at 0). */
    reset(); for (int s = 0; s < 12; ++s) data((uint16_t) s);
#ifdef EXPECT_UPSTREAM_START
    expect_run(4, 8);
#else
    expect_run(0, 12);
#endif
    /* B: first packet mid-block: its block's earlier shards were missed, so start at the
     *    next boundary exactly as upstream does. */
    reset(); for (int s = 1; s < 12; ++s) data((uint16_t) s);
    expect_run(4, 8);
    /* C: a non-zero boundary start. */
    reset(); for (int s = 100; s < 112; ++s) data((uint16_t) s);
#ifdef EXPECT_UPSTREAM_START
    expect_run(104, 8);
#else
    expect_run(100, 12);
#endif
    /* D: an FEC shard arrives first: upstream skip of that block is kept. */
    reset(); fec(0, 0); for (int s = 0; s < 8; ++s) data((uint16_t) s);
    expect_run(4, 4);
    /* E: boundary start just before the 16-bit wrap. Upstream's next boundary wraps to 0,
     *    which was also its "not started" marker, so it re-synchronizes and loses TWO blocks. */
    reset(); for (int s = 65532; s < 65532 + 12; ++s) data((uint16_t) s);
#ifdef EXPECT_UPSTREAM_START
    expect_run(4, 4);
#else
    expect_run(65532, 12);
#endif
    /* F: a kept first block with a missing shard still completes like any later block:
     *    0,1 handled now; 3 waits; after the OOS wait the hole is released (lost) and 3..7 follow. */
    reset(); data(0); data(1); data(3);
    clock_ms += 50;
    for (int s = 4; s < 8; ++s) data((uint16_t) s);
#ifndef EXPECT_UPSTREAM_START
    assert(ndelivered == 7 && lost == 1);
    const int want[] = {0, 1, 3, 4, 5, 6, 7};
    for (int i = 0; i < 7; ++i) assert(delivered[i] == want[i]);
#endif
    RtpaCleanupQueue(&q);
    puts("rtp audio queue start scenarios PASS");
    return 0;
}

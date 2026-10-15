/*
 * Host unit test of the Android MediaCodec module's decoder restarts (third_party/ss4s/modules/android/mediacodec.c),
 * compiled into this test with fakes for the NDK calls (ndk-stubs/ for the declarations) and a short grace.
 *
 * On the SHIELD TV the TV switching input put the device to sleep, Android abandoned the video surface, and the
 * restart onto it failed: Feed() returned an error and the stream ended as a decoder failure, before the activity
 * reported going to the background (2026-10-15). A failed restart is now retried at the next keyframes for a grace
 * period, dropping pictures meanwhile; only a decoder still failing after it ends the stream.
 */
#define RESTART_GRACE_US 200000u
#include MEDIACODEC_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static unsigned char inbuf[1 << 20];
static int configure_fails, configures, queued;

const char *AMEDIAFORMAT_KEY_MIME = "mime", *AMEDIAFORMAT_KEY_WIDTH = "width", *AMEDIAFORMAT_KEY_HEIGHT = "height",
        *AMEDIAFORMAT_KEY_FRAME_RATE = "frame-rate", *AMEDIAFORMAT_KEY_MAX_INPUT_SIZE = "max-input-size";

static int codec_object;
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *c, int64_t t) { (void) c; (void) t; return 0; }
uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *c, size_t i, size_t *out) {
    (void) c; (void) i; *out = sizeof(inbuf); return inbuf;
}
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec *c, size_t i, off_t o, size_t s, uint64_t t, uint32_t f) {
    (void) c; (void) i; (void) o; (void) s; (void) t; (void) f; queued++; return AMEDIA_OK;
}
AMediaCodec *AMediaCodec_createCodecByName(const char *n) { (void) n; return (AMediaCodec *) &codec_object; }
AMediaCodec *AMediaCodec_createDecoderByType(const char *n) { (void) n; return (AMediaCodec *) &codec_object; }
media_status_t AMediaCodec_delete(AMediaCodec *c) { (void) c; return AMEDIA_OK; }
media_status_t AMediaCodec_configure(AMediaCodec *c, const AMediaFormat *f, ANativeWindow *w, AMediaCrypto *cr,
                                     uint32_t fl) {
    (void) c; (void) f; (void) w; (void) cr; (void) fl;
    configures++;
    return configure_fails ? AMEDIA_ERROR_UNKNOWN : AMEDIA_OK;
}
media_status_t AMediaCodec_start(AMediaCodec *c) { (void) c; return AMEDIA_OK; }
media_status_t AMediaCodec_stop(AMediaCodec *c) { (void) c; return AMEDIA_OK; }
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *c, AMediaCodecBufferInfo *i, int64_t t) {
    (void) c; (void) i; usleep((useconds_t) (t > 0 ? t : 1000)); return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
}
AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *c) { (void) c; return NULL; }
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec *c, size_t i, bool r) { (void) c; (void) i; (void) r; return AMEDIA_OK; }
AMediaFormat *AMediaFormat_new(void) { return NULL; }
media_status_t AMediaFormat_delete(AMediaFormat *f) { (void) f; return AMEDIA_OK; }
const char *AMediaFormat_toString(AMediaFormat *f) { (void) f; return ""; }
void AMediaFormat_setInt32(AMediaFormat *f, const char *n, int32_t v) { (void) f; (void) n; (void) v; }
void AMediaFormat_setString(AMediaFormat *f, const char *n, const char *v) { (void) f; (void) n; (void) v; }
void AMediaFormat_setBuffer(AMediaFormat *f, const char *n, const void *d, size_t s) { (void) f; (void) n; (void) d; (void) s; }
void ANativeWindow_release(ANativeWindow *w) { (void) w; }
ANativeWindow *ANativeWindow_fromSurface(JNIEnv *e, jobject s) { (void) e; (void) s; return NULL; }
int __android_log_print(int p, const char *t, const char *f, ...) { (void) p; (void) t; (void) f; return 0; }
void *SDL_AndroidGetJNIEnv(void) { return NULL; }
void *SDL_AndroidGetActivity(void) { return NULL; }

static int failures;
static void expect(SS4S_VideoFeedResult got, SS4S_VideoFeedResult want, const char *what) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got %d, want %d\n", what, (int) got, (int) want);
        failures++;
    }
}

static const unsigned char key[] = {0, 0, 0, 1, 0x65, 0x88, 0x84, 0x00, 0x10};
static const unsigned char delta[] = {0, 0, 0, 1, 0x41, 0x9a, 0x00, 0x10};

static SS4S_VideoFeedResult feed_key(SS4S_VideoInstance *in) {
    return Feed(in, key, sizeof(key), SS4S_VIDEO_FEED_DATA_FRAME_START | SS4S_VIDEO_FEED_DATA_FRAME_END |
                                      SS4S_VIDEO_FEED_DATA_KEYFRAME);
}

static SS4S_VideoFeedResult feed_delta(SS4S_VideoInstance *in) {
    return Feed(in, delta, sizeof(delta), SS4S_VIDEO_FEED_DATA_FRAME_START | SS4S_VIDEO_FEED_DATA_FRAME_END);
}

int main(void) {
    SS4S_VideoInstance in;
    memset(&in, 0, sizeof(in));
    pthread_mutex_init(&in.lock, NULL);
    in.codec = SS4S_VIDEO_H264;
    in.mime = "video/avc";
    in.width = 1920;
    in.height = 1080;
    in.fps = 60;
    in.need_keyframe = true;

    /* A running decoder. */
    expect(StartCodecLocked(&in) ? SS4S_VIDEO_FEED_OK : SS4S_VIDEO_FEED_ERROR, SS4S_VIDEO_FEED_OK, "first start");
    expect(feed_key(&in), SS4S_VIDEO_FEED_OK, "keyframe on a running decoder");
    expect(feed_delta(&in), SS4S_VIDEO_FEED_OK, "picture on a running decoder");

    /* The surface goes: the codec failed and is restarted at the next keyframe, and every start fails. */
    in.reconfigure_at_keyframe = true;
    configure_fails = 1;
    expect(feed_delta(&in), SS4S_VIDEO_FEED_OK, "picture before the restart keyframe");
    expect(feed_key(&in), SS4S_VIDEO_FEED_NOT_READY, "failed restart within the grace: dropped, not an error");
    expect(feed_delta(&in), SS4S_VIDEO_FEED_REQUEST_KEYFRAME, "picture while waiting to retry: asks for a keyframe");
    expect(feed_key(&in), SS4S_VIDEO_FEED_NOT_READY, "second failed restart within the grace");

    /* The surface comes back: the next keyframe restarts the decoder and the stream goes on. */
    configure_fails = 0;
    expect(feed_key(&in), SS4S_VIDEO_FEED_OK, "restart once the surface is back");
    expect(feed_delta(&in), SS4S_VIDEO_FEED_OK, "pictures after the restart");
    if (in.restart_failing_since != 0) {
        fprintf(stderr, "FAIL: the failure time was not cleared\n");
        failures++;
    }

    /* A decoder that keeps failing: still an error once the grace is over. */
    in.reconfigure_at_keyframe = true;
    configure_fails = 1;
    expect(feed_key(&in), SS4S_VIDEO_FEED_NOT_READY, "failure within the grace");
    usleep(RESTART_GRACE_US + 50000);
    expect(feed_key(&in), SS4S_VIDEO_FEED_ERROR, "failure after the grace ends the stream");
    expect(feed_delta(&in), SS4S_VIDEO_FEED_ERROR, "pictures after that are errors too");

    /* An HDR change on a healthy decoder restarts at once, as before. */
    configure_fails = 0;
    in.restart_failing_since = 0;
    in.reconfigure_at_keyframe = true;
    const int before = configures;
    expect(feed_key(&in), SS4S_VIDEO_FEED_OK, "HDR restart on a healthy decoder");
    if (configures != before + 1) {
        fprintf(stderr, "FAIL: the HDR restart configured %d times\n", configures - before);
        failures++;
    }

    pthread_mutex_lock(&in.lock);
    StopCodecLocked(&in);
    pthread_mutex_unlock(&in.lock);
    printf("%s: %d configures, %d buffers queued\n", failures ? "FAIL" : "PASS", configures, queued);
    return failures ? 1 : 0;
}

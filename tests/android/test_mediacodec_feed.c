/*
 * Host unit test of the Android MediaCodec module's feeding (third_party/ss4s/modules/android/mediacodec.c),
 * compiled into this test with fakes for the NDK calls Feed() makes (ndk-stubs/ for the declarations).
 *
 * Given an Annex B file, it feeds every access unit as the stream does and checks what reaches the decoder:
 * the parameter sets as ONE codec-config buffer containing nothing else (sent again only when they change),
 * then pictures that begin with a slice (or its SEI/AUD). With DUMP set, the exact bytes the decoder would get
 * are written there, so run_mediacodec_test.sh can decode them with FFmpeg in strict mode.
 *
 *   test_mediacodec_feed <file> h264|hevc
 */
#include MEDIACODEC_SOURCE

#include <stdio.h>
#include <stdlib.h>

static unsigned char inbuf[16 << 20];
static int is_hevc;
static int queued, config_buffers, config_clean = 1, pictures_clean = 1;
static FILE *dump;

const char *AMEDIAFORMAT_KEY_MIME = "mime", *AMEDIAFORMAT_KEY_WIDTH = "width", *AMEDIAFORMAT_KEY_HEIGHT = "height",
        *AMEDIAFORMAT_KEY_FRAME_RATE = "frame-rate", *AMEDIAFORMAT_KEY_MAX_INPUT_SIZE = "max-input-size";

static unsigned nal_type(const unsigned char *nal) {
    return is_hevc ? (nal[0] >> 1) & 0x3fu : nal[0] & 0x1fu;
}

ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *c, int64_t t) {
    (void) c;
    (void) t;
    return 0;
}

uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *c, size_t i, size_t *out) {
    (void) c;
    (void) i;
    *out = sizeof(inbuf);
    return inbuf;
}

media_status_t AMediaCodec_queueInputBuffer(AMediaCodec *c, size_t i, off_t o, size_t size, uint64_t t,
                                            uint32_t flags) {
    (void) c;
    (void) i;
    (void) o;
    (void) t;
    queued++;
    if (dump != NULL) {
        fwrite(inbuf, 1, size, dump);
    }
    bool first = true;
    for (size_t p = 0; p + 3 < size; ++p) {
        if (inbuf[p] != 0 || inbuf[p + 1] != 0 || inbuf[p + 2] != 1) {
            continue;
        }
        const unsigned type = nal_type(inbuf + p + 3);
        if (flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
            if (is_hevc ? !(type >= 32 && type <= 34) : !(type == 7 || type == 8)) {
                config_clean = 0;
            }
        } else if (first) {
            const bool ok = is_hevc ? (type <= 31 || type == 35 || type == 39) : (type == 1 || type == 5 || type == 6 || type == 9);
            if (!ok) {
                pictures_clean = 0;
            }
        }
        first = false;
        p += 2;
    }
    if (flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
        config_buffers++;
    }
    return AMEDIA_OK;
}

/* Not reached by Feed(); defined so the module links. */
AMediaCodec *AMediaCodec_createCodecByName(const char *n) { (void) n; return NULL; }
AMediaCodec *AMediaCodec_createDecoderByType(const char *n) { (void) n; return NULL; }
media_status_t AMediaCodec_delete(AMediaCodec *c) { (void) c; return AMEDIA_OK; }
media_status_t AMediaCodec_configure(AMediaCodec *c, const AMediaFormat *f, ANativeWindow *w, AMediaCrypto *cr,
                                     uint32_t fl) { (void) c; (void) f; (void) w; (void) cr; (void) fl; return AMEDIA_OK; }
media_status_t AMediaCodec_start(AMediaCodec *c) { (void) c; return AMEDIA_OK; }
media_status_t AMediaCodec_stop(AMediaCodec *c) { (void) c; return AMEDIA_OK; }
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *c, AMediaCodecBufferInfo *i, int64_t t) { (void) c; (void) i; (void) t; return -1; }
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

static bool starts_access_unit(const unsigned char *nal, size_t len, bool *vcl, bool *irap) {
    const unsigned type = nal_type(nal);
    if (is_hevc) {
        *vcl = type <= 31;
        *irap = type >= 16 && type <= 23;
        return *vcl ? len > 2 && (nal[2] & 0x80u) : (type >= 32 && type <= 35) || type == 39;
    }
    *vcl = type == 1 || type == 5;
    *irap = type == 5;
    return *vcl ? len > 1 && (nal[1] & 0x80u) : type >= 6 && type <= 9;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <file> h264|hevc\n", argv[0]);
        return 2;
    }
    is_hevc = strcmp(argv[2], "hevc") == 0;
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    const size_t n = (size_t) ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc(n);
    if (d == NULL || fread(d, 1, n, f) != n) {
        return 1;
    }
    fclose(f);
    const char *dump_path = getenv("DUMP");
    dump = dump_path != NULL ? fopen(dump_path, "wb") : NULL;

    SS4S_VideoInstance instance;
    memset(&instance, 0, sizeof(instance));
    pthread_mutex_init(&instance.lock, NULL);
    instance.codec = is_hevc ? SS4S_VIDEO_H265 : SS4S_VIDEO_H264;
    instance.mc = (AMediaCodec *) &instance;   /* never dereferenced by the fakes */
    instance.need_keyframe = true;

    int fed = 0, ok = 0;
    size_t start = 0;
    bool seen_vcl = false, au_irap = false;
    for (size_t p = 0; p <= n; ++p) {
        size_t sc = 0;
        if (p + 3 < n && d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 1) sc = 3;
        else if (p + 4 < n && d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 0 && d[p + 3] == 1) sc = 4;
        const bool end = p == n;
        bool vcl = false, irap = false, boundary = end;
        if (sc != 0) {
            boundary = starts_access_unit(d + p + sc, n - p - sc, &vcl, &irap);
        }
        if (boundary && seen_vcl) {
            const SS4S_VideoFeedFlags flags = SS4S_VIDEO_FEED_DATA_FRAME_START | SS4S_VIDEO_FEED_DATA_FRAME_END |
                                              (au_irap ? SS4S_VIDEO_FEED_DATA_KEYFRAME : 0);
            ok += Feed(&instance, d + start, p - start, flags) == SS4S_VIDEO_FEED_OK;
            fed++;
            start = p;
            seen_vcl = false;
            au_irap = false;
        }
        if (vcl) {
            seen_vcl = true;
            au_irap = au_irap || irap;
        }
        if (sc != 0) {
            p += sc - 1;
        }
    }
    if (dump != NULL) {
        fclose(dump);
    }
    const bool pass = fed > 0 && ok == fed && config_buffers >= 1 && config_clean && pictures_clean &&
                      queued == fed + config_buffers;
    printf("%s %s: %d access units, %d accepted; %d decoder buffers, %d codec-config (parameter sets only: %s); "
           "pictures start with a slice: %s -> %s\n", argv[1], argv[2], fed, ok, queued, config_buffers,
           config_clean ? "yes" : "NO", pictures_clean ? "yes" : "NO", pass ? "PASS" : "FAIL");
    free(d);
    return pass ? 0 : 1;
}

#pragma once
/* TEST-ONLY declarations of the NDK API used by modules/android/mediacodec.c, so its host unit test
 * (tests/android) can compile it on a desktop. Signatures follow the Android NDK headers. */
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <android/native_window.h>
#include <media/NdkMediaError.h>
#include <media/NdkMediaFormat.h>
typedef struct AMediaCodec AMediaCodec;
typedef struct AMediaCrypto AMediaCrypto;
struct AMediaCodecBufferInfo { int32_t offset; int32_t size; int64_t presentationTimeUs; uint32_t flags; };
typedef struct AMediaCodecBufferInfo AMediaCodecBufferInfo;
enum { AMEDIACODEC_BUFFER_FLAG_KEY_FRAME = 1, AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG = 2, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM = 4,
       AMEDIACODEC_CONFIGURE_FLAG_ENCODE = 1, AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED = -3,
       AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED = -2, AMEDIACODEC_INFO_TRY_AGAIN_LATER = -1 };
AMediaCodec *AMediaCodec_createCodecByName(const char *name);
AMediaCodec *AMediaCodec_createDecoderByType(const char *mime_type);
media_status_t AMediaCodec_delete(AMediaCodec *);
media_status_t AMediaCodec_configure(AMediaCodec *, const AMediaFormat *format, ANativeWindow *surface, AMediaCrypto *crypto, uint32_t flags);
media_status_t AMediaCodec_start(AMediaCodec *);
media_status_t AMediaCodec_stop(AMediaCodec *);
media_status_t AMediaCodec_flush(AMediaCodec *);
uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *, size_t idx, size_t *out_size);
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *, int64_t timeoutUs);
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec *, size_t idx, off_t offset, size_t size, uint64_t time, uint32_t flags);
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *, AMediaCodecBufferInfo *info, int64_t timeoutUs);
AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *);
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec *, size_t idx, bool render);
media_status_t AMediaCodec_setParameters(AMediaCodec *mData, const AMediaFormat *params);

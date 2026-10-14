#pragma once
/* TEST-ONLY declarations of the NDK API used by modules/android/mediacodec.c, so its host unit test
 * (tests/android) can compile it on a desktop. Signatures follow the Android NDK headers. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <media/NdkMediaError.h>
typedef struct AMediaFormat AMediaFormat;
AMediaFormat *AMediaFormat_new(void);
media_status_t AMediaFormat_delete(AMediaFormat *);
const char *AMediaFormat_toString(AMediaFormat *);
bool AMediaFormat_getInt32(AMediaFormat *, const char *name, int32_t *out);
void AMediaFormat_setInt32(AMediaFormat *, const char *name, int32_t value);
void AMediaFormat_setInt64(AMediaFormat *, const char *name, int64_t value);
void AMediaFormat_setString(AMediaFormat *, const char *name, const char *value);
void AMediaFormat_setBuffer(AMediaFormat *, const char *name, const void *data, size_t size);
extern const char *AMEDIAFORMAT_KEY_MIME;
extern const char *AMEDIAFORMAT_KEY_WIDTH;
extern const char *AMEDIAFORMAT_KEY_HEIGHT;
extern const char *AMEDIAFORMAT_KEY_FRAME_RATE;
extern const char *AMEDIAFORMAT_KEY_MAX_INPUT_SIZE;

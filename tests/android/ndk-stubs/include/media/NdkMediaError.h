#pragma once
/* TEST-ONLY declarations of the NDK API used by modules/android/mediacodec.c, so its host unit test
 * (tests/android) can compile it on a desktop. Signatures follow the Android NDK headers. */
typedef enum { AMEDIA_OK = 0, AMEDIA_ERROR_BASE = -10000, AMEDIA_ERROR_UNKNOWN = AMEDIA_ERROR_BASE } media_status_t;

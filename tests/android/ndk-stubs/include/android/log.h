#pragma once
/* TEST-ONLY declarations of the NDK API used by modules/android/mediacodec.c, so its host unit test
 * (tests/android) can compile it on a desktop. Signatures follow the Android NDK headers. */
typedef enum android_LogPriority { ANDROID_LOG_UNKNOWN = 0, ANDROID_LOG_DEFAULT, ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG,
  ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR, ANDROID_LOG_FATAL, ANDROID_LOG_SILENT } android_LogPriority;
int __android_log_print(int prio, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#pragma once
/* TEST-ONLY declarations of the NDK API used by modules/android/mediacodec.c, so its host unit test
 * (tests/android) can compile it on a desktop. Signatures follow the Android NDK headers. */
#include <jni.h>
#include <android/native_window.h>
ANativeWindow *ANativeWindow_fromSurface(JNIEnv *env, jobject surface);

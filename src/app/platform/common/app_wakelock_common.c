#include "app.h"
#if TARGET_ANDROID
#include "platform/android/android_platform.h"
#endif

void app_set_keep_awake(app_t *app, bool awake) {
    if (awake) {
        SDL_DisableScreenSaver();
    } else {
        SDL_EnableScreenSaver();
    }
#if TARGET_ANDROID
    /* A stream's Wi-Fi stays out of power saving too. */
    android_stream_wifi_lock(awake);
#endif
}

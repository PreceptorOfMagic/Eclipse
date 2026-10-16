/* The refresh rate a stream asks its host for on Android (stream/session.c). JNI-free, so tests/android
 * checks it on the host. The display mode itself is chosen by the activity (DisplayModes.java, same rules). */
#include "android_platform.h"

#include <stdlib.h>

/* @p rate shows a stream at @p base evenly: the same rate, or a whole multiple of it. Rates are hundredths of
 * a Hz; one hundredth per multiple is rounding, as in DisplayModes.java. */
static bool shows_evenly(int rate, int base) {
    if (base <= 0 || rate <= 0) {
        return false;
    }
    const int n = (rate + base / 2) / base;
    return n >= 1 && abs(rate - n * base) <= n;
}

int android_stream_refresh_x100(int fps, int requested_x100, const int *rates_x100, int count) {
    if (requested_x100 > 0) {
        return requested_x100;   /* an exact rate the user chose (Settings -> Basic -> Custom FPS) */
    }
    if (fps <= 0 || rates_x100 == NULL || count <= 0) {
        return 0;
    }
    const int whole = fps * 100;
    for (int i = 0; i < count; ++i) {
        if (shows_evenly(rates_x100[i], whole)) {
            return 0;   /* the TV shows the frame rate as it is */
        }
    }
    /* Only the NTSC rate (60 -> 59.94): ask the host for that, as the webOS build always does for its panel,
     * so the game is not paced a thousandth faster than the screen and a picture is not dropped every ~17 s. */
    const int ntsc = (fps * 100000 + 500) / 1001;
    for (int i = 0; i < count; ++i) {
        if (shows_evenly(rates_x100[i], ntsc)) {
            return ntsc;
        }
    }
    return 0;
}

int android_parse_rates(const char *text, int *rates_x100, int max) {
    int count = 0;
    for (const char *p = text; p != NULL && *p != '\0' && count < max;) {
        char *end = NULL;
        const long value = strtol(p, &end, 10);
        if (end == p) {
            break;
        }
        if (value > 0 && value < 100000) {
            rates_x100[count++] = (int) value;
        }
        if (*end != ',') {
            break;
        }
        p = end + 1;
    }
    return count;
}

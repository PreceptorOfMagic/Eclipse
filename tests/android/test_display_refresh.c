/* The refresh rate an Android stream asks its host for (platform/android/display_android.c). */
#include "android_platform.h"

#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
#define N(a) ((int) (sizeof(a) / sizeof((a)[0])))

int main(void) {
    const int tv[] = {2398, 2400, 2500, 2997, 3000, 5000, 5994, 6000};   /* a 4K TV at 2160p */
    CHECK(android_stream_refresh_x100(60, 0, tv, N(tv)) == 0);             /* 60 Hz exists: plain 60 FPS */
    CHECK(android_stream_refresh_x100(24, 0, tv, N(tv)) == 0);
    CHECK(android_stream_refresh_x100(120, 0, tv, N(tv)) == 0);            /* nothing shows 120: plain 120 */

    const int ntsc_only[] = {2397, 2997, 5994};                              /* a TV that offers only NTSC rates */
    CHECK(android_stream_refresh_x100(60, 0, ntsc_only, N(ntsc_only)) == 5994);   /* as the webOS build asks */
    CHECK(android_stream_refresh_x100(30, 0, ntsc_only, N(ntsc_only)) == 2997);
    CHECK(android_stream_refresh_x100(24, 0, ntsc_only, N(ntsc_only)) == 2398);   /* 23.976, its 2397 within rounding */

    const int ntsc120[] = {5994, 11988};
    CHECK(android_stream_refresh_x100(120, 0, ntsc120, N(ntsc120)) == 11988);
    CHECK(android_stream_refresh_x100(60, 0, ntsc120, N(ntsc120)) == 5994);
    CHECK(android_stream_refresh_x100(30, 0, ntsc120, N(ntsc120)) == 2997);     /* 59.94 = 2 x 29.97 */

    const int gaming[] = {6000, 12000};
    CHECK(android_stream_refresh_x100(30, 0, gaming, N(gaming)) == 0);          /* 60 = 2 x 30 */
    CHECK(android_stream_refresh_x100(40, 0, gaming, N(gaming)) == 0);          /* 120 = 3 x 40 */

    /* An exact rate the user chose is kept as it is; nothing known about the display changes nothing. */
    CHECK(android_stream_refresh_x100(60, 5994, gaming, N(gaming)) == 5994);
    CHECK(android_stream_refresh_x100(120, 11988, tv, N(tv)) == 11988);
    CHECK(android_stream_refresh_x100(60, 0, NULL, 0) == 0);
    CHECK(android_stream_refresh_x100(0, 0, tv, N(tv)) == 0);

    /* The peer's copy of the list. */
    int rates[4];
    CHECK(android_parse_rates("5994,6000,11988", rates, 4) == 3 && rates[0] == 5994 && rates[2] == 11988);
    CHECK(android_parse_rates("5994,6000,11988,12000,24000", rates, 4) == 4);
    CHECK(android_parse_rates("", rates, 4) == 0);
    CHECK(android_parse_rates(NULL, rates, 4) == 0);
    CHECK(android_parse_rates("6000,junk", rates, 4) == 1 && rates[0] == 6000);
    CHECK(android_parse_rates("-5,6000", rates, 4) == 1 && rates[0] == 6000);

    if (failures == 0) printf("display refresh: OK\n");
    return failures == 0 ? 0 : 1;
}

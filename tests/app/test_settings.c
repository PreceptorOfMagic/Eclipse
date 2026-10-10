#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "app_settings.h"
#include "uuidstr.h"
#include "util/path.h"

#ifndef FIXTURES_PATH_PREFIX
#define FIXTURES_PATH_PREFIX "./"
#endif

app_settings_t settings;

void setUp() {
    char *dir = malloc(128);
    uuidstr_t uuid;
    uuidstr_random(&uuid);
    snprintf(dir, 128, "/tmp/moonlight-%s", (char *) &uuid);
    settings_initialize(&settings, dir);
}

void tearDown() {
    settings_clear(&settings);
}

static bool file_contains(const char *path, const char *needle) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return false;
    }
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strstr(line, needle) != NULL) {
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

void testReadINI() {
    char *ini_backup = settings.ini_path;
    settings.ini_path = strdup(FIXTURES_PATH_PREFIX "settings_read.ini");
    TEST_ASSERT_NOT_NULL(settings.ini_path);
    /* The fixture still carries the removed hid_passthrough keys (2026-10-08): an old
     * moonlight.ini must keep reading cleanly with them present. */
    TEST_ASSERT_TRUE(settings_read(&settings));
    TEST_ASSERT_EQUAL_INT(45000, settings.stream.bitrate);
    TEST_ASSERT_FALSE(settings.stream_bitrate_defaulted);
    free(settings.ini_path);
    settings.ini_path = ini_backup;
}

void testWriteINI() {
    char *ini_backup = settings.ini_path;
    settings.ini_path = strdup("settings_write_tmp.ini");
    TEST_ASSERT_NOT_NULL(settings.ini_path);

    TEST_ASSERT_TRUE(settings.stream_bitrate_defaulted);
    TEST_ASSERT_TRUE(settings_save(&settings));
    TEST_ASSERT_FALSE(file_contains(settings.ini_path, "bitrate ="));

    settings.stream.bitrate = 45000;
    settings.stream_bitrate_defaulted = false;
    TEST_ASSERT_TRUE(settings_save(&settings));
    TEST_ASSERT_TRUE(file_contains(settings.ini_path, "bitrate = 45000"));

    remove(settings.ini_path);
    free(settings.ini_path);
    settings.ini_path = ini_backup;
}

void testDefaultBitrateCurve() {
    TEST_ASSERT_EQUAL_INT(10000, settings_optimal_bitrate(NULL, 1280, 720, 60));
    TEST_ASSERT_EQUAL_INT(20000, settings_optimal_bitrate(NULL, 1920, 1080, 60));
    TEST_ASSERT_EQUAL_INT(40000, settings_optimal_bitrate(NULL, 2560, 1440, 60));
    TEST_ASSERT_EQUAL_INT(80000, settings_optimal_bitrate(NULL, 3840, 2160, 60));
    TEST_ASSERT_EQUAL_INT(113000, settings_optimal_bitrate(NULL, 3840, 2160, 120));
}

void testDefaultBitrateOwnership() {
    settings.stream.width = 3840;
    settings.stream.height = 2160;
    settings.stream.fps = 60;
    settings_refresh_auto_bitrate(&settings, NULL);
    TEST_ASSERT_EQUAL_INT(80000, settings.stream.bitrate);

    settings.stream.bitrate = 45000;
    settings.stream_bitrate_defaulted = false;
    settings.stream.width = 2560;
    settings.stream.height = 1440;
    settings_refresh_auto_bitrate(&settings, NULL);
    TEST_ASSERT_EQUAL_INT(45000, settings.stream.bitrate);
}

void testAutomaticBitrateLoadsAfterGeometry() {
    static const char path[] = "settings_auto_tmp.ini";
    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_TRUE(fputs("[streaming]\nwidth = 2560\nheight = 1440\nnet_fps = 60\n", fp) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));

    char *ini_backup = settings.ini_path;
    settings.ini_path = strdup(path);
    TEST_ASSERT_NOT_NULL(settings.ini_path);
    TEST_ASSERT_TRUE(settings_read(&settings));
    TEST_ASSERT_TRUE(settings.stream_bitrate_defaulted);
    TEST_ASSERT_EQUAL_INT(40000, settings.stream.bitrate);

    remove(path);
    free(settings.ini_path);
    settings.ini_path = ini_backup;
}

void testCoopPadBothReadsAsPrimary() {
    /* "Both" (2) is no longer offered for a controller; a conf saved while it was must not keep
     * sending a pad to both hosts. Anything outside the two routes reads back as PRIMARY. */
    static const char path[] = "settings_coop_pad_tmp.ini";
    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_TRUE(fputs("[input]\ncoop_pad1_pane = 2\ncoop_pad2_pane = 1\n"
                           "coop_pad3_pane = 0\ncoop_pad4_pane = 7\n", fp) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));

    char *ini_backup = settings.ini_path;
    settings.ini_path = strdup(path);
    TEST_ASSERT_NOT_NULL(settings.ini_path);
    TEST_ASSERT_TRUE(settings_read(&settings));
    TEST_ASSERT_EQUAL_INT(COOP_PAD_PRIMARY, settings.coop_pad_pane[0]);
    TEST_ASSERT_EQUAL_INT(COOP_PAD_SECONDARY, settings.coop_pad_pane[1]);
    TEST_ASSERT_EQUAL_INT(COOP_PAD_PRIMARY, settings.coop_pad_pane[2]);
    TEST_ASSERT_EQUAL_INT(COOP_PAD_PRIMARY, settings.coop_pad_pane[3]);

    remove(path);
    free(settings.ini_path);
    settings.ini_path = ini_backup;
}

void testWindowMaximizedRoundTrip() {
    /* issue #6: a maximized window is saved as a FLAG, not as its maximized rectangle. */
    static const char path[] = "settings_window_tmp.ini";
    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_TRUE(fputs("[window]\nx = 100\ny = 80\nwidth = 1280\nheight = 720\nmaximized = true\n", fp) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));

    char *ini_backup = settings.ini_path;
    settings.ini_path = strdup(path);
    TEST_ASSERT_NOT_NULL(settings.ini_path);
    TEST_ASSERT_TRUE(settings_read(&settings));
    TEST_ASSERT_TRUE(settings.window_state.maximized);
    TEST_ASSERT_EQUAL_INT(1280, settings.window_state.w);

    settings.window_state.maximized = false;
    TEST_ASSERT_TRUE(settings_save(&settings));
    TEST_ASSERT_TRUE(file_contains(settings.ini_path, "maximized = false"));
    settings.window_state.maximized = true;
    TEST_ASSERT_TRUE(settings_save(&settings));
    TEST_ASSERT_TRUE(file_contains(settings.ini_path, "maximized = true"));

    remove(path);
    free(settings.ini_path);
    settings.ini_path = ini_backup;
}

void testUnreadableOrMalformedSettingsAreNeverOverwritten() {
    static const char path[] = "settings_malformed_tmp.ini";
    FILE *fp = path_fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    static const char malformed[] = "[streaming\nbitrate = 12345\n";
    TEST_ASSERT_EQUAL_UINT(sizeof(malformed) - 1,
                           fwrite(malformed, 1, sizeof(malformed) - 1, fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));

    char *ini_backup = settings.ini_path;
    settings.ini_path = strdup(path);
    TEST_ASSERT_NOT_NULL(settings.ini_path);
    TEST_ASSERT_FALSE(settings_read(&settings));
    TEST_ASSERT_FALSE(settings.settings_save_allowed);
    TEST_ASSERT_FALSE(settings_save(&settings));
    TEST_ASSERT_TRUE(file_contains(path, "bitrate = 12345"));

    TEST_ASSERT_EQUAL_INT(0, path_file_remove(path));
    free(settings.ini_path);
    settings.ini_path = ini_backup;
}

void testSettingsUtf8PathRoundTrip() {
    const char *directory = "settings-\xE9\x9B\xAA-\xF0\x9F\x8C\x99";
    TEST_ASSERT_EQUAL_INT(0, path_dir_ensure(directory));
    char *path = path_join(directory, "moonlight.ini");
    TEST_ASSERT_NOT_NULL(path);

    char *ini_backup = settings.ini_path;
    settings.ini_path = path;
    settings.stream.bitrate = 42000;
    settings.stream_bitrate_defaulted = false;
    TEST_ASSERT_TRUE(settings_save(&settings));

    settings.stream.bitrate = 1;
    settings.stream_bitrate_defaulted = true;
    TEST_ASSERT_TRUE(settings_read(&settings));
    TEST_ASSERT_EQUAL_INT(42000, settings.stream.bitrate);
    TEST_ASSERT_FALSE(settings.stream_bitrate_defaulted);

    TEST_ASSERT_EQUAL_INT(0, path_file_remove(path));
    free(path);
    settings.ini_path = ini_backup;
}

void testSyncRefreshRate() {
#if defined(TARGET_WEBOS)
    settings.stream.fps = 120;
    settings.client_refresh_rate_x100 = 11988;
    settings_sync_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(120, settings.stream.fps);
    TEST_ASSERT_EQUAL_INT(11988, settings.client_refresh_rate_x100);
    settings.client_refresh_rate_x100 = 0;
    settings.stream.fps = 60;
    settings_sync_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(60, settings.stream.fps);
    TEST_ASSERT_EQUAL_INT(5994, settings.client_refresh_rate_x100);
#else
    settings.stream.fps = 119;
    settings.client_refresh_rate_x100 = 11988;
    settings_sync_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(120, settings.stream.fps);
#endif
}

void testNtscRefreshRateMapping() {
    TEST_ASSERT_EQUAL_INT(2997, settings_ntsc_refresh_rate_x100_for_fps(30));
    TEST_ASSERT_EQUAL_INT(5994, settings_ntsc_refresh_rate_x100_for_fps(60));
    TEST_ASSERT_EQUAL_INT(11988, settings_ntsc_refresh_rate_x100_for_fps(120));
    TEST_ASSERT_EQUAL_INT(23976, settings_ntsc_refresh_rate_x100_for_fps(240));
    TEST_ASSERT_EQUAL_INT(0, settings_ntsc_refresh_rate_x100_for_fps(90));
    TEST_ASSERT_EQUAL_INT(0, settings_ntsc_refresh_rate_x100_for_fps(144));
}

void testReconcileRefreshRate() {
    settings.stream.fps = 144;
    settings.client_refresh_rate_x100 = 11988;
    settings_reconcile_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(0, settings.client_refresh_rate_x100);
    TEST_ASSERT_EQUAL_INT(144, settings.stream.fps);

#if defined(TARGET_WEBOS)
    settings.stream.fps = 120;
    settings.client_refresh_rate_x100 = 0;
    settings_reconcile_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(11988, settings.client_refresh_rate_x100);

    settings.stream.fps = 60;
    settings_reconcile_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(5994, settings.client_refresh_rate_x100);
#endif
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(testReadINI);
    RUN_TEST(testWriteINI);
    RUN_TEST(testDefaultBitrateCurve);
    RUN_TEST(testDefaultBitrateOwnership);
    RUN_TEST(testAutomaticBitrateLoadsAfterGeometry);
    RUN_TEST(testCoopPadBothReadsAsPrimary);
    RUN_TEST(testWindowMaximizedRoundTrip);
    RUN_TEST(testUnreadableOrMalformedSettingsAreNeverOverwritten);
    RUN_TEST(testSettingsUtf8PathRoundTrip);
    RUN_TEST(testSyncRefreshRate);
    RUN_TEST(testReconcileRefreshRate);
    RUN_TEST(testNtscRefreshRateMapping);
    return UNITY_END();
}

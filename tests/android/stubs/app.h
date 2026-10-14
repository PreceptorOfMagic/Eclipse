#pragma once
/* TEST-ONLY stand-in for src/app/app.h (tests/android/test_paths_launch.c): app_android.c needs an opaque
 * app_t, the settings type and app_configuration -- not the UI, SS4S and LVGL state app.h brings along. */
#include "app_settings.h"

typedef struct app_t app_t;

extern PCONFIGURATION app_configuration;

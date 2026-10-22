#pragma once

#include "app_input.h"

typedef struct executor_t executor_t;
typedef struct app_settings_t app_settings_t;

void app_input_init_gamepad_mapping(app_input_t *input, executor_t *executor, const app_settings_t *settings);

void app_input_deinit_gamepad_mapping(app_input_t *input);

void app_input_copy_initial_gamepad_mapping(const app_settings_t *settings);

void app_input_reload_gamepad_mapping(app_input_t *input);

#if TARGET_ANDROID
/** Android: keep the controller database to choose from per controller (see the .c).
 *  @return its Android lines */
int app_input_load_gamepad_db(const char *path);

/** Android: choose the mapping for the controller at @p device_index by how it fits (see the .c). */
void app_input_fit_gamepad_mapping(int device_index);
#endif
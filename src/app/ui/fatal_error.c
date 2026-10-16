#include "fatal_error.h"
#include "app.h"

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <SDL_thread.h>
#if defined(TARGET_ANDROID)
#include <unistd.h>
#endif

#include "lvgl.h"

#include "util/bus.h"
#include "logging.h"

static void fatal_error_popup(void *data);

static void fatal_error_quit(lv_event_t *ev);

typedef struct fatal_error_data_t {
    char title[32];
    char message[1024];
} fatal_error_data_t;

void app_fatal_error(const char *title, const char *fmt, ...) {
    fatal_error_data_t *data = malloc(sizeof(fatal_error_data_t));
    snprintf(data->title, 32, "%s", title);
    va_list args;
    va_start(args, fmt);
    vsnprintf(data->message, 1024, fmt, args);
    va_end(args);
    /* In the log too: the kept diagnostic logs are what a user sends, and the dialog is gone once they quit. */
    commons_log_fatal("APP", "Fatal error: %s: %s", data->title, data->message);
    app_bus_post(global, fatal_error_popup, data);
}


static void fatal_error_popup(void *data) {
    fatal_error_data_t *error_data = data;
    static const char *btn_txts[] = {"Quit", ""};
    lv_obj_t *msgbox = lv_msgbox_create(NULL, error_data->title, error_data->message, btn_txts, false);
    lv_obj_add_event_cb(msgbox, fatal_error_quit, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(msgbox, LV_OBJ_FLAG_USER_4);
    lv_obj_center(msgbox);
}

static void fatal_error_quit(lv_event_t *ev) {
#if defined(TARGET_ANDROID)
    /* As main.c ends the process: exit() would run Android's own libraries' destructors under their render threads. */
    fflush(NULL);
    _exit(1);
#else
    exit(1);
#endif
}
#include "lvgl/lv_sdl_drv_input.h"

#include <SDL.h>

#include "app.h"
#include "stream/session.h"
#include "stream/session_events.h"
#include "ui/root.h"
#include "ui/ui_scroll.h"

static void sdl_input_read(lv_indev_drv_t *drv, lv_indev_data_t *data);

#ifndef TARGET_WEBOS
static void wheel_scroll(app_ui_input_t *input, const SDL_MouseWheelEvent *wheel);
#endif

int lv_sdl_init_wheel(lv_indev_drv_t *drv, app_ui_input_t *input) {
    lv_indev_drv_init(drv);
    drv->user_data = input;
    drv->type = LV_INDEV_TYPE_ENCODER;
    drv->read_cb = sdl_input_read;

    return 0;
}

static void sdl_input_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    app_ui_input_t *input = drv->user_data;
    SDL_Event e;
    data->state = LV_INDEV_STATE_RELEASED;
    data->enc_diff = 0;
    if (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_MOUSEWHEEL, SDL_MOUSEWHEEL)) {
        app_t *app = input->ui->app;
        if (app->session != NULL && !ui_should_block_input()) {
            session_handle_input_event(app->session, &e);
        } else {
#ifdef TARGET_WEBOS
            /* The magic remote's wheel walks the focus, one item per notch: that is how a
             * TV list is navigated, and the TV is unchanged. */
            if (e.wheel.y != 0) {
                /* Encoder scroll only: never set PRESSED — that triggers LVGL "encoder button" clicks. */
                data->enc_diff = (e.wheel.y > 0) ? -1 : 1;
            }
#else
            wheel_scroll(input, &e.wheel);
#endif
        }
    }
    data->continue_reading = false;
}

#ifndef TARGET_WEBOS
/**
 * ON A DESKTOP THE WHEEL SCROLLS, IN SMALL STEPS, WHATEVER IS UNDER THE POINTER. As an
 * encoder it moved the focus one item per notch and snapped the view to it -- the d-pad's
 * behaviour, and the opposite of what a mouse wheel does anywhere else. Focus is left where
 * it is; the d-pad carries on from there.
 */
static void wheel_scroll(app_ui_input_t *input, const SDL_MouseWheelEvent *wheel) {
#if SDL_VERSION_ATLEAST(2, 0, 18)
    /* Fractional on a precision touchpad, so a two-finger scroll moves smoothly. */
    float x = wheel->preciseX, y = wheel->preciseY;
#else
    float x = (float) wheel->x, y = (float) wheel->y;
#endif
    if (wheel->direction == SDL_MOUSEWHEEL_FLIPPED) {
        x = -x;
        y = -y;
    }
    /* Shift+wheel scrolls sideways, as it does in every Windows application. */
    if (x == 0 && (SDL_GetModState() & KMOD_SHIFT)) {
        x = -y;
        y = 0;
    }
    lv_point_t point;
#if SDL_VERSION_ATLEAST(2, 26, 0)
    point.x = (lv_coord_t) wheel->mouseX;
    point.y = (lv_coord_t) wheel->mouseY;
    (void) input;
#else
    lv_indev_get_point(input->pointer.indev, &point);
#endif
    ui_scroll_wheel(&point, x, y);
}
#endif

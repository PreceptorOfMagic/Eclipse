#include "lvgl/lv_sdl_drv_input.h"

#include "app.h"

#include <SDL.h>
#include "ui/root.h"
#include "ui/ui_scroll.h"
#include "logging.h"

#include "lvgl.h"
#include "stream/session.h"
#include "stream/session_events.h"

static void indev_pointer_read(lv_indev_drv_t *drv, lv_indev_data_t *data);

int lv_sdl_init_pointer(lv_indev_drv_t *drv, app_ui_input_t *input) {
    lv_indev_drv_init(drv);
    drv->user_data = input;
    drv->type = LV_INDEV_TYPE_POINTER;
    drv->read_cb = indev_pointer_read;
    return 0;
}

static void indev_pointer_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    app_ui_input_t *input = drv->user_data;
    app_t *app = input->ui->app;
    SDL_Event e;
    data->continue_reading = SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_MOUSEMOTION, SDL_MOUSEBUTTONUP) > 0;
    static lv_point_t point = {0, 0};
    static lv_indev_state_t state = LV_INDEV_STATE_RELEASED;
    if (!data->continue_reading) {
        data->point = point;
        data->state = state;
        return;
    }
    if (e.type == SDL_MOUSEMOTION) {
        if (app->session != NULL) {
            session_handle_input_event(app->session, &e);
        }
        state = data->state = e.motion.state;
        point = data->point = (lv_point_t) {.x = e.motion.x, .y = e.motion.y};
        /* MOVING THE CURSOR IS USING THE POINTER. The mode changed only on a BUTTON
         * event, so nothing highlighted under the magic remote until you had clicked
         * something first -- and a highlight you have to discover by clicking might as
         * well not exist. The remote is held in the air, though, and it keeps sending
         * small movements, so a few pixels of travel are required before the pointer
         * takes the UI back: a tremor must not steal the highlight from the d-pad
         * mid-navigation. Both stay live -- whichever was last USED owns the highlight. */
        static lv_point_t wake_anchor = {0, 0};
        if (app_ui_get_input_mode(input) & UI_INPUT_MODE_POINTER_FLAG) {
            wake_anchor = point;
        } else if (LV_ABS(point.x - wake_anchor.x) + LV_ABS(point.y - wake_anchor.y) >= LV_DPX(6)) {
            wake_anchor = point;
            ui_set_input_mode(input, UI_INPUT_MODE_MOUSE);
        }
        if (ui_scroll_bar_dragging()) {
            /* The drag is ours: LVGL sees the pointer move with no button down. A move with
             * the button already up means its release never reached us (focus lost mid-drag):
             * end the drag there, or every later move would keep scrolling the bar. */
            if (e.motion.state & SDL_BUTTON_LMASK) {
                ui_scroll_bar_move(&point);
            } else {
                ui_scroll_bar_release();
            }
            state = data->state = LV_INDEV_STATE_RELEASED;
        }
    } else {
        if (app->session != NULL) {
            session_handle_input_event(app->session, &e);
        }
        /* A SCROLLBAR PRESS NEVER REACHES LVGL. LVGL cannot drag a bar; handed the press it
         * would drag the CONTENT the other way, then click whatever sits under the bar on
         * release. So a left press on a visible bar is taken here, and LVGL is told only
         * that the pointer is up. Not while streaming: the mouse belongs to the host then. */
        if (app->session == NULL && e.button.button == SDL_BUTTON_LEFT) {
            const lv_point_t at = {.x = (lv_coord_t) e.button.x, .y = (lv_coord_t) e.button.y};
            const bool took = e.type == SDL_MOUSEBUTTONDOWN ? ui_scroll_bar_press(&at)
                                                              : ui_scroll_bar_dragging();
            if (took) {
                if (e.type == SDL_MOUSEBUTTONUP) {
                    ui_scroll_bar_release();
                }
                state = data->state = LV_INDEV_STATE_RELEASED;
                point = data->point = at;
                ui_set_input_mode(input, UI_INPUT_MODE_MOUSE);
                return;
            }
        }
        state = data->state = e.button.state;
        point = data->point = (lv_point_t) {.x = e.button.x, .y = e.button.y};
        ui_set_input_mode(input, UI_INPUT_MODE_MOUSE);
    }
}
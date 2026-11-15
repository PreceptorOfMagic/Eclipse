/*
 * ui_stick_scroll.c -- the right stick scrolls menus and dialogs. See ui_stick_scroll.h.
 */
#include "ui_stick_scroll.h"

#include "app.h"
#include "ui/root.h"
#include "lvgl.h"

#define STICK_ENGAGE 19660      /* ~60 % of 32767, as navkey_sdl.c */
#define STICK_RELEASE 11469     /* ~35 %: must fall back this far to let go */
#define STICK_SLOTS 8
#define TICK_MS 16

typedef struct {
    bool used;
    bool engaged;
    SDL_JoystickID which;
    int value;
} stick_t;

static stick_t sticks[STICK_SLOTS];
static lv_timer_t *timer;
static app_ui_input_t *ui_input;
static uint32_t last_tick;
static float carry;             /* sub-pixel remainder, so a slow scroll still moves */

static stick_t *stick_for(SDL_JoystickID which, bool create) {
    stick_t *free_slot = NULL;
    for (int i = 0; i < STICK_SLOTS; ++i) {
        if (sticks[i].used && sticks[i].which == which) {
            return &sticks[i];
        }
        if (!sticks[i].used && free_slot == NULL) {
            free_slot = &sticks[i];
        }
    }
    if (create && free_slot != NULL) {
        SDL_zerop(free_slot);
        free_slot->used = true;
        free_slot->which = which;
    }
    return create ? free_slot : NULL;
}

/* The strongest engaged deflection over all controllers; 0 = none. */
static int strongest(void) {
    int best = 0;
    for (int i = 0; i < STICK_SLOTS; ++i) {
        if (sticks[i].used && sticks[i].engaged && SDL_abs(sticks[i].value) > SDL_abs(best)) {
            best = sticks[i].value;
        }
    }
    return best;
}

/* What to scroll: the nearest ancestor of the focused object that can scroll vertically. In a
 * msgbox the focus is on the buttons, which are not inside the content, so a msgbox on the way up
 * means its content. */
static lv_obj_t *scroll_target(void) {
    if (ui_input == NULL || ui_input->key.indev == NULL) {
        return NULL;
    }
    lv_group_t *group = ui_input->key.indev->group;
    lv_obj_t *obj = group != NULL ? lv_group_get_focused(group) : NULL;
    for (; obj != NULL; obj = lv_obj_get_parent(obj)) {
        lv_obj_t *candidate = obj;
        if (lv_obj_check_type(obj, &lv_msgbox_class)) {
            candidate = lv_msgbox_get_content(obj);
        }
        if (candidate != NULL && lv_obj_has_flag(candidate, LV_OBJ_FLAG_SCROLLABLE)
            && (lv_obj_get_scroll_dir(candidate) & LV_DIR_VER)
            && lv_obj_get_scroll_top(candidate) + lv_obj_get_scroll_bottom(candidate) > 0) {
            return candidate;
        }
    }
    return NULL;
}

static void stop(void) {
    if (timer != NULL) {
        lv_timer_del(timer);
        timer = NULL;
    }
    carry = 0;
}

static void tick_cb(lv_timer_t *t) {
    (void) t;
    const int v = strongest();
    if (v == 0 || ui_input == NULL || ui_input->ui->app->session != NULL) {
        stop();
        return;
    }
    const uint32_t dt = lv_tick_elaps(last_tick);
    last_tick = lv_tick_get();
    lv_obj_t *target = scroll_target();
    if (target == NULL) {
        return;
    }
    /* Speed from the deflection beyond the release point, squared for fine control near it:
     * up to about two screen heights a second at full tilt. */
    float f = (float) (SDL_abs(v) - STICK_RELEASE) / (float) (32767 - STICK_RELEASE);
    f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    const float px_per_s = (float) LV_MAX(LV_DPX(300), lv_obj_get_height(lv_scr_act()) * 2) * f * f +
                           (float) LV_DPX(120);
    carry += px_per_s * (float) SDL_min(dt, 100u) / 1000.0f;
    const lv_coord_t step = (lv_coord_t) carry;
    if (step == 0) {
        return;
    }
    carry -= (float) step;
    /* lv_obj_scroll.c: a POSITIVE dy shows the content ABOVE. SDL's Y grows downward, so a stick
     * pushed down (v > 0) scrolls toward the bottom: dy < 0. */
    lv_obj_scroll_by_bounded(target, 0, v > 0 ? -step : step, LV_ANIM_OFF);
}

void ui_stick_scroll_axis(app_ui_input_t *input, SDL_JoystickID which, int value) {
    ui_input = input;
    stick_t *s = stick_for(which, true);
    if (s == NULL) {
        return;
    }
    s->value = value;
    const int mag = SDL_abs(value);
    if (!s->engaged && mag >= STICK_ENGAGE) {
        s->engaged = true;
    } else if (s->engaged && mag < STICK_RELEASE) {
        s->engaged = false;
    }
    if (strongest() != 0 && timer == NULL) {
        last_tick = lv_tick_get();
        carry = 0;
        timer = lv_timer_create(tick_cb, TICK_MS, NULL);
    }
    if (s->engaged) {
        ui_set_input_mode(input, UI_INPUT_MODE_GAMEPAD);
    }
}

void ui_stick_scroll_forget(SDL_JoystickID which) {
    stick_t *s = stick_for(which, false);
    if (s != NULL) {
        s->used = false;
        s->engaged = false;
    }
}

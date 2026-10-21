#include "app.h"
#include "config.h"

#include "pref_obj.h"

#include "ui/settings/settings.controller.h"

#include "util/i18n.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct input_pane_t {
    lv_fragment_t base;
    settings_controller_t *parent;

    lv_obj_t *absmouse_toggle;
    lv_obj_t *absmouse_hint;
    lv_obj_t *deadzone_label;
    lv_obj_t *deadzone_slider;
    lv_obj_t *swap_abxy_toggle;
} input_pane_t;

static const pref_dropdown_int_entry_t nintendo_layout_entries[] = {
        {.name = "Buttons as printed (A = A)", .value = NINTENDO_LAYOUT_AS_PRINTED},
        {.name = "Xbox positions (B = A)", .value = NINTENDO_LAYOUT_XBOX},
        {.name = "Don't detect them", .value = NINTENDO_LAYOUT_OFF},
};

static const pref_dropdown_int_entry_t controller_shortcuts_entries[] = {
        {.name = "Button combos and Start wheel", .value = CONTROLLER_SHORTCUTS_ALL},
        {.name = "Button combos only", .value = CONTROLLER_SHORTCUTS_COMBOS},
        {.name = "Start wheel only", .value = CONTROLLER_SHORTCUTS_WHEEL},
        {.name = "Off", .value = CONTROLLER_SHORTCUTS_NONE},
};

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *view);

static void pane_ctor(lv_fragment_t *self, void *args);

#if FEATURE_INPUT_EVMOUSE
static void hwmouse_state_update_cb(lv_event_t *e);

static void hwmouse_state_update(input_pane_t *pane);
#endif

static void update_deadzone_label(input_pane_t *pane);

static void on_deadzone_changed(lv_event_t *e);

static void mark_stream_reconnect_cb(lv_event_t *e);

static void mark_app_restart_cb(lv_event_t *e);

const lv_fragment_class_t settings_pane_input_cls = {
        .constructor_cb = pane_ctor,
        .create_obj_cb = create_obj,
        .instance_size = sizeof(input_pane_t),
};

static void pane_ctor(lv_fragment_t *self, void *args) {
    input_pane_t *pane = (input_pane_t *) self;
    pane->parent = args;
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container) {
    input_pane_t *pane = (input_pane_t *) self;
    lv_obj_t *view = pref_pane_container(container);
    lv_obj_set_layout(view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *viewonly_toggle = pref_checkbox(view, locstr("View-only mode"),
                                               &app_configuration->viewonly, false);
    lv_obj_add_event_cb(viewonly_toggle, mark_stream_reconnect_cb, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Don't send mouse, keyboard or gamepad input to host computer."), false);

    lv_obj_t *syskey_toggle = pref_checkbox(view, locstr("Capture system keys"),
                                             &app_configuration->syskey_capture, false);
    lv_obj_add_event_cb(syskey_toggle, mark_app_restart_cb, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Capture and send system keys (e.g. Meta/Win key) to host computer."), false);

    pref_header(view, locstr("Mouse"));

#if FEATURE_INPUT_EVMOUSE
    lv_obj_t *hwmouse_toggle = pref_checkbox(view, locstr("Use mouse hardware"),
                                             &app_configuration->hardware_mouse, false);
    lv_obj_add_event_cb(hwmouse_toggle, hwmouse_state_update_cb, LV_EVENT_VALUE_CHANGED, pane);
    lv_obj_add_event_cb(hwmouse_toggle, mark_stream_reconnect_cb, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Use plugged mouse device only when streaming. "
                                 "This will have better performance, but absolute mouse mode will not be enabled."),
                    false);
#endif

    pane->absmouse_toggle = pref_checkbox(view, locstr("Absolute mouse mode"),
                                          &app_configuration->absmouse, false);
    lv_obj_add_event_cb(pane->absmouse_toggle, mark_stream_reconnect_cb, LV_EVENT_VALUE_CHANGED, pane);
    pane->absmouse_hint = pref_desc_label(view, locstr("Better for remote desktop. "
                                                       "For some games, mouse will not work properly."), false);

    pref_header(view, locstr("Gamepad"));


    pane->deadzone_label = pref_title_label(view, locstr("Analog stick deadzone"));
    pane->deadzone_slider = pref_slider(view, &app_configuration->stick_deadzone, 0, 20, 1);
    lv_obj_set_width(pane->deadzone_slider, LV_PCT(100));
    lv_obj_add_event_cb(pane->deadzone_slider, on_deadzone_changed, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Note: Some games can enforce a larger deadzone "
                                 "than what Eclipse is configured to use."),
                    false);

    lv_obj_t *vmouse_toggle = pref_checkbox(view, locstr("Virtual mouse"),
                                             &app_configuration->virtual_mouse, false);
    lv_obj_add_event_cb(vmouse_toggle, mark_stream_reconnect_cb, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Press LB + RS to move mouse cursor with sticks. "
                                 "LT/RT for left/right mouse buttons."),
                    false);

    pref_title_label(view, locstr("Nintendo controllers"));
    lv_obj_t *nintendo_layout = pref_dropdown_int(view, nintendo_layout_entries,
                                                  sizeof(nintendo_layout_entries) / sizeof(nintendo_layout_entries[0]),
                                                  &app_configuration->nintendo_layout, NULL);
    lv_obj_set_width(nintendo_layout, LV_PCT(100));
    pref_desc_label(view, locstr("Switch Pro Controllers and Joy-Cons are recognised on their own: \"as printed\" "
                                 "sends A for the button labelled A, \"Xbox positions\" sends A for the bottom "
                                 "button, like an Xbox controller. Other controllers are not affected."), false);

    pane->swap_abxy_toggle = pref_checkbox(view, locstr("Swap ABXY buttons"), &app_configuration->swap_abxy, false);
    pref_desc_label(view, locstr("Swap A/B and X/Y on every controller, except Nintendo controllers "
                                 "(unless they are set to \"Don't detect them\")."),
                    false);

    pref_desc_label(view, locstr("Press RB + RS on the gamepad during streaming to open the keyboard."), false);

    pref_title_label(view, locstr("Controller shortcuts"));
    lv_obj_t *shortcuts = pref_dropdown_int(view, controller_shortcuts_entries,
                                            sizeof(controller_shortcuts_entries) /
                                            sizeof(controller_shortcuts_entries[0]),
                                            &app_configuration->controller_shortcuts, NULL);
    lv_obj_set_width(shortcuts, LV_PCT(100));
    pref_desc_label(view, locstr("Start wheel: hold Start, push a stick or the d-pad towards Menu, Mouse, Keyboard "
                                 "or Performance, and let go. It works on any controller. Button combos: "
                                 "LB + RB + View + Start opens the stream menu, and the combos above."), false);

#if FEATURE_INPUT_EVMOUSE
    hwmouse_state_update(pane);
#endif
    update_deadzone_label(pane);
    return view;
}

#if FEATURE_INPUT_EVMOUSE
static void hwmouse_state_update_cb(lv_event_t *e) {
    hwmouse_state_update((input_pane_t *) lv_event_get_user_data(e));
}

static void hwmouse_state_update(input_pane_t *pane) {
    if (app_configuration->hardware_mouse) {
        lv_obj_add_state(pane->absmouse_toggle, LV_STATE_DISABLED);
        lv_label_set_text(pane->absmouse_hint, locstr("Absolute mouse mode can't be used when "
                                                      "\"Use mouse hardware\" enabled."));
    } else {
        lv_obj_clear_state(pane->absmouse_toggle, LV_STATE_DISABLED);
        lv_label_set_text(pane->absmouse_hint, locstr("Better for remote desktop. "
                                                      "For some games, mouse will not work properly."));
    }
}
#endif

static void update_deadzone_label(input_pane_t *pane) {
    lv_label_set_text_fmt(pane->deadzone_label, "%s - %d", locstr("Analog stick deadzone"),
                          app_configuration->stick_deadzone);
}

static void on_deadzone_changed(lv_event_t *e) {
    input_pane_t *pane = (input_pane_t *) lv_event_get_user_data(e);
    pane->parent->needs_stream_reconnect = true;
    update_deadzone_label(pane);
}


static void mark_stream_reconnect_cb(lv_event_t *e) {
    input_pane_t *pane = (input_pane_t *) lv_event_get_user_data(e);
    pane->parent->needs_stream_reconnect = true;
}

static void mark_app_restart_cb(lv_event_t *e) {
    input_pane_t *pane = (input_pane_t *) lv_event_get_user_data(e);
    pane->parent->needs_app_restart = true;
}

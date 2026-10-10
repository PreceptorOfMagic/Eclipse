#include "platform/this_pc.h"
#include "app.h"

#include "pref_obj.h"

#include "app_settings.h"
#include "backend/pcmanager.h"
#include "backend/types.h"
#include "ui/launcher/pin.dialog.h"
#include "ui/settings/settings.controller.h"

#include "lvgl/util/lv_app_utils.h"
#include "util/i18n.h"

/*
 * WAKE & SIGN-IN LIVES HERE, not under Co-op. Waking a PC and typing its credential is
 * ordinary single-host behaviour that a co-op launch happens to also use, and filing it
 * under a co-op tab would hide it from everyone who never opens one.
 *
 * ONE ROW PER PC, BUILT FROM THE HOST LIST. Not a fixed pair: the number of machines is
 * however many have been paired, and a hard-coded two would silently drop the third. The
 * rows sit in the pane's flex column, so any number of them scrolls like the rest.
 */

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *view);

static void pane_ctor(lv_fragment_t *self, void *args);

static void mark_stream_reconnect_cb(lv_event_t *e);

typedef struct host_pane_t {
    lv_fragment_t base;
    settings_controller_t *parent;
} host_pane_t;

/** Per-row state: which host it is, and the label that says what it holds. */
typedef struct host_pin_row_t {
    uuidstr_t uuid;
    lv_obj_t *value;
} host_pin_row_t;

const lv_fragment_class_t settings_pane_host_cls = {
        .constructor_cb = pane_ctor,
        .create_obj_cb = create_obj,
        .instance_size = sizeof(host_pane_t),
};

static void pane_ctor(lv_fragment_t *self, void *args) {
    host_pane_t *pane = (host_pane_t *) self;
    pane->parent = args;
}

/** "PIN set" / "Password set" / "Not set" -- never the credential itself. */
static void row_value_refresh(void *userdata) {
    host_pin_row_t *row = userdata;
    if (row == NULL || row->value == NULL) {
        return;
    }
    const pclist_t *node = pcmanager_node(pcmanager, &row->uuid);
    const bool set = node != NULL && node->pin_code != NULL && node->pin_code[0] != '\0';
    lv_label_set_text(row->value, !set ? locstr("Not set")
                                       : (node->pin_is_password ? locstr("Password set")
                                                                : locstr("PIN set")));
    lv_obj_set_style_text_opa(row->value, set ? LV_OPA_COVER : LV_OPA_50, 0);
}

static void row_clicked_cb(lv_event_t *e) {
    host_pin_row_t *row = lv_event_get_user_data(e);
    pin_dialog_open(&row->uuid, row_value_refresh, row);
}

static void row_deleted_cb(lv_event_t *e) {
    lv_mem_free(lv_event_get_user_data(e));
}

static void build_host_row(lv_obj_t *parent, const pclist_t *node) {
    host_pin_row_t *row = lv_mem_alloc(sizeof(*row));
    if (row == NULL) {
        return;
    }
    lv_memset_00(row, sizeof(*row));
    row->uuid = node->id;

    /* A BUTTON, not a plain container: only group_def widgets join the focus group, so
     * an lv_obj row is unreachable with a d-pad. Styled flat so it reads as a list row
     * and takes the focus highlight like one. */
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_ver(btn, LV_DPX(10), 0);
    lv_obj_set_style_pad_hor(btn, LV_DPX(8), 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(btn, row_clicked_cb, LV_EVENT_SHORT_CLICKED, row);
    lv_obj_add_event_cb(btn, row_deleted_cb, LV_EVENT_DELETE, row);

    lv_obj_t *name = lv_label_create(btn);
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_label_set_text(name, node->server != NULL && node->server->hostname != NULL
                            ? node->server->hostname : locstr("Unnamed PC"));

    row->value = lv_label_create(btn);
    lv_obj_set_style_text_font(row->value, lv_theme_get_font_small(row->value), 0);
    row_value_refresh(row);
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container) {
    host_pane_t *pane = (host_pane_t *) self;
    lv_obj_t *view = pref_pane_container(container);
    lv_obj_set_layout(view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *sops_toggle = pref_checkbox(view, locstr("Optimize game settings for streaming"),
                                           &app_configuration->sops, false);
    lv_obj_add_event_cb(sops_toggle, mark_stream_reconnect_cb, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Change in-game settings to optimize for streaming. Resolution will be "
                                 "limited to 720p, 1080p or 4K. Framerate will be also limited to 30/60 FPS."),
                    false);
    lv_obj_t *local_audio_toggle = pref_checkbox(view, locstr("Mute computer while streaming"),
                                                  &app_configuration->localaudio, true);
    lv_obj_add_event_cb(local_audio_toggle, mark_stream_reconnect_cb, LV_EVENT_VALUE_CHANGED, pane);

    /* HOST MONITOR (app_settings.h host_monitor_mode): which start buttons a launch offers. */
    static const pref_dropdown_int_entry_t monitor_entries[] = {
            {.name = "Choose when starting a stream", .value = HOST_MONITOR_CHOOSE},
            {.name = "Always off", .value = HOST_MONITOR_OFF},
            {.name = "Always on", .value = HOST_MONITOR_ON},
    };
    pref_header(view, locstr("Computer's monitor while streaming"));
    lv_obj_t *monitor = pref_dropdown_int(view, monitor_entries, sizeof(monitor_entries) / sizeof(monitor_entries[0]),
                                          &app_configuration->host_monitor_mode, NULL);
    lv_obj_set_width(monitor, LV_PCT(100));
    pref_desc_label(view, locstr("Whether the streamed computer's own screen switches off while you play. "
                                 "\"Choose\" adds a second start button, \"Start streaming (host monitor off)\"; "
                                 "the others keep one button and always do the same. Needs Umbra on the computer, "
                                 "and never applies to the computer this app is running on."),
                    false);

    pref_header(view, locstr("Wake & sign-in"));
    pref_desc_label(view, locstr("A PC woken from sleep comes back to the Windows sign-in screen. "
                                 "These settings let Eclipse sign in for you."), false);

    pref_checkbox(view, locstr("Sign in automatically after waking a PC"),
                  &app_configuration->pin_autoentry, false);
    pref_desc_label(view, locstr("Types the credential set below for that PC. Turn this off to be "
                                 "left at the sign-in screen and type it yourself."), false);

    pref_checkbox(view, locstr("Send the credential as text"),
                  &app_configuration->pin_type_as_text, false);
    pref_desc_label(view, locstr("Sends the characters themselves, so it works whatever keyboard "
                                 "layout the PC is set to. Turn it off only if a PC never receives "
                                 "the credential at all — Eclipse then sends key presses instead, "
                                 "and those assume a US keyboard layout on the PC: symbols in a "
                                 "password would come out wrong on a UK, German or other layout."),
                    false);

    pref_checkbox(view, locstr("Restart the stream after signing in"),
                  &app_configuration->pin_hdr_relaunch, false);
    pref_desc_label(view, locstr("Needed for HDR: the PC only sets up an HDR display when a stream "
                                 "starts fresh, and the first one started on the sign-in screen. "
                                 "Costs a few seconds of reconnecting. Turn it off if you do not "
                                 "use HDR."), false);

    pref_header(view, locstr("Sign-in credential for each PC"));
#if defined(TARGET_WEBOS)
    pref_desc_label(view, locstr("Choose a PC to set its Windows PIN or password. A PIN gets a number "
                                 "pad; a password gets a full keyboard. Stored on this TV so it can be "
                                 "typed for you, and never shown again once set."), false);
#else
    pref_desc_label(view, locstr("Choose a PC to set its Windows PIN or password. A PIN gets a number "
                                 "pad; a password gets a full keyboard. Stored on this device so it can "
                                 "be typed for you, and never shown again once set."), false);
#endif

    int hosts = 0;
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        /* THIS PC appears only in the co-op options (issue #39). */
        if (cur->server == NULL || local_host_is_this_pc(cur->server->serverInfo.address)) {
            continue;
        }
        build_host_row(view, cur);
        hosts++;
    }
    if (hosts == 0) {
        /* Say why the list is empty rather than leaving a blank area that reads as a
         * broken screen. */
        pref_desc_label(view, locstr("No computers have been added yet."), false);
    }
    return view;
}

static void mark_stream_reconnect_cb(lv_event_t *e) {
    host_pane_t *pane = (host_pane_t *) lv_event_get_user_data(e);
    pane->parent->needs_stream_reconnect = true;
}

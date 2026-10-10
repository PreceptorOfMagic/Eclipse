#include "app.h"
#include "pin.dialog.h"
#include "app_settings.h"
#include "backend/pcmanager.h"
#include "backend/types.h"
#include "lvgl/util/lv_app_utils.h"

#include "util/i18n.h"

/*
 * PER-HOST WAKE-ON-LAN LOGIN CREDENTIAL.
 *
 * TWO KINDS, because Windows has two. A PIN is digits and Windows Hello submits it
 * the moment the last one is typed; a password is any character and always needs
 * Enter. Getting that wrong is not cosmetic -- a password typed with no Enter sits on
 * the lock screen forever, and there is no way to tell from the TV.
 *
 * THE KIND ALSO PICKS THE KEYBOARD. Someone entering a PIN on a remote should get a
 * number pad, not a full QWERTY they have to walk a cursor across. That is what the
 * user asked for and it is the difference between four presses and forty.
 */

static void dialog_cb(lv_event_t *event);

static void dialog_deleted_cb(lv_event_t *event);

static void input_key_cb(lv_event_t *event);

static void input_cancel_cb(lv_event_t *event);

static void kind_changed_cb(lv_event_t *event);

static void kind_key_preprocess_cb(lv_event_t *event);

typedef struct pin_dialog_controller_t {
    lv_obj_t *dialog;
    uuidstr_t uuid;
    void (*on_saved)(void *);
    void *userdata;
    lv_obj_t *input;
    lv_obj_t *kind;
    lv_obj_t *keyboard;
    lv_obj_t *hint;
    lv_obj_t *layout_warning;
} pin_dialog_controller_t;

static void layout_warning_refresh(pin_dialog_controller_t *controller);

static bool kind_is_password(const pin_dialog_controller_t *controller) {
    return controller->kind != NULL && lv_dropdown_get_selected(controller->kind) == 1;
}

/** Point the textarea and the on-screen keyboard at the selected kind. */
static void apply_kind(pin_dialog_controller_t *controller) {
    const bool password = kind_is_password(controller);
    /* accepted_chars is the guard that matters: without it a PIN can be given
     * letters that the digit-only typing path silently drops, and the login then
     * fails with a credential that looked right on this screen. */
    lv_textarea_set_accepted_chars(controller->input, password ? NULL : "0123456789");
    lv_textarea_set_placeholder_text(controller->input,
                                     password ? locstr("Password") : locstr("PIN digits"));
    lv_keyboard_set_mode(controller->keyboard,
                         password ? LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_NUMBER);
    lv_label_set_text(controller->hint,
                      password
                      ? locstr("Typed at the Windows sign-in screen after this PC is woken, "
                               "followed by Enter. Leave empty to clear.")
                      : locstr("Typed at the Windows sign-in screen after this PC is woken. "
                               "Leave empty to clear."));
    if (!password) {
        /* Switching password -> PIN with letters already in the box: accepted_chars
         * only filters NEW input, so the existing text has to be cleaned out or it is
         * saved as a PIN that cannot be typed. */
        const char *text = lv_textarea_get_text(controller->input);
        for (const char *p = text; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9') {
                lv_textarea_set_text(controller->input, "");
                break;
            }
        }
    }
}

static void kind_key_preprocess_cb(lv_event_t *event) {
    lv_obj_t *target = lv_event_get_target(event);
    if (lv_dropdown_is_open(target)) {
        return;
    }
    lv_group_t *group = lv_obj_get_group(target);
    if (group == NULL) {
        return;
    }
    switch (lv_event_get_key(event)) {
        case LV_KEY_DOWN:
        case LV_KEY_RIGHT:
            lv_group_focus_next(group);
            lv_event_stop_processing(event);
            break;
        case LV_KEY_UP:
        case LV_KEY_LEFT:
            lv_group_focus_prev(group);
            lv_event_stop_processing(event);
            break;
        default:
            break;
    }
}

static void kind_changed_cb(lv_event_t *event) {
    pin_dialog_controller_t *controller = lv_event_get_user_data(event);
    apply_kind(controller);
    layout_warning_refresh(controller);
}

/*
 * THE US-LAYOUT WARNING, and why it only appears sometimes.
 *
 * With "Send the credential as text" on -- the default -- there is no layout involved
 * at all: the characters go over as characters. The warning would be noise.
 *
 * With it off, Eclipse sends KEY POSITIONS, and what each position produces is decided
 * by the layout selected on the HOST. Letters and digits sit in the same place on
 * essentially every Latin layout, so they are safe; SYMBOLS are not -- on a UK layout
 * the US `"` key gives `@`, on a German one `y` and `z` are swapped. That silently
 * types a different password, and a lock screen gives no clue which.
 *
 * So: warn only when it can actually bite -- key-code mode AND a character outside
 * [0-9A-Za-z] in the box. Anything broader is a warning people learn to ignore.
 */
static void layout_warning_refresh(pin_dialog_controller_t *controller) {
    bool risky = false;
    if (app_configuration != NULL && !app_configuration->pin_type_as_text) {
        for (const char *p = lv_textarea_get_text(controller->input); *p != '\0'; ++p) {
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= 'A' && *p <= 'Z'))) {
                risky = true;
                break;
            }
        }
    }
    if (risky) {
        lv_obj_clear_flag(controller->layout_warning, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(controller->layout_warning, LV_OBJ_FLAG_HIDDEN);
    }
}

static void input_changed_cb(lv_event_t *event) {
    layout_warning_refresh((pin_dialog_controller_t *) lv_event_get_user_data(event));
}

void pin_dialog_open(const uuidstr_t *uuid, void (*on_saved)(void *), void *userdata) {
    pin_dialog_controller_t *controller = lv_mem_alloc(sizeof(*controller));
    if (controller == NULL) {
        return;
    }
    lv_memset_00(controller, sizeof(*controller));
    controller->uuid = *uuid;
    controller->on_saved = on_saved;
    controller->userdata = userdata;
    const pclist_t *node = pcmanager_node(pcmanager, &controller->uuid);
    const char *name = node != NULL && node->server != NULL && node->server->hostname != NULL
                       ? node->server->hostname : locstr("this PC");

    const static char *btn_texts[] = {translatable("Cancel"), translatable("OK"), ""};
    lv_obj_t *dialog = lv_msgbox_create_i18n(NULL, locstr("Sign-in credential"), NULL, btn_texts, false);
    lv_obj_add_event_cb(dialog, dialog_cb, LV_EVENT_VALUE_CHANGED, controller);
    lv_obj_set_style_max_width(dialog, LV_PCT(70), 0);
    lv_obj_set_width(dialog, LV_PCT(70));
    /* Room for the keyboard below everything else. The theme's cap is lower than this
     * and silently wins otherwise. */
    lv_obj_set_style_max_height(dialog, LV_PCT(92), 0);
    lv_obj_t *content = lv_msgbox_get_content(dialog);
    lv_obj_set_style_pad_all(content, lv_dpx(8), 0);
    lv_obj_set_style_pad_gap(content, lv_dpx(8), 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    /* Scrollable ON PURPOSE, unlike the version this replaces. It should always fit,
     * but a longer translation or a larger font must push the keyboard out of reach
     * rather than off the edge of a box that refuses to scroll. */
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    /* NAME THE MACHINE. With a row per PC in Settings, an unlabelled dialog is how a
     * credential ends up on the wrong one. */
    lv_obj_t *who = lv_label_create(content);
    lv_obj_set_width(who, LV_PCT(100));
    lv_label_set_long_mode(who, LV_LABEL_LONG_WRAP);
    lv_label_set_text_fmt(who, locstr("Sign-in for %s"), name);

    controller->kind = lv_dropdown_create(content);
    /* WITHOUT THIS, DOWN OPENS THE LIST INSTEAD OF MOVING ON. lv_dropdown's own key
     * handler treats LV_KEY_DOWN/RIGHT as "open", so walking the dialog with a d-pad
     * stops dead here and drops a menu over the screen. PREPROCESS is required: the
     * widget's class handler runs BEFORE ordinary user callbacks, so only a
     * preprocess handler can take the key away from it. When the list IS open the key
     * is left alone -- that is when it legitimately means "next option". */
    lv_obj_add_event_cb(controller->kind, kind_key_preprocess_cb, LV_EVENT_PREPROCESS |
                                                                  LV_EVENT_KEY, controller);
    lv_obj_set_width(controller->kind, LV_PCT(100));
    lv_dropdown_set_options_static(controller->kind, "PIN\nPassword");
    lv_dropdown_set_selected(controller->kind,
                             node != NULL && node->pin_is_password ? 1 : 0);
    lv_obj_add_event_cb(controller->kind, kind_changed_cb, LV_EVENT_VALUE_CHANGED, controller);

    controller->hint = lv_label_create(content);
    lv_obj_set_width(controller->hint, LV_PCT(100));
    lv_label_set_long_mode(controller->hint, LV_LABEL_LONG_WRAP);

    lv_obj_t *input = lv_textarea_create(content);
    lv_obj_set_width(input, LV_PCT(100));
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_password_mode(input, true);
    lv_textarea_set_max_length(input, 63);
    if (node != NULL && node->pin_code != NULL) {
        lv_textarea_set_text(input, node->pin_code);
    }
    lv_obj_add_event_cb(input, input_key_cb, LV_EVENT_KEY, controller);
    lv_obj_add_event_cb(input, input_cancel_cb, LV_EVENT_CANCEL, controller);
    lv_obj_add_event_cb(input, input_changed_cb, LV_EVENT_VALUE_CHANGED, controller);
    controller->input = input;

    controller->layout_warning = lv_label_create(content);
    lv_obj_set_width(controller->layout_warning, LV_PCT(100));
    lv_label_set_long_mode(controller->layout_warning, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(controller->layout_warning,
                                lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_label_set_text(controller->layout_warning,
                      locstr("\"Send the credential as text\" is off, so this is typed as US "
                             "keyboard positions. Symbols will come out wrong unless this PC is "
                             "set to a US layout. Turn it back on in Host Settings."));

    /* ON-SCREEN KEYBOARD, because a TV has no other one. It is a btnmatrix, so it is
     * reachable with the d-pad as well as the magic-remote pointer.
     *
     * SIZED FROM THE SCREEN, NOT THE PARENT. LV_PCT(45) here rendered as a ~15 px
     * sliver: a msgbox sizes itself to its content, so the content has no height of
     * its own for a percentage to resolve against, and the theme's max_height then
     * clamped what little there was. A fraction of the display is a real number at
     * the moment it is set. */
    controller->keyboard = lv_keyboard_create(content);
    lv_obj_set_width(controller->keyboard, LV_PCT(100));
    lv_obj_set_height(controller->keyboard, lv_disp_get_ver_res(NULL) * 38 / 100);
    lv_keyboard_set_textarea(controller->keyboard, input);
    /* THE CONTROL KEYS ARE LV_SYMBOL_* GLYPHS and drew as tofu boxes: they live in the
     * FontAwesome range that the built-in Montserrat fonts carry, and neither Eclipse's
     * text font nor its 18-glyph Material icon subset has it. Montserrat 22 is enabled
     * in lv_conf.h for exactly this. Applied to this widget only, so nothing else in
     * the app changes typeface.
     *
     * BOTH PARTS, and that is not belt-and-braces. The theme sets a font on the
     * btnmatrix MAIN part at creation; setting only ITEMS left the keys still drawing
     * in the app font, which renders a missing glyph as a filled box -- so the control
     * keys came out as tofu while the digits looked fine. Setting MAIN as well means
     * the lookup lands on Montserrat whichever part it resolves against. */
    lv_obj_set_style_text_font(controller->keyboard, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_font(controller->keyboard, &lv_font_montserrat_22, LV_PART_ITEMS);
    /* BACK gets out of the keyboard. A btnmatrix consumes every arrow key to walk its
     * own buttons, so without this the d-pad has no way back to Cancel/OK once focus
     * lands in here -- the same trap the textarea already guards against. */
    lv_obj_add_event_cb(controller->keyboard, input_cancel_cb, LV_EVENT_CANCEL, controller);

    apply_kind(controller);
    layout_warning_refresh(controller);

    controller->dialog = dialog;
    lv_obj_add_event_cb(dialog, dialog_deleted_cb, LV_EVENT_DELETE, controller);
    lv_obj_center(dialog);
}

static void dialog_deleted_cb(lv_event_t *event) {
    lv_mem_free(lv_event_get_user_data(event));
}

static void dialog_cb(lv_event_t *event) {
    pin_dialog_controller_t *controller = lv_event_get_user_data(event);
    lv_obj_t *dialog = lv_event_get_current_target(event);
    if (dialog != controller->dialog) {
        return;
    }
    uint16_t btn = lv_msgbox_get_active_btn(dialog);
    if (btn == 1) {
        pcmanager_set_host_pin(pcmanager, &controller->uuid,
                               lv_textarea_get_text(controller->input),
                               kind_is_password(controller));
        if (controller->on_saved != NULL) {
            controller->on_saved(controller->userdata);
        }
    }
    lv_msgbox_close_async(dialog);
}

static void input_key_cb(lv_event_t *event) {
    switch (lv_event_get_key(event)) {
        case LV_KEY_UP: {
            lv_group_t *group = lv_obj_get_group(lv_event_get_target(event));
            lv_group_focus_prev(group);
            break;
        }
        case LV_KEY_DOWN: {
            lv_group_t *group = lv_obj_get_group(lv_event_get_target(event));
            lv_group_focus_next(group);
            break;
        }
    }
}

static void input_cancel_cb(lv_event_t *event) {
    pin_dialog_controller_t *controller = lv_event_get_user_data(event);
    lv_obj_t *btns = lv_msgbox_get_btns(controller->dialog);
    lv_group_focus_obj(btns);
}

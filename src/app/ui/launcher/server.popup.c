#include "platform/this_pc.h"
#include "server.popup.h"
#include "server.context_menu.h"
#include "coop.dialog.h"

#include "app.h"
#include "backend/pcmanager.h"

#include "lvgl.h"
#include "lvgl/util/lv_app_utils.h"
#include "lvgl/font/material_icons_regular_symbols.h"
#include "lvgl/theme/lv_theme_moonlight.h"
#include "lvgl/theme/lv_theme_moonlight_colors.h"

#include "util/i18n.h"

#include <SDL.h>

typedef struct server_popup_t {
    lv_obj_t *msgbox;
    launcher_fragment_t *launcher;
} server_popup_t;

static const char *server_popup_item_icon(const pclist_t *node) {
    if (node == NULL) {
        return MAT_SYMBOL_WARNING;
    }
    switch (node->state.code) {
        case SERVER_STATE_NONE:
        case SERVER_STATE_QUERYING:
            return MAT_SYMBOL_TV;
        case SERVER_STATE_AVAILABLE:
            return node->server->currentGame ? MAT_SYMBOL_ONDEMAND_VIDEO : MAT_SYMBOL_TV;
        case SERVER_STATE_NOT_PAIRED:
            return MAT_SYMBOL_LOCK;
        case SERVER_STATE_ERROR:
        case SERVER_STATE_OFFLINE:
            return MAT_SYMBOL_WARNING;
        default:
            return MAT_SYMBOL_TV;
    }
}

static void server_popup_item_deleted(lv_event_t *e) {
    void *uuid = lv_obj_get_user_data(lv_event_get_target(e));
    if (uuid) {
        SDL_free(uuid);
    }
}

/* Marks the co-op row. A list item's user_data is otherwise a heap uuidstr_t, so the
 * sentinel has to be a pointer that can never be one -- its own address serves, and it
 * is checked before anything dereferences user_data as a uuid. */
static const char coop_entry_marker = 0;

static void server_popup_item_clicked(lv_event_t *e) {
    server_popup_t *popup = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target(e);
    /* Only react to clicks on actual list buttons, not the surrounding container. */
    if (lv_obj_get_parent(target) != lv_event_get_current_target(e)) { return; }
    void *data = lv_obj_get_user_data(target);
    if (data == &coop_entry_marker) {
        launcher_fragment_t *launcher = popup->launcher;
        lv_msgbox_close_async(popup->msgbox);
        coop_dialog_open(launcher);
        return;
    }
    const uuidstr_t *uuid = (const uuidstr_t *) data;
    if (!uuid || !popup->launcher) {
        lv_msgbox_close_async(popup->msgbox);
        return;
    }
    /* Snapshot the UUID since closing the popup destroys the list (and our user_data). */
    uuidstr_t snapshot = *uuid;
    lv_msgbox_close_async(popup->msgbox);
    launcher_select_server(popup->launcher, &snapshot);
}

static void server_popup_item_longpress(lv_event_t *e) {
    server_popup_t *popup = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target(e);
    if (lv_obj_get_parent(target) != lv_event_get_current_target(e)) { return; }
    void *data = lv_obj_get_user_data(target);
    /* The co-op row has no server behind it, so rename/pair/unpair mean nothing. */
    if (data == NULL || data == &coop_entry_marker) { return; }
    const uuidstr_t *uuid = (const uuidstr_t *) data;
    /* Reuse the existing server context menu (rename / pair / unpair). It opens as a
     * separate modal, so we close ourselves first to avoid stacking two modals on
     * top of each other and confusing the focus group. */
    uuidstr_t snapshot = *uuid;
    lv_msgbox_close_async(popup->msgbox);
    lv_fragment_t *fragment = lv_fragment_create(&server_menu_class, &snapshot);
    lv_obj_t *menu = lv_fragment_create_obj(fragment, NULL);
    lv_obj_add_event_cb(menu, ui_cb_destroy_fragment, LV_EVENT_DELETE, fragment);
}

/**
 * BACK CLOSES THE POPUP FROM A LIST ROW TOO.
 *
 * LVGL sends LV_EVENT_CANCEL to the FOCUSED object, and the theme's handler that closes
 * a msgbox sits on the msgbox -- reachable only by bubbling, which stops at the msgbox
 * content (no EVENT_BUBBLE there). So back worked while "Close" was focused and did
 * nothing once the d-pad had moved into the list, which is where it spends its time.
 */
static void server_popup_cancel(lv_event_t *e) {
    server_popup_t *popup = lv_event_get_user_data(e);
    lv_msgbox_close_async(popup->msgbox);
}

/**
 * UP AND DOWN INSIDE THE LIST.
 *
 * The theme moves focus for a msgbox by mapping up/down on the MSGBOX itself
 * (lv_theme_moonlight.c: msgbox_key), and a key only reaches it by bubbling. Bubbling
 * stops at the first object without LV_OBJ_FLAG_EVENT_BUBBLE, and the msgbox CONTENT
 * does not have it -- so a key pressed on a list row went row -> list -> content and
 * died there. The button row is the one child lv_msgbox marks as bubbling, which is why
 * the first press down from "Close" worked and every press after it did nothing: focus
 * was in the list and could not get out, by keypad OR by stick.
 *
 * Handled here rather than by making the content bubble, because that would carry
 * VALUE_CHANGED up to the msgbox too, where the dialog reads it as a button press.
 */
static void server_popup_list_key(lv_event_t *e) {
    lv_obj_t *list = lv_event_get_current_target(e);
    lv_group_t *group = lv_obj_get_group(list);
    if (group == NULL) {
        group = lv_obj_get_group(lv_event_get_target(e));
    }
    if (group == NULL) {
        return;
    }
    switch (lv_event_get_key(e)) {
        case LV_KEY_UP:
            lv_group_focus_prev(group);
            break;
        case LV_KEY_DOWN:
            lv_group_focus_next(group);
            break;
        default:
            break;
    }
}

static void server_popup_deleted(lv_event_t *e) {
    server_popup_t *popup = lv_event_get_user_data(e);
    SDL_free(popup);
}

static void server_popup_msgbox_cb(lv_event_t *e) {
    server_popup_t *popup = lv_event_get_user_data(e);
    lv_obj_t *msgbox = lv_event_get_current_target(e);
    if (msgbox != popup->msgbox) {
        return;
    }
    /* Button 0 = "Close" in lv_msgbox_create_i18n. */
    if (lv_msgbox_get_active_btn(msgbox) == 0) {
        lv_msgbox_close_async(msgbox);
    }
}

void server_popup_open(launcher_fragment_t *controller) {
    if (!controller) { return; }

    static const char *btn_texts[] = {translatable("Close"), ""};
    lv_obj_t *msgbox = lv_msgbox_create_i18n(NULL, locstr("Select server"), NULL, btn_texts, false);

    server_popup_t *popup = SDL_malloc(sizeof(server_popup_t));
    popup->msgbox = msgbox;
    popup->launcher = controller;
    lv_obj_set_user_data(msgbox, popup);
    lv_obj_add_event_cb(msgbox, server_popup_deleted, LV_EVENT_DELETE, popup);
    lv_obj_add_event_cb(msgbox, server_popup_msgbox_cb, LV_EVENT_VALUE_CHANGED, popup);

    lv_obj_t *content = lv_msgbox_get_content(msgbox);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, lv_dpx(8), 0);
    lv_obj_set_style_pad_gap(content, lv_dpx(8), 0);
    /* The popup width is constrained so very long PC names get truncated rather than
     * blowing the dialog out across the whole screen. */
    lv_obj_set_width(msgbox, LV_PCT(60));

    lv_obj_t *list = lv_list_create(content);
    lv_obj_add_flag(list, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_height(list, LV_DPX(280));
    /* ROOM FOR THE RING. A focus or hover outline is drawn OUTSIDE the row, and LVGL
     * clips children to their parent (lv_refr.c: children get the parent's coords as
     * their clip area) -- so with the list packed edge to edge the ring came out with
     * its sides missing, and the row below painted over the part that reached into it.
     * The padding and the row gap are the outline's own extent, nothing more. */
    lv_obj_set_style_pad_all(list, LV_DPX(4), 0);
    lv_obj_set_style_pad_row(list, LV_DPX(4), 0);
    lv_obj_set_style_radius(list, lv_dpx(8), 0);
    lv_obj_add_event_cb(list, server_popup_item_clicked, LV_EVENT_SHORT_CLICKED, popup);
    lv_obj_add_event_cb(list, server_popup_item_longpress, LV_EVENT_LONG_PRESSED, popup);
    lv_obj_add_event_cb(list, server_popup_cancel, LV_EVENT_CANCEL, popup);
    lv_obj_add_event_cb(list, server_popup_list_key, LV_EVENT_KEY, popup);

    /* CO-OP SITS WITH THE SERVERS, NOT IN A GAME LIST. It used to be an application
     * entry under each PC, which reads as "a game on that machine" -- but a co-op
     * session spans two hosts, so an entry filed under one of them can never name the
     * other. Listed first because it is a mode, not one more computer. */
    lv_obj_t *coop_item = lv_list_add_btn(list, MAT_SYMBOL_ADD_TO_QUEUE, locstr("Co-op (two PCs)"));
    lv_obj_add_flag(coop_item, LV_OBJ_FLAG_EVENT_BUBBLE);
    /* THE ICON IS IN THE ICON FONT, and lv_list_add_btn leaves the row's image label on
     * the TEXT font -- which is subsetted here and has no such glyph, so every row in
     * this popup drew tofu. The settings nav has always set this; the popup never did. */
    lv_btn_set_icon_font(coop_item, lv_theme_moonlight_get_iconfont_normal(coop_item));
    lv_obj_set_user_data(coop_item, (void *) &coop_entry_marker);

    lv_obj_t *focus_item = coop_item;
    int count = 0;
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        const SERVER_DATA *server = cur->server;
        if (server == NULL) { continue; }
        /* THIS PC is offered only as a co-op host (issue #39): streaming it to itself on its
         * own is not a thing, and the co-op entry above still lists it. BUT ONLY ONCE PAIRED
         * (D15, 2026-09-28): the co-op dialog lists paired PCs only, so hiding an UNPAIRED
         * this-PC left a fresh install no way to pair its own PC for co-op at all. Shown with
         * the padlock like any unpaired PC, and named as this PC, until it is paired -- and
         * only while its state is NOT_PAIRED: that is the one state in which a click opens the
         * pair dialog (launcher_select_server); in any other (querying, offline) a click would
         * make it the launcher's host instead. */
        const bool this_pc = local_host_is_this_pc(server->serverInfo.address);
        if (this_pc && cur->state.code != SERVER_STATE_NOT_PAIRED) { continue; }
        const char *icon = server_popup_item_icon(cur);
        char this_pc_label[160];
        if (this_pc) {
            snprintf(this_pc_label, sizeof(this_pc_label), "%s (%s)", server->hostname, locstr("this PC"));
        }
        lv_obj_t *item = lv_list_add_btn(list, icon, this_pc ? this_pc_label : server->hostname);
        lv_obj_add_flag(item, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_btn_set_icon_font(item, lv_theme_moonlight_get_iconfont_normal(item));
        if (cur->selected) {
            lv_obj_add_state(item, LV_STATE_CHECKED);
            /* SAY WHICH PC IS CURRENT (issue #3): the theme gives LV_STATE_CHECKED no look, so the
             * selected row was identical to the rest. An accent bar and accent text -- no glyph,
             * because the fonts are subsetted and a missing one draws tofu. */
            lv_obj_set_style_border_side(item, LV_BORDER_SIDE_LEFT, LV_STATE_CHECKED);
            lv_obj_set_style_border_width(item, LV_DPX(4), LV_STATE_CHECKED);
            lv_obj_set_style_border_color(item, ml_color_hex(ML_COLOR_PRIMARY), LV_STATE_CHECKED);
            lv_obj_set_style_border_opa(item, LV_OPA_COVER, LV_STATE_CHECKED);
            lv_obj_set_style_text_color(item, ml_color_hex(ML_COLOR_PRIMARY), LV_STATE_CHECKED);
            focus_item = item;
        }
        uuidstr_t *uuid = SDL_malloc(sizeof(uuidstr_t));
        *uuid = cur->id;
        lv_obj_set_user_data(item, uuid);
        lv_obj_add_event_cb(item, server_popup_item_deleted, LV_EVENT_DELETE, NULL);
        count++;
    }

    if (count == 0) {
        lv_obj_t *empty = lv_label_create(content);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_label_set_text_static(empty,
                                 locstr("No paired computers yet. Use the \"+\" button on the top bar to add one."));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    }

    /* OPEN ON THE CURRENT PC, NOT ON "Close". The dialog's button row joins the focus group
     * first (lv_msgbox_create builds it before these rows exist), so focus started on Close
     * -- and the first UP from there wrapped to the LAST row, selecting whatever PC happened
     * to be listed last (measured 2026-09-26: one press picked a PC that was not meant to be
     * touched). Starting on the selected row makes UP/DOWN mean "the PC above/below this one". */
    if (lv_obj_get_group(focus_item) != NULL) {
        lv_group_focus_obj(focus_item);
    }

    lv_obj_center(msgbox);
}

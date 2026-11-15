/*
 * pad_own_gate.c -- the stream-start popup for controller ownership. See pad_own_gate.h.
 *
 * The slow parts (the handle scan, closing and reopening Steam) run on a worker thread; an LVGL
 * timer polls for the result so the UI never blocks. Every path ends in exactly one done() call,
 * unless the caller cancelled.
 */
#include "pad_own_gate.h"

#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "app.h"
#include "app_settings.h"
#include "logging.h"
#include "lvgl.h"
#include "lvgl/theme/lv_theme_moonlight_colors.h"
#include "lvgl/util/lv_app_utils.h"
#include "ui/common/progress_dialog.h"
#include "util/i18n.h"

typedef enum {
    STAGE_IDLE = 0,
    STAGE_SCANNING,
    STAGE_ASKING,
    STAGE_EVICTING,
} stage_t;

static struct {
    stage_t stage;
    pad_own_gate_done_fn done;
    void *ctx;
    lv_obj_t *dialog;        /* the spinner or the question */
    lv_obj_t *remember;      /* the question's checkbox */
    lv_timer_t *timer;
    SDL_atomic_t worker_done;
    pad_own_scan_t scan;
    char report[512];
    char admin_report[1024];
    pad_own_admin_result_t admin_result;
    bool admin_ran;
} g;

static void notice(const char *why);

static void finish(bool proceed) {
    if (g.timer != NULL) {
        lv_timer_del(g.timer);
        g.timer = NULL;
    }
    if (g.dialog != NULL) {
        lv_msgbox_close_async(g.dialog);
        g.dialog = NULL;
    }
    g.remember = NULL;
    g.stage = STAGE_IDLE;
    pad_own_gate_done_fn done = g.done;
    void *ctx = g.ctx;
    g.done = NULL;
    g.ctx = NULL;
    if (!proceed || done == NULL) {
        /* Cancelled, or the dialog that asked went away: no stream will start, so nothing may
         * stay hidden. */
        pad_own_session_end();
    }
    if (done != NULL) {
        done(ctx, proceed);
    }
}

static int SDLCALL scan_worker(void *arg) {
    (void) arg;
    pad_own_scan(&g.scan);
    SDL_AtomicSet(&g.worker_done, 1);
    return 0;
}

static int SDLCALL evict_worker(void *arg) {
    (void) arg;
    /* THE ADMIN STEP FIRST: services restarted, driver routes locked -- here when this Eclipse is elevated,
     * otherwise through Windows' administrator prompt, which the question explained before it was answered.
     * Then scan again: a program that read the pads through a now-locked driver (Steam) is a holder now, and is
     * closed and reopened with the rest, as this user, so it comes back reading them the ordinary way. */
    g.admin_ran = pad_own_admin_needed(&g.scan);
    g.admin_result = g.admin_ran ? pad_own_evict_admin(g.admin_report, sizeof(g.admin_report))
                                 : PAD_OWN_ADMIN_DONE;
    if (g.admin_ran && g.admin_result == PAD_OWN_ADMIN_DONE) {
        pad_own_scan(&g.scan);
    }
    pad_own_evict(&g.scan, g.report, sizeof(g.report));
    SDL_AtomicSet(&g.worker_done, 1);
    return 0;
}

/* The scan could not look inside services and administrator programs (this Eclipse is not elevated), so they
 * are one row of the list, and the admin step finds out which of them hold the controllers. */
static bool admin_row_shown(void) {
    return !g.scan.elevated && g.scan.unscanned > 0;
}

static bool start_worker(SDL_ThreadFunction fn, const char *name) {
    SDL_AtomicSet(&g.worker_done, 0);
    SDL_Thread *t = SDL_CreateThread(fn, name, NULL);
    if (t == NULL) {
        return false;
    }
    SDL_DetachThread(t);
    return true;
}

static int closable_count(void) {
    int n = 0;
    for (int i = 0; i < g.scan.count; i++) {
        n += g.scan.holders[i].closable ? 1 : 0;
    }
    return n;
}

/* "Steam, Xbox Game Bar" -- each display name once. */
static void list_apps(char *out, size_t len, bool closable) {
    out[0] = 0;
    for (int i = 0; i < g.scan.count; i++) {
        const pad_own_holder_t *h = &g.scan.holders[i];
        if (h->closable != closable || strstr(out, h->display) != NULL) {
            continue;
        }
        const size_t used = strlen(out);
        snprintf(out + used, len - used, "%s%s", used ? ", " : "", h->display);
    }
}

static void begin_evict(void) {
    char apps[256];
    list_apps(apps, sizeof(apps), true);
    char msg[384];
    if (apps[0] != 0) {
        snprintf(msg, sizeof(msg), locstr("Closing and reopening %s..."), apps);
    } else {
        SDL_strlcpy(msg, locstr("Restarting the Windows services that are using the controllers..."), sizeof(msg));
    }
    g.dialog = progress_dialog_create(msg);
    g.stage = STAGE_EVICTING;
    if (!start_worker(evict_worker, "PadOwnEvict")) {
        finish(true);
    }
}

static void question_cb(lv_event_t *e) {
    lv_obj_t *msgbox = lv_event_get_current_target(e);
    if (g.stage != STAGE_ASKING || msgbox != g.dialog) {
        return;
    }
    const uint16_t picked = lv_btnmatrix_get_selected_btn(lv_msgbox_get_btns(msgbox));
    const bool remember = g.remember != NULL && lv_obj_has_state(g.remember, LV_STATE_CHECKED);
    lv_msgbox_close_async(msgbox);
    g.dialog = NULL;
    g.remember = NULL;
    if (picked == 0 || picked == 1) {
        if (remember) {
            app_configuration->pad_apps_policy = picked == 0 ? PAD_APPS_CLOSE_REOPEN : PAD_APPS_LEAVE;
            settings_save(app_configuration);
            commons_log_info("PadOwn", "choice remembered: %s", picked == 0 ? "close and reopen" : "leave them");
        }
        if (picked == 0) {
            begin_evict();
        } else {
            finish(true);
        }
        return;
    }
    finish(false);                      /* Cancel (2) or dismissed */
}

/* B on a controller, Escape, or anything else that closes the dialog without a button: the theme's
 * msgbox_cancel closes it directly and no button callback runs. Treat it as Cancel, or the launch
 * would wait forever with the controllers still hidden. A button answer clears g.dialog first, so
 * it never gets here. */
static void dialog_deleted_cb(lv_event_t *e) {
    if (lv_event_get_current_target(e) != g.dialog || g.stage != STAGE_ASKING) {
        return;
    }
    g.dialog = NULL;
    g.remember = NULL;
    finish(false);
}

/* Lay the dialog out for whatever window it is on: content stacked top to bottom, wrapped to the
 * dialog's width, NEVER scrolled sideways. Vertical scrolling stays only as the backstop the theme
 * gives every msgbox (msgbox_fit) for a window too short to hold it all. Fonts never shrink. */
static void fit_dialog(lv_obj_t *dialog) {
    /* As wide as the theme allows (its max-width is 60 %): wider lines = fewer lines = no scroll on
     * any ordinary window. */
    lv_obj_set_width(dialog, LV_PCT(60));
    lv_obj_t *content = lv_msgbox_get_content(dialog);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, LV_DPX(12), 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
}

static void remember_toggle(lv_obj_t *box) {
    if (lv_obj_has_state(box, LV_STATE_CHECKED)) {
        lv_obj_clear_state(box, LV_STATE_CHECKED);
    } else {
        lv_obj_add_state(box, LV_STATE_CHECKED);
    }
}

/* Mouse, Enter or the controller's A on the ROW toggles the box it carries. */
static void remember_click_cb(lv_event_t *e) {
    remember_toggle(lv_event_get_user_data(e));
}

/* Up/Down MOVE FOCUS here like everywhere else in a dialog (msgbox_key). */
static void remember_key_cb(lv_event_t *e) {
    lv_obj_t *row = lv_event_get_current_target(e);
    lv_group_t *group = lv_obj_get_group(row);
    if (group == NULL) {
        return;
    }
    const uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP) {
        lv_group_focus_prev(group);
    } else if (key == LV_KEY_DOWN) {
        lv_group_focus_next(group);
    }
}

/* "[ ] Remember my choice ..." as ONE item: the ROW is what takes the focus, the clicks and the highlight
 * (user 2026-11-04: "we only really need either the checkbox or the whole line highlighting" -- the box
 * and the label each drew their own, and a stray bar showed between them that removing the box's gap did
 * not cure). The box is only a picture of the state: not clickable, not in a group, so nothing else
 * highlights. The label wraps (an lv_checkbox's own label never does -- the old sideways scroll). */
static lv_obj_t *remember_row(lv_obj_t *dialog) {
    lv_obj_t *row = lv_obj_create(lv_msgbox_get_content(dialog));
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, LV_DPX(10), 0);
    lv_obj_set_style_pad_all(row, LV_DPX(6), 0);
    lv_obj_set_style_radius(row, LV_DPX(8), 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    /* The highlight: the same ring a focused button gets (lv_theme_moonlight.c), and the theme's hover
     * outline for the mouse -- both on the row only. DRAWN INSIDE THE ROW (negative pad = the width): the row
     * is the full width of the msgbox content, which clips anything outside it, so an outside ring showed as a
     * line along the top and stubs of the bottom corners (user photo 2026-11-13). The row's padding keeps it
     * off the box and the text. */
    lv_obj_set_style_outline_color(row, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(row, LV_DPX(2), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_opa(row, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(row, -LV_DPX(2), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(row, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_HOVERED);
    lv_obj_set_style_outline_width(row, LV_DPX(2), LV_STATE_HOVERED);
    lv_obj_set_style_outline_opa(row, LV_OPA_70, LV_STATE_HOVERED);
    lv_obj_set_style_outline_pad(row, -LV_DPX(2), LV_STATE_HOVERED);

    lv_obj_t *box = lv_checkbox_create(row);
    lv_checkbox_set_text_static(box, "");
    lv_obj_set_style_pad_column(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);      /* clicks fall through to the row */
    if (lv_obj_get_group(box) != NULL) {
        lv_group_remove_obj(box);
    }

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_text(label, locstr("Remember my choice (you can change it in Settings > Co-op)"));

    lv_obj_add_event_cb(row, remember_click_cb, LV_EVENT_CLICKED, box);
    lv_obj_add_event_cb(row, remember_key_cb, LV_EVENT_KEY, NULL);
    /* Reachable from a controller or the keyboard: only the msgbox's DIRECT children join its
     * group by themselves (cb_child_group_add), so put the row in the buttons' group. */
    lv_group_t *group = lv_obj_get_group(lv_msgbox_get_btns(dialog));
    if (group != NULL) {
        if (lv_obj_get_group(row) != NULL) {
            lv_group_remove_obj(row);
        }
        lv_group_add_obj(group, row);
    }
    return box;
}

/* One paragraph of the dialog's content: wraps to the dialog's width. */
static void add_paragraph(lv_obj_t *content, const char *text) {
    lv_obj_t *label = lv_label_create(content);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_text(label, text);
}

/* One bulleted row of an app list. "(admin)" after anything that needs administrator rights (user 2026-11-12:
 * "in the list of things that need to close just have something like (admin) next to them"). */
static void add_app_row(lv_obj_t *box, const char *display, bool admin) {
    lv_obj_t *row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, LV_DPX(12), 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, LV_DPX(7), LV_DPX(7));
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, ml_color_hex(ML_COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_t *name = lv_label_create(row);
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(name, 1);
    if (admin) {
        lv_label_set_text_fmt(name, "%s %s", display, locstr("(admin)"));
    } else {
        lv_label_set_text(name, display);
    }
}

/* The apps as a LIST the eye finds at once (user, 2026-11-04: a lone app name in open space did not
 * read as a list): an inset box, one row per app, each with a bullet. The bullet is a drawn dot, not
 * a glyph, so no font can show it as a missing character. Each display name once. */
static void add_app_list(lv_obj_t *content, bool closable) {
    lv_obj_t *box = lv_obj_create(content);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, ml_color_hex(ML_COLOR_SURFACE_ALT), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, ml_color_hex(ML_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(box, LV_DPX(1), 0);
    lv_obj_set_style_radius(box, LV_DPX(8), 0);
    lv_obj_set_style_pad_all(box, LV_DPX(12), 0);
    lv_obj_set_style_pad_row(box, LV_DPX(8), 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < g.scan.count; i++) {
        const pad_own_holder_t *h = &g.scan.holders[i];
        bool seen = false;
        for (int j = 0; j < i && !seen; j++) {
            seen = g.scan.holders[j].closable == closable && strcmp(g.scan.holders[j].display, h->display) == 0;
        }
        if (h->closable != closable || seen) {
            continue;
        }
        add_app_row(box, h->display, h->admin);
    }
    if (closable) {
        for (int i = 0; i < g.scan.routes && i < 4; i++) {
            add_app_row(box, g.scan.route[i], true);   /* a driver programs read the pads through */
        }
    }
    if (closable && admin_row_shown()) {
        add_app_row(box, locstr("Windows services and programs running as administrator"), true);
    }
}

static void ask(void) {
    char others[256];
    list_apps(others, sizeof(others), false);
    /* No prompt to explain when this Eclipse is elevated or the session's elevated copy already runs. */
    const bool admin_prompt = pad_own_admin_needed(&g.scan) && !g.scan.elevated && !pad_own_admin_ready();
    static const char *btns[] = {translatable("Close and reopen"), translatable("Leave them"),
                                 translatable("Cancel"), ""};
    g.dialog = lv_msgbox_create_i18n(NULL, locstr("Controllers for the other PC"), NULL, btns, false);
    fit_dialog(g.dialog);
    lv_obj_t *content = lv_msgbox_get_content(g.dialog);
    add_paragraph(content, locstr("These apps on this PC are already using the controllers you gave to the other PC:"));
    add_app_list(content, true);
    add_paragraph(content, locstr("\"Close and reopen\" restarts them in the background, and they stop seeing those "
                                  "controllers. Or choose \"Leave them\" and turn those controllers off and on."));
    if (others[0] != 0) {
        add_paragraph(content, locstr("Also using them, and Eclipse cannot close or restart these. If this PC's games still react, close them yourself, or turn those controllers off and on:"));
        add_app_list(content, false);
    }
    if (admin_prompt) {
        /* WHAT THE ADMINISTRATOR PROMPT IS FOR, before Windows shows it (decided 2026-11-03, the design's
         * "Admin rights"; user 2026-11-12: "prompt the user letting them know what the admin does"). */
        add_paragraph(content, locstr("Items marked (admin) need administrator rights. If you choose \"Close and "
                                      "reopen\", Windows asks you for permission once. Eclipse then restarts the "
                                      "Windows services using these controllers, and blocks drivers that pass them to "
                                      "apps (such as Steam's Xbox driver) for this session only, so this PC's games "
                                      "stop seeing them. When the session ends it undoes both, so this PC gets the "
                                      "controllers back. Nothing else is changed. To skip the prompt next time, "
                                      "right-click Eclipse and choose \"Run as administrator\"."));
    }
    lv_obj_add_event_cb(g.dialog, dialog_deleted_cb, LV_EVENT_DELETE, NULL);
    g.remember = remember_row(g.dialog);
    lv_obj_add_event_cb(g.dialog, question_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(g.dialog);
    g.stage = STAGE_ASKING;
}

static void after_scan(void) {
    if (g.dialog != NULL) {
        lv_msgbox_close_async(g.dialog);
        g.dialog = NULL;
    }
    const bool admin = pad_own_admin_needed(&g.scan);
    if (closable_count() == 0 && !admin) {
        commons_log_info("PadOwn", "no app needs closing (%d other holder(s))", g.scan.count);
        finish(true);
        return;
    }
    switch (app_configuration->pad_apps_policy) {
        case PAD_APPS_CLOSE_REOPEN:
            /* A remembered "close and reopen" never raises Windows' administrator prompt unexplained: the
             * question (which says what it is for) comes first whenever the prompt would. */
            if (admin && !g.scan.elevated && !pad_own_admin_ready()) {
                ask();
                return;
            }
            begin_evict();
            return;
        case PAD_APPS_LEAVE:
            finish(true);
            return;
        default:
            ask();
            return;
    }
}

static void poll_cb(lv_timer_t *t) {
    (void) t;
    if (!SDL_AtomicGet(&g.worker_done)) {
        return;
    }
    SDL_AtomicSet(&g.worker_done, 0);
    if (g.stage == STAGE_SCANNING) {
        after_scan();
    } else if (g.stage == STAGE_EVICTING) {
        commons_log_info("PadOwn", "evict: %s", g.report);
        if (g.admin_ran) {
            commons_log_info("PadOwn", "admin step: %s -- %s",
                             g.admin_result == PAD_OWN_ADMIN_DONE ? "done"
                             : g.admin_result == PAD_OWN_ADMIN_DECLINED ? "declined" : "FAILED",
                             g.admin_report);
        }
        if (g.dialog != NULL) {
            lv_msgbox_close_async(g.dialog);
            g.dialog = NULL;
        }
        if (g.admin_ran && g.admin_result != PAD_OWN_ADMIN_DONE) {
            /* Declined or failed (decided 2026-11-03): the stream goes on, and the user is told what to expect. */
            notice(g.admin_result == PAD_OWN_ADMIN_DECLINED
                   ? locstr("Administrator permission was not given, so Windows services that already had these "
                            "controllers open may still pass them to this PC's games. Turn those controllers off "
                            "and on to stop that.")
                   : locstr("Eclipse could not restart the Windows services using these controllers, so they may "
                            "still pass them to this PC's games. Turn those controllers off and on to stop that."));
            return;
        }
        finish(true);
    }
}

static void notice_cb(lv_event_t *e) {
    lv_obj_t *msgbox = lv_event_get_current_target(e);
    if (msgbox != g.dialog) {
        return;
    }
    const uint16_t picked = lv_btnmatrix_get_selected_btn(lv_msgbox_get_btns(msgbox));
    lv_msgbox_close_async(msgbox);
    g.dialog = NULL;
    finish(picked == 0);
}

/* Hiding is impossible on this PC: say what the user will see, let them continue or stop. */
static void notice(const char *why) {
    static const char *btns[] = {translatable("Continue"), translatable("Cancel"), ""};
    char text[700];
    snprintf(text, sizeof(text),
             locstr("%s\n\nThe controllers still work on the PC you gave them to; this PC's game may react to "
                    "them as well."), why);
    g.dialog = lv_msgbox_create_i18n(NULL, locstr("Controllers for the other PC"), text, btns, false);
    fit_dialog(g.dialog);
    lv_obj_add_event_cb(g.dialog, dialog_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(g.dialog, notice_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(g.dialog);
    g.stage = STAGE_ASKING;
}

bool pad_own_gate_begin(const pad_own_pad_t *pads, int count, pad_own_gate_done_fn done, void *ctx) {
    if (count <= 0 || g.stage != STAGE_IDLE) {
        return false;
    }
    g.done = done;
    g.ctx = ctx;
    if (g.timer == NULL) {
        g.timer = lv_timer_create(poll_cb, 100, NULL);
    }
    char why[320];
    if (!pad_own_supported(why, sizeof(why))) {
        commons_log_warn("PadOwn", "not supported: %s", why);
        notice(why);
        return true;
    }
    char err[160];
    if (!pad_own_hide(pads, count, err, sizeof(err))) {
        char msg[400];
        snprintf(msg, sizeof(msg), locstr("The controllers could not be hidden from this PC (%s)."), err);
        notice(msg);
        return true;
    }
    g.dialog = progress_dialog_create(locstr("Checking which apps are using the controllers..."));
    g.stage = STAGE_SCANNING;
    if (!start_worker(scan_worker, "PadOwnScan")) {
        finish(true);
    }
    return true;
}

void pad_own_gate_cancel(void *ctx) {
    if (g.ctx == ctx) {
        g.done = NULL;
        g.ctx = NULL;
    }
}

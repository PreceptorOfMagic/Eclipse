/*
 * admin_gate.c -- the "administrator rights for this session" question. See admin_gate.h.
 *
 * Windows' prompt blocks the thread that asks, so the asking runs on a worker; an LVGL timer polls for the
 * answer so the UI never blocks. Every path ends in exactly one done() call, unless the caller cancelled.
 */
#include "admin_gate.h"

#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "logging.h"
#include "lvgl.h"
#include "lvgl/util/lv_app_utils.h"
#include "platform/pad_ownership.h"
#include "ui/common/progress_dialog.h"
#include "util/i18n.h"

static struct {
    admin_gate_done_fn done;
    void *ctx;
    lv_obj_t *dialog;
    lv_timer_t *timer;
    SDL_atomic_t worker_done;
    pad_own_admin_result_t result;
    char report[512];
    bool asking;
} g;

static void finish(bool proceed) {
    if (g.timer != NULL) {
        lv_timer_del(g.timer);
        g.timer = NULL;
    }
    if (g.dialog != NULL) {
        lv_msgbox_close_async(g.dialog);
        g.dialog = NULL;
    }
    g.asking = false;
    admin_gate_done_fn done = g.done;
    void *ctx = g.ctx;
    g.done = NULL;
    g.ctx = NULL;
    if (done != NULL) {
        done(ctx, proceed);
    }
}

static int SDLCALL ask_worker(void *arg) {
    (void) arg;
    g.result = pad_own_session_admin_begin(g.report, sizeof(g.report));
    SDL_AtomicSet(&g.worker_done, 1);
    return 0;
}

static void fit(lv_obj_t *dialog) {
    lv_obj_set_width(dialog, LV_PCT(60));
    lv_obj_t *content = lv_msgbox_get_content(dialog);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, LV_DPX(12), 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
}

static void paragraph(lv_obj_t *content, const char *text) {
    lv_obj_t *label = lv_label_create(content);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_text(label, text);
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

static void deleted_cb(lv_event_t *e) {
    if (lv_event_get_current_target(e) != g.dialog) {
        return;
    }
    g.dialog = NULL;
    finish(false);                       /* B / Escape: treated as Cancel */
}

/* Declined or failed: the session can still run; say what will not work. */
static void without_admin_notice(bool declined) {
    static const char *btns[] = {translatable("Continue"), translatable("Cancel"), ""};
    g.dialog = lv_msgbox_create_i18n(NULL, locstr("Without administrator rights"), NULL, btns, false);
    fit(g.dialog);
    lv_obj_t *content = lv_msgbox_get_content(g.dialog);
    paragraph(content, declined ? locstr("Administrator permission was not given.")
                                : locstr("Eclipse could not get administrator rights."));
    paragraph(content, locstr("The session can still run, but on this PC:\n"
                              "- prompts asking for administrator permission (some games show one when they start) "
                              "stay hidden: the screen darkens and nothing can be clicked;\n"
                              "- Windows services and drivers that already have a controller given to the other PC "
                              "may keep passing it to this PC's games."));
    lv_obj_add_event_cb(g.dialog, deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(g.dialog, notice_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(g.dialog);
}

static void poll_cb(lv_timer_t *t) {
    (void) t;
    if (!SDL_AtomicGet(&g.worker_done)) {
        return;
    }
    SDL_AtomicSet(&g.worker_done, 0);
    if (g.dialog != NULL) {
        lv_msgbox_close_async(g.dialog);
        g.dialog = NULL;
    }
    commons_log_info("Admin", "session admin: %s -- %s",
                     g.result == PAD_OWN_ADMIN_DONE ? "in force"
                     : g.result == PAD_OWN_ADMIN_DECLINED ? "declined" : "FAILED", g.report);
    if (g.result == PAD_OWN_ADMIN_DONE) {
        finish(true);
        return;
    }
    without_admin_notice(g.result == PAD_OWN_ADMIN_DECLINED);
}

static void question_cb(lv_event_t *e) {
    lv_obj_t *msgbox = lv_event_get_current_target(e);
    if (msgbox != g.dialog) {
        return;
    }
    const uint16_t picked = lv_btnmatrix_get_selected_btn(lv_msgbox_get_btns(msgbox));
    lv_msgbox_close_async(msgbox);
    g.dialog = NULL;
    if (picked == 0) {
        g.dialog = progress_dialog_create(locstr("Waiting for Windows' administrator prompt..."));
        SDL_AtomicSet(&g.worker_done, 0);
        SDL_Thread *th = SDL_CreateThread(ask_worker, "AdminAsk", NULL);
        if (th == NULL) {
            finish(true);
            return;
        }
        SDL_DetachThread(th);
        return;
    }
    if (picked == 1) {
        commons_log_info("Admin", "session admin: the user chose to continue without it");
        finish(true);
        return;
    }
    finish(false);
}

bool admin_gate_begin(admin_gate_done_fn done, void *ctx) {
#ifndef _WIN32
    (void) done;
    (void) ctx;
    return false;                        /* only Windows asks for admin: Linux needs none (no driver, grabs need no root) */
#endif
    if (g.asking) {
        return false;
    }
    if (pad_own_admin_ready()) {
        /* Already elevated (or a copy is running): nothing to ask, but admin prompts still move to the pane. */
        char report[256];
        pad_own_session_admin_begin(report, sizeof(report));
        commons_log_info("Admin", "session admin without a question (already elevated): %s", report);
        return false;
    }
    g.done = done;
    g.ctx = ctx;
    g.asking = true;
    if (g.timer == NULL) {
        g.timer = lv_timer_create(poll_cb, 100, NULL);
    }
    static const char *btns[] = {translatable("Continue"), translatable("Without admin"), translatable("Cancel"), ""};
    g.dialog = lv_msgbox_create_i18n(NULL, locstr("Administrator rights for this session"), NULL, btns, false);
    fit(g.dialog);
    lv_obj_t *content = lv_msgbox_get_content(g.dialog);
    paragraph(content, locstr("This PC is both a co-op host and the screen you play on. For this session Eclipse "
                              "needs administrator rights, and Windows will ask once. They are used to:"));
    paragraph(content, locstr("- show prompts that ask for administrator permission (some games show one when they "
                              "start) inside this PC's half of the screen. Windows normally shows them on a "
                              "separate secure screen that cannot appear in the co-op view, so the screen would "
                              "darken with nothing to click;\n"
                              "- keep the controllers you gave to the other PC away from this PC's games, including "
                              "through Windows services and drivers that would otherwise still pass them on (such "
                              "as Steam's Xbox driver, whose extra features pause until the session ends)."));
    paragraph(content, locstr("Everything is put back when the session ends. Nothing else is changed. To skip this "
                              "question, right-click Eclipse and choose \"Run as administrator\"."));
    lv_obj_add_event_cb(g.dialog, deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(g.dialog, question_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(g.dialog);
    return true;
}

void admin_gate_cancel(void *ctx) {
    if (g.ctx == ctx) {
        g.done = NULL;
        g.ctx = NULL;
    }
}

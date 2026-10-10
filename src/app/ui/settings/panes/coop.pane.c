/*
 * CO-OP SPLIT-SCREEN settings.
 *
 * ORIENTATION IS NOT COSMETIC. It selects a different composition path in the client and a
 * different slice layout from the host:
 *  - HORIZONTAL splices one slice per pane. Long-proven, and costs nothing extra.
 *  - VERTICAL builds a two-tile-column picture. It has to: an HEVC slice is a contiguous run
 *    of CTUs in RASTER order, so a slice covering a whole pane can only ever be a horizontal
 *    band. Tiles are the only construct that makes a left/right split expressible.
 *
 * CONTROLLERS ARE COLUMNS, ONE PER PC, with everything unassigned on the left -- the same
 * shape as the launch dialog, because it is the same question asked once instead of every
 * session. The old screen had `Controller 1..4`, each a dropdown of player 1 / player 2 /
 * both: a slot cannot say which of the four exists, "player 1" is a PANE so an assignment
 * silently moved when the roles were swapped, and there was no way to say "these two pads
 * both belong to that PC".
 *
 * A PAD LIVES IN EXACTLY ONE COLUMN. Available, or one PC. Sending the same pad to both
 * machines at once is not offered anywhere any more (the user, 2026-09-26: no "Both" in the
 * launch dialog either); as a permanent default it mostly meant "I have not decided", and a
 * name that appears in three places at once is not a list anyone can read.
 *
 * ADVANCED HOLDS ONE VALUE PER ORIENTATION, shown as one control while the two agree and
 * as two columns once they part. The modes load opposite hardware and compose by different
 * paths, so a value measured in one has never been evidence for the other -- but making
 * every dial a pair up front would double the screen for a setup that mostly wants one
 * number. A dial that HAS been parted always shows both columns, whatever the tick says:
 * hiding a difference would be a lie about what is going to run.
 *
 * The dials are still sentinel files (coop_tunables.c), so the streaming code is untouched
 * and run_coop.sh keeps overriding them for a measured run.
 */
#include "logging.h"
#include "app.h"
#include "config.h"

#include "pref_obj.h"
#include "app_settings.h"

#include "backend/pcmanager.h"
#include "backend/aurora_host_caps.h"
#include "platform/input_names.h"
#include "stream/input/coop_assign.h"
#include "stream/input/coop_pi_pads.h"
#include "stream/video/coop_tunables.h"
#include "ui/settings/settings.controller.h"
#include "ui/ui_hover.h"
#include "lvgl/theme/lv_theme_moonlight.h"
#include "lvgl/theme/lv_theme_moonlight_colors.h"
#include "lvgl/util/lv_app_utils.h"

#include "util/i18n.h"

#include <SDL.h>

#include <string.h>

/**
 * A column never gets narrower than this; past the point where they all fit, the row
 * SCROLLS SIDEWAYS instead of squeezing. Dividing the width by the number of PCs looks
 * fine with two and is unusable with five -- "DESKTOP-86QFVD0" in a sixth of the screen
 * is an ellipsis -- and the count is not something this screen gets to limit: however
 * many computers someone has paired is how many columns there are.
 */
#define COOP_PANE_COL_MIN_W LV_DPX(150)
#define COOP_PANE_MAX_DIALS 24
#define COOP_PI_ROUTE_ACK_TIMEOUT_MS 250

/** One advanced dial, in both of its shapes. BOTH ARE BUILT and one is hidden, rather
 *  than rebuilt on demand: the tick that switches them fires from inside a callback on
 *  one of these very objects, and deleting the object you are being called from is how
 *  UI toolkits crash. Hiding also keeps the unused shape out of the focus order, because
 *  LVGL's group skips objects whose parent is hidden. */
/**
 * ONE SETTING IS ONE STOP, and everything inside it is reached with left and right.
 *
 * Every control used to be its own focus stop, so pressing DOWN walked through a
 * slider, then its explanation button, then the next slider -- and with the two
 * orientations split, through both of those as well. Down stopped meaning "next
 * setting", which is the only thing it should ever mean on a list this long. Worse, the
 * right-hand column of a split pair could not be reached at all by ordinary navigation.
 *
 * So the whole setting is the focus stop. Left and right move a cursor between its parts
 * -- horizontal value, vertical value, the explanation button -- and OK acts on the part
 * under that cursor. A SLIDER HAS TO BE OPENED FIRST: OK starts editing it, left and
 * right then change the value, and OK or Back finishes. Without that step there is no way
 * for left and right to mean "move between parts" and "change this number" at once, and
 * nudging a value while trying to reach the button beside it is exactly the accident this
 * design has to prevent.
 */
typedef struct dial_view_t {
    /** The single focus stop: everything below lives inside it. */
    lv_obj_t *row;
    lv_obj_t *title;
    lv_obj_t *merged_row;
    lv_obj_t *split_row;
    /** merged control, then the horizontal and vertical ones. */
    lv_obj_t *ctrl[3];
    /** What the cursor outlines: the box around each control, and the button. */
    lv_obj_t *part_box[3];
    lv_obj_t *explain_btn;
    int cursor;
    bool editing;
    /** The number beside each control. A slider on a TV is otherwise unreadable: there
     *  is no cursor to hover for a tooltip, and the knob's position says "somewhere
     *  around a third" when the value that matters is "8 slices" or "120 ms". */
    lv_obj_t *readout[3];
    const char *label;
    const char *zero_text;
    /** Exactly one of these is set. Both point at a two-element array indexed by
     *  COOP_ORIENTATION_*. */
    int *ivals;
    bool *bvals;
    int min, max, step;
    /** SAY WHAT THE NUMBER MEANS TO THE PERSON READING IT. A composite height is a fact
     *  about how the picture is assembled, not something anyone sitting down with this
     *  screen has any reason to know -- so that dial reports what each PLAYER gets, in
     *  the shape that orientation actually produces. */
    bool per_side;
} dial_view_t;

/** Every explanation the "Detailed explanation" buttons can open. Held per pane so the
 *  strings outlive the callback that shows them. */
typedef struct detail_ref_t {
    const char *title;
    const char *body;
} detail_ref_t;

typedef struct coop_pane_t {
    lv_fragment_t base;
    settings_controller_t *parent;
    /** Live sweep for hub-attached pads, so a controller that only ever appears through
     *  the Pi can still be assigned here. It never blocks. */
    coop_pi_scan_t pi_scan;
    lv_timer_t *pi_poll;

    /* --- controllers ------------------------------------------------------- */
    lv_obj_t *pad_box;
    lv_obj_t *pad_body;
    /** Sized at build time from however many PCs are known: no fixed cap, because a
     *  cap here does not mean "narrower columns", it means a PC that silently has no
     *  column and cannot be given a controller at all. */
    lv_obj_t **pad_col;                /**< where the pad labels live, one per column */
    uuidstr_t *host_uuid;              /**< col_count - 1 entries */
    int col_count;
    lv_obj_t *pad_item[KNOWN_PADS_MAX + 2];   /* + the mouse and the keyboard (coop_kbm_assign) */
    int pad_count;
    int pad_cursor;
    bool pad_held;
    lv_obj_t *forget_btn;
    lv_obj_t *forget_label;

    /* --- default PCs ------------------------------------------------------- */
    /* Entries for the two "default computer" dropdowns, built from the paired list at
     * pane construction. The dropdown keeps the pointers, so these outlive the build and
     * are freed with the pane. One array, used by both dropdowns: the choice they offer
     * is the same list. */
    pref_dropdown_string_entry_t *pc_entries;
    int pc_entries_len;
    char (*pc_entry_uuid)[UUIDSTR_CAPACITY];
    char **pc_entry_name;

    /* --- advanced ---------------------------------------------------------- */
    dial_view_t dials[COOP_PANE_MAX_DIALS];
    int dial_count;
    lv_obj_t *split_head;

    /* --- shared-sound filter ------------------------------------------------ */
    lv_obj_t *filter_ahead_label, *filter_ahead_slider;
} coop_pane_t;

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container);
static void build_pc_entries(coop_pane_t *pane);

static void pane_ctor(lv_fragment_t *self, void *args);

static void pane_dtor(lv_fragment_t *self);

static void pads_refresh(coop_pane_t *pane);

static void dials_refresh(coop_pane_t *pane);

const lv_fragment_class_t settings_pane_coop_cls = {
        .constructor_cb = pane_ctor,
        .destructor_cb = pane_dtor,
        .create_obj_cb = create_obj,
        .instance_size = sizeof(coop_pane_t),
};

static const pref_dropdown_int_entry_t orientation_entries[] = {
        {.name = "Horizontal (top / bottom)", .value = COOP_ORIENTATION_HORIZONTAL},
        {.name = "Vertical (left / right)", .value = COOP_ORIENTATION_VERTICAL},
};



static const pref_dropdown_int_entry_t encoder_entries[] = {
        {.name = "Automatic (recommended)", .value = COOP_ENCODER_AUTO},
        {.name = "Each PC's own encoder", .value = COOP_ENCODER_NATIVE},
        {.name = "GPU HEVC encoder on both PCs", .value = COOP_ENCODER_GPU_HEVC},
};

/* "Play shared sounds once" (user 2026-10-10): Accurate is the evaluated default, Fast the old 5 ms behaviour. */
static const pref_dropdown_int_entry_t filter_entries[] = {
        {.name = "Accurate", .value = COOP_AUDIO_FILTER_ACCURATE},
        {.name = "Fast", .value = COOP_AUDIO_FILTER_FAST},
        {.name = "Off", .value = COOP_AUDIO_FILTER_OFF},
};
#define FILTER_AHEAD_STEP_MS 5

/* The look-ahead slider belongs to Accurate only. Reads the widgets, not app_configuration: the pref helpers write the
 * setting back from their own event handlers, whose order relative to these is not guaranteed. */
static void filter_ahead_refresh(coop_pane_t *pane, int filter) {
    if (pane->filter_ahead_slider == NULL) return;
    const int ms = lv_slider_get_value(pane->filter_ahead_slider) * FILTER_AHEAD_STEP_MS;
    lv_label_set_text_fmt(pane->filter_ahead_label, locstr("Accurate look-ahead - %d ms (sound delayed %d ms)"), ms, ms + 5);
    if (filter == COOP_AUDIO_FILTER_ACCURATE) {
        lv_obj_clear_flag(pane->filter_ahead_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(pane->filter_ahead_slider, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(pane->filter_ahead_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(pane->filter_ahead_slider, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_filter_changed(lv_event_t *e) {
    const uint16_t i = lv_dropdown_get_selected(lv_event_get_current_target(e));
    if (i < sizeof(filter_entries) / sizeof(filter_entries[0])) {
        filter_ahead_refresh((coop_pane_t *) lv_event_get_user_data(e), filter_entries[i].value);
    }
}

static void on_filter_ahead_changed(lv_event_t *e) {
    filter_ahead_refresh((coop_pane_t *) lv_event_get_user_data(e), COOP_AUDIO_FILTER_ACCURATE);
}

static const pref_dropdown_int_entry_t audio_entries[] = {
        {.name = "Mix both", .value = COOP_AUDIO_MIX},
        {.name = "Player 1 only", .value = COOP_AUDIO_PRIMARY_ONLY},
        {.name = "Player 2 only", .value = COOP_AUDIO_SECONDARY_ONLY},
};

static void pane_ctor(lv_fragment_t *self, void *args) {
    coop_pane_t *pane = (coop_pane_t *) self;
    pane->parent = (settings_controller_t *) args;
}

static void pane_dtor(lv_fragment_t *self) {
    coop_pane_t *pane = (coop_pane_t *) self;
    if (pane->pi_poll != NULL) {
        lv_timer_del(pane->pi_poll);
        pane->pi_poll = NULL;
    }
    coop_pi_scan_end(&pane->pi_scan);
    lv_mem_free(pane->pad_col);
    lv_mem_free(pane->host_uuid);
    for (int i = 0; i < pane->pc_entries_len; ++i) {
        lv_mem_free(pane->pc_entry_name[i]);
    }
    lv_mem_free(pane->pc_entry_name);
    lv_mem_free(pane->pc_entry_uuid);
    lv_mem_free(pane->pc_entries);
    pane->pad_col = NULL;
    pane->host_uuid = NULL;
}

/* ------------------------------------------------------------ seen controllers */

static void remember_pi_pads(coop_pane_t *pane) {
    bool changed = false;
    for (size_t i = 0; i < pane->pi_scan.count; ++i) {
        changed |= settings_remember_pad(app_configuration, pane->pi_scan.pads[i].name);
    }
    if (changed) {
        settings_save(app_configuration);
    }
}

static void pi_poll_cb(lv_timer_t *timer) {
    coop_pane_t *pane = timer->user_data;
    if (coop_pi_scan_poll(&pane->pi_scan)) {
        remember_pi_pads(pane);
        pads_refresh(pane);
    }
}

/* ------------------------------------------------------------------ columns */

/** Which column @p name sits in: 0 = available, else 1 + host index. */
static int pad_column_of(const coop_pane_t *pane, const char *name) {
    for (int h = 0; h < pane->col_count - 1; ++h) {
        const pclist_t *node = pcmanager_node(pcmanager, &pane->host_uuid[h]);
        if (node != NULL && pcmanager_node_has_pad(node, name)) {
            return h + 1;
        }
    }
    return 0;
}

/** Which entry of the live Pi scan @p name is, or -1 when it is not a hub pad. */
static int pad_pi_index(const coop_pane_t *pane, const char *name) {
    for (size_t i = 0; i < pane->pi_scan.count; ++i) {
        if (strcmp(pane->pi_scan.pads[i].name, name) == 0) {
            return (int) i;
        }
    }
    return -1;
}

/** THE BOARD'S ITEMS are the remembered controllers, then -- while "assign mouse and keyboard
 *  like controllers" is on -- the mouse and the keyboard (issue #37). They are claimed per PC
 *  exactly like a pad, under reserved names, so everything below that moves or saves a pad
 *  works on them unchanged; only Forget leaves them alone. */
static const char *board_name(int i) {
    const int known = (int) app_configuration->known_pads_count;
    if (i < known) {
        return app_configuration->known_pads[i];
    }
    return i == known ? COOP_KBM_MOUSE : COOP_KBM_KEYBOARD;
}

static bool board_is_kbm(int i) {
    return i >= (int) app_configuration->known_pads_count;
}

/** The mouse and keyboard are shown by the devices' own names where Windows can tell them
 *  (platform/input_names.h), like a controller is. */
static const char *board_label(const char *name) {
    static char device[2][128];
    const bool mouse = strcmp(name, COOP_KBM_MOUSE) == 0;
    if (mouse || strcmp(name, COOP_KBM_KEYBOARD) == 0) {
        char *buf = device[mouse ? 0 : 1];
        input_device_label(!mouse, mouse ? locstr("Mouse") : locstr("Keyboard"), buf, sizeof(device[0]));
        return buf;
    }
    return name;
}

static void hub_route_error_dialog_cb(lv_event_t *e) {
    lv_msgbox_close_async(lv_event_get_current_target(e));
}

static void show_hub_route_error(const coop_pane_t *pane, int pi_idx,
                                 const char *controller, int column,
                                 const char *target, const char *addr,
                                 bool address_missing) {
    const char *hub = pane->pi_scan.pads[pi_idx].pi_addr;
    char message[512];
    if (address_missing) {
        snprintf(message, sizeof(message),
                 locstr("The controller hub at %s could not route '%s' to %s because Eclipse "
                        "does not have a usable address for that PC. Refresh or reconnect the "
                        "PC, then try again. The previous saved assignment was kept."),
                 hub, controller, target);
    } else if (column == 0) {
        snprintf(message, sizeof(message),
                 locstr("The controller hub at %s did not confirm clearing the route for '%s' "
                        "to Available (automatic discovery). Check that the hub is online and "
                        "reachable, then try again. The previous saved assignment was kept."),
                 hub, controller);
    } else {
        snprintf(message, sizeof(message),
                 locstr("The controller hub at %s did not confirm routing '%s' to %s (%s). "
                        "Check that the hub is online and reachable, then try again. The "
                        "previous saved assignment was kept."),
                 hub, controller, target, addr != NULL ? addr : locstr("no address"));
    }
    commons_log_warn("COOP", "%s", message);
    static const char *buttons[] = {translatable("OK"), ""};
    lv_obj_t *dialog = lv_msgbox_create_i18n(NULL, locstr("Controller route was not applied"),
                                             message, buttons, false);
    lv_obj_add_event_cb(dialog, hub_route_error_dialog_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(dialog);
}

/** Move the pad at @p index to @p column, exclusively.
 *
 * Hub routing is transactional from the UI's point of view: the Pi must acknowledge
 * the exact destination before hosts.ini is changed. On failure the old saved column
 * remains the source of truth, so the redraw cannot claim an unconfirmed route. */
static bool pad_assign_column(coop_pane_t *pane, int index, int column) {
    if (index < 0 || index >= pane->pad_count || column < 0 || column >= pane->col_count) {
        return false;
    }
    const char *name = board_name(index);
    const int previous_column = pad_column_of(pane, name);

    /* A HUB PAD HAS TO BE TOLD, NOT JUST RECORDED. Moving it back to Available
     * clears the pin (NULL host), handing the Pi back to automatic discovery. */
    const int pi_idx = pad_pi_index(pane, name);
    if (previous_column == column && pi_idx < 0) {
        return true;
    }
    if (pi_idx >= 0) {
        const char *addr = NULL;
        const char *target = locstr("Available (automatic discovery)");
        if (column > 0) {
            const pclist_t *node = pcmanager_node(pcmanager, &pane->host_uuid[column - 1]);
            if (node != NULL && node->server != NULL) {
                target = node->server->hostname != NULL ? node->server->hostname : locstr("unknown PC");
                addr = node->server->serverInfo.address;
            } else {
                target = locstr("unknown PC");
            }
            if (addr == NULL || addr[0] == '\0') {
                show_hub_route_error(pane, pi_idx, name, column, target, addr, true);
                return false;
            }
        }
        if (!coop_pi_pads_route(pane->pi_scan.pads[pi_idx].pi_addr,
                                pane->pi_scan.pads[pi_idx].name, addr,
                                COOP_PI_ROUTE_ACK_TIMEOUT_MS)) {
            show_hub_route_error(pane, pi_idx, name, column, target, addr, false);
            return false;
        }
    }

    /* Only an acknowledged hub route (or a local controller with no hub route) may
     * change the persisted host claims. Clearing every other host first is what makes
     * the columns mean exactly what they show. */
    for (int h = 0; h < pane->col_count - 1; ++h) {
        const bool want = column == h + 1;
        const pclist_t *node = pcmanager_node(pcmanager, &pane->host_uuid[h]);
        if (node != NULL && pcmanager_node_has_pad(node, name) != want) {
            pcmanager_set_host_pad(pcmanager, &pane->host_uuid[h], name, want);
        }
    }
    return true;
}

static void forget_label_update(coop_pane_t *pane) {
    if (pane->forget_label == NULL) {
        return;
    }
    char text[128];
    if (pane->pad_count > 0 && !board_is_kbm(pane->pad_cursor)) {
        snprintf(text, sizeof(text), locstr("Forget \"%s\""),
                 app_configuration->known_pads[pane->pad_cursor]);
    } else {
        snprintf(text, sizeof(text), "%s", locstr("Forget controller"));
    }
    lv_label_set_text(pane->forget_label, text);
}

static void pad_item_clicked_cb(lv_event_t *e);

static void kbm_toggle_cb(lv_event_t *e) {
    coop_pane_t *pane = lv_event_get_user_data(e);
    pane->pad_held = false;
    pads_refresh(pane);
}

/**
 * Rebuild the column contents from the settings. Cheap enough to do wholesale -- at most
 * two dozen labels -- and rebuilding removes any chance of the drawn list and the stored
 * assignment drifting apart, which is the failure that made the old screen unreadable.
 */
static void pads_refresh(coop_pane_t *pane) {
    for (int i = 0; i < pane->pad_count; ++i) {
        if (pane->pad_item[i] != NULL) {
            lv_obj_del(pane->pad_item[i]);
            pane->pad_item[i] = NULL;
        }
    }
    pane->pad_count = (int) app_configuration->known_pads_count;
    if (pane->pad_count > KNOWN_PADS_MAX) {
        pane->pad_count = KNOWN_PADS_MAX;
    }
    if (app_configuration->coop_kbm_assign) {
        pane->pad_count += 2;
    }
    if (pane->pad_cursor >= pane->pad_count) {
        pane->pad_cursor = pane->pad_count > 0 ? pane->pad_count - 1 : 0;
    }
    for (int i = 0; i < pane->pad_count; ++i) {
        const char *name = board_name(i);
        const int col = pad_column_of(pane, name);
        lv_obj_t *item = lv_label_create(pane->pad_col[col]);
        lv_label_set_long_mode(item, LV_LABEL_LONG_DOT);
        lv_obj_set_width(item, LV_PCT(100));
        lv_obj_set_style_text_font(item, lv_theme_get_font_small(item), 0);
        lv_obj_set_style_pad_all(item, LV_DPX(2), 0);
        lv_obj_set_style_radius(item, LV_DPX(3), 0);
        lv_label_set_text(item, board_label(name));
        /* Clickable so a pointer can move a controller without the pick-up step, and
         * marked so the pointer highlights the CONTROLLER rather than walking up to the
         * box and outlining the whole thing. */
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        ui_hover_mark(item);
        lv_obj_add_event_cb(item, pad_item_clicked_cb, LV_EVENT_SHORT_CLICKED, pane);
        pane->pad_item[i] = item;

        const bool on_cursor = i == pane->pad_cursor;
        lv_obj_set_style_bg_opa(item, on_cursor ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(item, pane->pad_held ? lv_palette_main(LV_PALETTE_AMBER)
                                                       : lv_palette_main(LV_PALETTE_BLUE), 0);
        if (on_cursor) {
            lv_obj_set_style_text_color(item, lv_color_black(), 0);
        }
    }
    /* DRAG THE VIEW ALONG WITH THE CURSOR, sideways as well as down: with enough PCs the
     * columns run off the edge, and a cursor sitting in a column nobody can see is the
     * same bug as no cursor at all. Recursive, so the column's own vertical scroll, the
     * row's horizontal scroll and the pane all follow. Done after the rebuild above,
     * because the item it scrolls to has to exist. */
    if (pane->pad_count > 0 && pane->pad_item[pane->pad_cursor] != NULL) {
        lv_obj_scroll_to_view_recursive(pane->pad_item[pane->pad_cursor], LV_ANIM_OFF);
    }
    forget_label_update(pane);
}

/** The pad one row up or down in the cursor's own column, or -1 at the end of it. Read
 *  from the live tree, because a pad moved to another column is re-parented. */
static int pad_step(coop_pane_t *pane, int step) {
    if (pane->pad_count == 0) {
        return -1;
    }
    lv_obj_t *cur = pane->pad_item[pane->pad_cursor];
    lv_obj_t *col = cur != NULL ? lv_obj_get_parent(cur) : NULL;
    if (col == NULL) {
        return -1;
    }
    const int32_t want = (int32_t) lv_obj_get_index(cur) + step;
    if (want < 0 || want >= (int32_t) lv_obj_get_child_cnt(col)) {
        return -1;
    }
    lv_obj_t *next = lv_obj_get_child(col, (uint32_t) want);
    for (int i = 0; i < pane->pad_count; ++i) {
        if (pane->pad_item[i] == next) {
            return i;
        }
    }
    return -1;
}

/** The pad nearest the cursor's row in column @p col, or -1 if that column is empty. */
static int pad_in_column(coop_pane_t *pane, int col) {
    if (col < 0 || col >= pane->col_count) {
        return -1;
    }
    lv_obj_t *box = pane->pad_col[col];
    const uint32_t count = lv_obj_get_child_cnt(box);
    if (count == 0) {
        return -1;
    }
    uint32_t row = 0;
    if (pane->pad_count > 0 && pane->pad_item[pane->pad_cursor] != NULL) {
        row = lv_obj_get_index(pane->pad_item[pane->pad_cursor]);
    }
    if (row >= count) {
        row = count - 1;
    }
    lv_obj_t *target = lv_obj_get_child(box, row);
    for (int i = 0; i < pane->pad_count; ++i) {
        if (pane->pad_item[i] == target) {
            return i;
        }
    }
    return -1;
}

static int pad_cursor_column(coop_pane_t *pane) {
    if (pane->pad_count == 0 || pane->pad_item[pane->pad_cursor] == NULL) {
        return 0;
    }
    lv_obj_t *col = lv_obj_get_parent(pane->pad_item[pane->pad_cursor]);
    for (int c = 0; c < pane->col_count; ++c) {
        if (pane->pad_col[c] == col) {
            return c;
        }
    }
    return 0;
}

static void pad_box_key_cb(lv_event_t *e) {
    coop_pane_t *pane = lv_event_get_user_data(e);
    const uint32_t key = lv_event_get_key(e);
    if (pane->pad_count == 0) {
        return;                          /* nothing to steer; up/down leave as usual */
    }
    if (pane->pad_held) {
        /* Holding a pad: left/right MOVE it between columns. Up and down are
         * deliberately inert so a pad cannot be dropped somewhere unseen. */
        if (key != LV_KEY_LEFT && key != LV_KEY_RIGHT) {
            if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
                lv_event_stop_processing(e);
            }
            return;
        }
        int col = pad_cursor_column(pane) + (key == LV_KEY_RIGHT ? 1 : -1);
        if (col < 0 || col >= pane->col_count) {
            /* At the end of the row: claim the key anyway. While a pad is HELD, a
             * left/right that fell through would carry the focus out of the box and
             * leave the pad picked up in a section nobody is looking at. */
            lv_event_stop_processing(e);
            return;
        }
        pad_assign_column(pane, pane->pad_cursor, col);
        pads_refresh(pane);
        lv_event_stop_processing(e);
        return;
    }
    int next = -1;
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        next = pad_step(pane, key == LV_KEY_DOWN ? 1 : -1);
        if (next < 0) {
            return;                      /* end of the column: let the pane scroll on */
        }
    } else if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
        const int dir = key == LV_KEY_RIGHT ? 1 : -1;
        for (int col = pad_cursor_column(pane) + dir; col >= 0 && col < pane->col_count; col += dir) {
            next = pad_in_column(pane, col);
            if (next >= 0) {
                break;
            }
        }
        if (next < 0) {
            return;
        }
    } else {
        return;
    }
    pane->pad_cursor = next;
    pads_refresh(pane);
    /* STOP PROCESSING, not stop BUBBLING. The settings screen puts its own key handler
     * on every pane child (on_detail_key), and left/right there means "leave this
     * control" for anything that is not a slider -- so a key this box has just acted on
     * would ALSO move the focus off it. Stopping the event is what claims the key;
     * bubbling was never the part that mattered. Keys this box does NOT act on fall
     * through untouched, which is what lets up/down leave at the end of a column. */
    lv_event_stop_processing(e);
}

static void pad_box_clicked_cb(lv_event_t *e) {
    coop_pane_t *pane = lv_event_get_user_data(e);
    if (pane->pad_count == 0) {
        return;
    }
    /* OK picks the controller up, and the next left/right moves it. The pointer never
     * needs this -- it has already aimed at something. */
    lv_indev_t *indev = lv_indev_get_act();
    if (indev != NULL && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
        return;
    }
    pane->pad_held = !pane->pad_held;
    pads_refresh(pane);
}

/** POINTER PATH: clicking a controller sends it to the next column, wrapping back to
 *  Available. With a cursor there is no pick-up step to justify. */
static void pad_item_clicked_cb(lv_event_t *e) {
    coop_pane_t *pane = lv_event_get_user_data(e);
    lv_obj_t *item = lv_event_get_target(e);
    for (int i = 0; i < pane->pad_count; ++i) {
        if (pane->pad_item[i] != item) {
            continue;
        }
        pane->pad_cursor = i;
        pane->pad_held = false;
        const int col = (pad_cursor_column(pane) + 1) % pane->col_count;
        pad_assign_column(pane, i, col);
        pads_refresh(pane);
        return;
    }
}

/**
 * FORGET: the pad leaves the seen list AND every PC that claimed it.
 *
 * Dropping only the seen entry would be worse than doing nothing -- the name would still
 * sit in hosts.ini, so the assignment would come back from the dead the next time that
 * controller was plugged in, with no way to see why.
 */
static void pad_forget_cb(lv_event_t *e) {
    coop_pane_t *pane = lv_event_get_user_data(e);
    if (pane->pad_count == 0 || board_is_kbm(pane->pad_cursor)) {
        return;                          /* the mouse and keyboard are not forgotten, only unticked */
    }
    char name[PADNAME_MAX];
    SDL_strlcpy(name, app_configuration->known_pads[pane->pad_cursor], sizeof(name));
    /* Forgetting a hub pad also clears its live pin. If the hub does not confirm
     * that, keep both the seen entry and its saved column so the UI tells the truth. */
    if (!pad_assign_column(pane, pane->pad_cursor, 0)) {
        pads_refresh(pane);
        return;
    }
    if (settings_forget_pad(app_configuration, name)) {
        settings_save(app_configuration);
    }
    pane->pad_held = false;
    pads_refresh(pane);
}

static void build_controllers(coop_pane_t *pane, lv_obj_t *view) {
    int hosts = 0;
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        if (cur->server != NULL) {
            hosts++;
        }
    }
    pane->col_count = hosts + 1;                       /* + the Available column */
    pane->pad_col = lv_mem_alloc(sizeof(lv_obj_t *) * (size_t) pane->col_count);
    pane->host_uuid = hosts > 0 ? lv_mem_alloc(sizeof(uuidstr_t) * (size_t) hosts) : NULL;
    if (pane->pad_col == NULL || (hosts > 0 && pane->host_uuid == NULL)) {
        pane->col_count = 0;
        return;
    }
    int at = 0;
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL && at < hosts; cur = cur->next) {
        if (cur->server != NULL) {
            pane->host_uuid[at++] = cur->id;
        }
    }

    /* A BUTTON, NOT A PLAIN OBJECT, and that is the whole reason it is reachable: the
     * settings screen adds pane children to the focus group only when their CLASS is
     * group_def, and lv_obj_class is not (lv_obj.c: LV_OBJ_CLASS_GROUP_DEF_FALSE). Built
     * as a plain object this box drew perfectly and the d-pad walked straight past it --
     * up from the Forget button wrapped to the close button instead. Styled flat, so it
     * still reads as a frame around the columns rather than a giant button. */
    pane->pad_box = lv_btn_create(view);
    lv_obj_remove_style_all(pane->pad_box);
    lv_obj_set_width(pane->pad_box, LV_PCT(100));
    lv_obj_set_height(pane->pad_box, LV_SIZE_CONTENT);
    lv_obj_clear_flag(pane->pad_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pane->pad_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_flex_flow(pane->pad_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(pane->pad_box, LV_DPX(4), 0);
    lv_obj_set_style_pad_row(pane->pad_box, LV_DPX(2), 0);
    lv_obj_set_style_outline_width(pane->pad_box, LV_DPX(3), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(pane->pad_box, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_opa(pane->pad_box, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(pane->pad_box, LV_DPX(2), LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(pane->pad_box, pad_box_key_cb, LV_EVENT_KEY, pane);
    lv_obj_add_event_cb(pane->pad_box, pad_box_clicked_cb, LV_EVENT_SHORT_CLICKED, pane);

    /* ONE SCROLLING ROW OF COLUMNS. The headings used to be a separate row above the
     * boxes, which is fine while everything fits and comes apart the moment the row
     * scrolls -- two rows scrolling independently would put every name over the wrong
     * column. Each heading is part of its own column instead, so they cannot drift. */
    pane->pad_body = lv_obj_create(pane->pad_box);
    lv_obj_remove_style_all(pane->pad_body);
    lv_obj_set_width(pane->pad_body, LV_PCT(100));
    lv_obj_set_height(pane->pad_body, LV_SIZE_CONTENT);
    lv_obj_clear_flag(pane->pad_body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(pane->pad_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(pane->pad_body, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(pane->pad_body, LV_SCROLLBAR_MODE_AUTO);
    /* remove_style_all() took the bar's style too, so AUTO drew nothing. */
    lv_theme_moonlight_add_scrollbar(pane->pad_body);
    lv_obj_set_flex_flow(pane->pad_body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pane->pad_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(pane->pad_body, LV_DPX(4), 0);
    lv_obj_set_style_pad_bottom(pane->pad_body, LV_DPX(4), 0);

    for (int c = 0; c < pane->col_count; ++c) {
        /* Share the width while they fit, and stop at a legible minimum after that --
         * min_width is applied by lv_obj_refr_size after flex has handed out the space,
         * so the row simply overflows and scrolls once there is not enough to go round. */
        lv_obj_t *colbox = lv_obj_create(pane->pad_body);
        lv_obj_remove_style_all(colbox);
        lv_obj_clear_flag(colbox, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_grow(colbox, 1);
        lv_obj_set_style_min_width(colbox, COOP_PANE_COL_MIN_W, 0);
        lv_obj_set_height(colbox, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(colbox, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(colbox, LV_DPX(2), 0);

        lv_obj_t *title = lv_label_create(colbox);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title, LV_PCT(100));
        lv_obj_set_style_text_font(title, lv_theme_get_font_small(title), 0);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        if (c == 0) {
            lv_label_set_text(title, locstr("Available"));
        } else {
            const pclist_t *node = pcmanager_node(pcmanager, &pane->host_uuid[c - 1]);
            lv_label_set_text(title, node != NULL && node->server != NULL
                                     ? node->server->hostname : "?");
        }

        lv_obj_t *col = lv_obj_create(colbox);
        lv_obj_remove_style_all(col);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_width(col, LV_PCT(100));
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(col, LV_DPX(40), 0);
        lv_obj_set_style_max_height(col, LV_DPX(150), 0);
        lv_obj_set_scroll_dir(col, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_AUTO);
        lv_theme_moonlight_add_scrollbar(col);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(col, LV_DPX(3), 0);
        lv_obj_set_style_pad_row(col, LV_DPX(2), 0);
        lv_obj_set_style_border_width(col, LV_DPX(1), 0);
        lv_obj_set_style_border_color(col, lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_set_style_radius(col, LV_DPX(4), 0);
        pane->pad_col[c] = col;
    }

    pads_refresh(pane);

    pane->forget_btn = lv_btn_create(view);
    lv_obj_set_width(pane->forget_btn, LV_PCT(100));
    lv_obj_set_height(pane->forget_btn, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(pane->forget_btn, LV_DPX(6), 0);
    pane->forget_label = lv_label_create(pane->forget_btn);
    lv_obj_add_event_cb(pane->forget_btn, pad_forget_cb, LV_EVENT_CLICKED, pane);
    forget_label_update(pane);
    pref_desc_label(view, locstr("OK picks the selected controller up, then left/right moves it "
                                 "between columns; with a pointer, clicking one sends it to the "
                                 "next column. Forgetting a controller drops it from this list "
                                 "and from every PC that had it -- it comes back if it is ever "
                                 "connected again."), false);

    lv_obj_t *kbm = pref_checkbox(view, locstr("Assign mouse and keyboard like controllers"),
                                  &app_configuration->coop_kbm_assign, false);
    lv_obj_add_event_cb(kbm, kbm_toggle_cb, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Off: the mouse and keyboard work on whichever PC the pointer is "
                                 "over. On: Mouse and Keyboard join the controllers above and on "
                                 "the co-op launch screen, and each stays with the PC it is given "
                                 "to for the whole session. When this PC is one of the two, "
                                 "Ctrl+Alt+Shift+Z brings a mouse given to it back to Eclipse."), false);
}

/* ------------------------------------------------------------------- advanced */

/** Does this dial have to show both columns? Either the user asked for the split, or the
 *  two values have already been parted -- in which case hiding one would misdescribe
 *  what is going to run. */
static bool dial_is_split(const dial_view_t *dial) {
    if (app_configuration->coop_split_orientations) {
        return true;
    }
    if (dial->ivals != NULL) {
        return dial->ivals[0] != dial->ivals[1];
    }
    return dial->bvals[0] != dial->bvals[1];
}

/** One column's value as the user should read it: the number, or what 0 stands for. */
static void dial_value_text(const dial_view_t *dial, int o, char *out, size_t len) {
    if (dial->ivals != NULL) {
        const int val = dial->ivals[o];
        if (val == 0 && dial->zero_text != NULL) {
            snprintf(out, len, "%s", dial->zero_text);
        } else if (dial->per_side) {
            /* WHAT EACH PLAYER GETS, which is the only form of this number anybody has a
             * use for. The whole picture is @p val tall and 16:9 wide; a top/bottom split
             * gives each player the full width and half the height, a left/right split
             * gives each the full height and half the width. */
            const int full_w = val * 16 / 9;
            if (o == COOP_ORIENTATION_VERTICAL) {
                snprintf(out, len, "%d x %d each", full_w / 2, val);
            } else {
                snprintf(out, len, "%d x %d each", full_w, val / 2);
            }
        } else {
            snprintf(out, len, "%d", val);
        }
    } else {
        snprintf(out, len, "%s", dial->bvals[o] ? locstr("on") : locstr("off"));
    }
}

static void dial_title_update(const dial_view_t *dial) {
    /* THE TITLE IS THE NAME, NOTHING ELSE. It used to carry the values too -- "Shared
     * frame-rate cap: H free-running / V free-running" -- while each control repeated
     * them a line below, so every dial said the same thing three times and the name was
     * lost in the middle of it. The value belongs with the control it belongs to. */
    lv_label_set_text(dial->title, dial->label);
    for (int i = 0; i < 3; ++i) {
        if (dial->readout[i] == NULL) {
            continue;
        }
        const int o = i == 2 ? COOP_ORIENTATION_VERTICAL : COOP_ORIENTATION_HORIZONTAL;
        char one[48];
        dial_value_text(dial, o, one, sizeof(one));
        lv_label_set_text(dial->readout[i], one);
    }
}

/** Put the widgets back in step with the values -- after a defaults reset, or after the
 *  merged control wrote through to both columns. */
static void dial_controls_sync(const dial_view_t *dial) {
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *ctrl = dial->ctrl[i];
        if (ctrl == NULL) {
            continue;
        }
        const int o = i == 2 ? COOP_ORIENTATION_VERTICAL : COOP_ORIENTATION_HORIZONTAL;
        if (dial->ivals != NULL) {
            lv_slider_set_value(ctrl, dial->ivals[o] / (dial->step > 0 ? dial->step : 1), LV_ANIM_OFF);
        } else if (dial->bvals[o]) {
            lv_obj_add_state(ctrl, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(ctrl, LV_STATE_CHECKED);
        }
    }
}

/**
 * The parts the cursor can sit on, in the order left/right walks them. Depends on the
 * shape: one control and a button when merged, two controls and a button when split.
 *
 * @param boxes  filled with what to outline; @p ctrls with the control each part drives
 *               (NULL for the button).
 * @return number of parts.
 */
static int dial_parts(const dial_view_t *dial, lv_obj_t *boxes[3], lv_obj_t *ctrls[3]) {
    int n = 0;
    if (dial_is_split(dial)) {
        boxes[n] = dial->part_box[1];
        ctrls[n++] = dial->ctrl[1];
        boxes[n] = dial->part_box[2];
        ctrls[n++] = dial->ctrl[2];
    } else {
        boxes[n] = dial->part_box[0];
        ctrls[n++] = dial->ctrl[0];
    }
    if (dial->explain_btn != NULL) {
        boxes[n] = dial->explain_btn;
        ctrls[n++] = NULL;
    }
    return n;
}

/** Draw the cursor: a ring on the part it is on, amber while that part is being edited so
 *  "left and right now change the number" is visible rather than remembered. */
static void dial_cursor_paint(dial_view_t *dial) {
    lv_obj_t *boxes[3], *ctrls[3];
    const int n = dial_parts(dial, boxes, ctrls);
    if (dial->cursor >= n) {
        dial->cursor = n > 0 ? n - 1 : 0;
    }
    const bool focused = lv_obj_has_state(dial->row, LV_STATE_FOCUS_KEY);
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *box = dial->part_box[i];
        if (box == NULL) {
            continue;
        }
        lv_obj_set_style_outline_width(box, 0, 0);
    }
    if (dial->explain_btn != NULL) {
        lv_obj_set_style_outline_width(dial->explain_btn, 0, 0);
    }
    if (!focused || n == 0) {
        return;
    }
    lv_obj_t *at = boxes[dial->cursor];
    if (at == NULL) {
        return;
    }
    lv_obj_set_style_outline_width(at, LV_DPX(3), 0);
    lv_obj_set_style_outline_pad(at, LV_DPX(2), 0);
    lv_obj_set_style_outline_opa(at, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_color(at, dial->editing ? lv_palette_main(LV_PALETTE_AMBER)
                                                     : lv_palette_main(LV_PALETTE_BLUE), 0);
}

static void dial_shape_update(dial_view_t *dial) {
    const bool split = dial_is_split(dial);
    if (split) {
        lv_obj_add_flag(dial->merged_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(dial->split_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(dial->merged_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(dial->split_row, LV_OBJ_FLAG_HIDDEN);
    }
    dial->cursor = 0;
    dial->editing = false;
    dial_title_update(dial);
    dial_cursor_paint(dial);
}

static void dials_refresh(coop_pane_t *pane) {
    for (int i = 0; i < pane->dial_count; ++i) {
        dial_controls_sync(&pane->dials[i]);
        dial_shape_update(&pane->dials[i]);
    }
    if (pane->split_head != NULL) {
        if (app_configuration->coop_split_orientations) {
            lv_obj_clear_flag(pane->split_head, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(pane->split_head, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/** Which control moved: the merged one writes through to BOTH columns, so the two stay
 *  equal until they are deliberately parted. */
typedef struct dial_change_t {
    dial_view_t *dial;
    coop_pane_t *pane;
    bool merged;
} dial_change_t;

static void dial_change_free_cb(lv_event_t *e) {
    lv_mem_free(lv_event_get_user_data(e));
}

static void dial_changed_cb(lv_event_t *e) {
    dial_change_t *ch = lv_event_get_user_data(e);
    if (ch->merged) {
        if (ch->dial->ivals != NULL) {
            ch->dial->ivals[COOP_ORIENTATION_VERTICAL] = ch->dial->ivals[COOP_ORIENTATION_HORIZONTAL];
        } else {
            ch->dial->bvals[COOP_ORIENTATION_VERTICAL] = ch->dial->bvals[COOP_ORIENTATION_HORIZONTAL];
        }
        dial_controls_sync(ch->dial);
    }
    dial_title_update(ch->dial);
    /* Applied the moment it changes rather than on close: these become files, and the
     * next stream reads the files. */
    coop_tunables_apply(app_configuration->coop_orientation);
}

static void dial_hook(lv_obj_t *ctrl, dial_view_t *dial, coop_pane_t *pane, bool merged, bool slider) {
    dial_change_t *ch = lv_mem_alloc(sizeof(dial_change_t));
    if (ch == NULL) {
        return;
    }
    ch->dial = dial;
    ch->pane = pane;
    ch->merged = merged;
    if (slider) {
        /* PRESSING and KEY are the events pref_slider writes the value back on, and this
         * is registered after it -- on VALUE_CHANGED alone the dial would show, and
         * apply, the value from before the keypress. */
        lv_obj_add_event_cb(ctrl, dial_changed_cb, LV_EVENT_PRESSING, ch);
        lv_obj_add_event_cb(ctrl, dial_changed_cb, LV_EVENT_KEY, ch);
    } else {
        lv_obj_add_event_cb(ctrl, dial_changed_cb, LV_EVENT_VALUE_CHANGED, ch);
    }
    lv_obj_add_event_cb(ctrl, dial_change_free_cb, LV_EVENT_DELETE, ch);
}

static lv_obj_t *dial_row(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, LV_DPX(6), 0);
    return row;
}

/** THE LINE BETWEEN THE TWO COLUMNS. Two sliders side by side with a gap read as one
 *  control that happens to be split; a rule between them says they are separate values
 *  for separate modes, which is the entire point of the column. Full height of the row,
 *  so it lines up down the section rather than floating beside each dial. */
static void dial_divider(lv_obj_t *row) {
    lv_obj_t *line = lv_obj_create(row);
    lv_obj_remove_style_all(line);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(line, LV_DPX(2));
    lv_obj_set_height(line, LV_PCT(100));
    lv_obj_set_style_min_height(line, LV_DPX(18), 0);
    lv_obj_set_style_bg_color(line, ml_color_hex(ML_COLOR_BORDER), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
}

/**
 * ONE COLUMN OF A DIAL: its value ABOVE its control.
 *
 * The value used to sit beside the slider, in the row, which put the horizontal column's
 * number BETWEEN the two sliders -- reading as a label for the right-hand one -- and a
 * long value like "free-running" then wrapped to two lines and shoved the whole row
 * about. Stacked, each number is unambiguously over its own control, nothing wraps into
 * anything else, and the two columns stay the same height whatever the values say.
 *
 * @param head  column name when split ("Horizontal"), or NULL for the merged control.
 */
static lv_obj_t *dial_column(lv_obj_t *row, const char *head, lv_obj_t **out_value) {
    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, LV_DPX(2), 0);
    /* ROOM FOR THE KNOB. A slider's knob is drawn CENTRED on the value, so at either end
     * half of it falls outside the track -- and LVGL clips a child to its parent, which
     * cut it in half against this column's edge. Hard to attribute when you see it: the
     * bar looks like it is spilling out of its box rather than being trimmed by it. The
     * inset is half a knob, so both ends stay whole at any value. */
    lv_obj_set_style_pad_hor(col, LV_DPX(9), 0);

    lv_obj_t *line = lv_obj_create(col);
    lv_obj_remove_style_all(line);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(line, LV_PCT(100));
    lv_obj_set_height(line, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name = lv_label_create(line);
    lv_obj_set_style_text_font(name, lv_theme_get_font_small(name), 0);
    lv_obj_set_style_text_opa(name, LV_OPA_60, 0);
    lv_label_set_text(name, head != NULL ? head : "");

    lv_obj_t *value = lv_label_create(line);
    lv_obj_set_style_text_font(value, lv_theme_get_font_small(value), 0);
    lv_obj_set_style_text_color(value, ml_color_hex(ML_COLOR_PRIMARY), 0);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    *out_value = value;
    return col;
}

/**
 * THE ONE-LINE NOTE, AND A WAY TO ASK FOR THE REST.
 *
 * Every setting used to carry its whole justification inline -- measurements, the history
 * of what was tried, the failure it prevents. All of it true and all of it written for
 * someone who had been there. Read by anyone else it is a wall of jargon in front of a
 * control they only wanted a sentence about. So: one plain sentence here, and the full
 * account one button away, written for someone who has never seen this project.
 */
static void detail_free_cb(lv_event_t *e) {
    lv_mem_free(lv_event_get_user_data(e));
}

static void detail_open_cb(lv_event_t *e) {
    const detail_ref_t *ref = lv_event_get_user_data(e);
    static const char *btns[] = {translatable("Close"), ""};
    lv_obj_t *box = lv_msgbox_create_i18n(NULL, ref->title, ref->body, btns, false);
    /* USER_4 tells the theme this dialog handles its own keys; without it the theme's
     * msgbox handler fights the one the settings screen already installed. */
    lv_obj_set_width(box, LV_PCT(74));
    lv_obj_set_style_max_height(box, LV_PCT(80), 0);
    lv_obj_center(box);
}

/** The short note, with the button that opens the long one beside it. */
static void dial_explain(dial_view_t *dial, lv_obj_t *parent, const char *title,
                         const char *brief, const char *detail) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, LV_DPX(8), 0);
    lv_obj_set_style_pad_bottom(row, LV_DPX(6), 0);

    lv_obj_t *note = lv_label_create(row);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(note, 1);
    lv_obj_set_style_pad_left(note, LV_DPX(16), 0);
    lv_obj_set_style_text_font(note, lv_theme_get_font_small(note), 0);
    lv_obj_set_style_text_opa(note, LV_OPA_60, 0);
    lv_label_set_text(note, brief);

    if (detail == NULL) {
        return;
    }
    detail_ref_t *ref = lv_mem_alloc(sizeof(detail_ref_t));
    if (ref == NULL) {
        return;
    }
    ref->title = title;
    ref->body = detail;
    lv_obj_t *btn = lv_btn_create(row);
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(btn, LV_DPX(6), 0);
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_obj_set_style_text_font(btn_label, lv_theme_get_font_small(btn_label), 0);
    lv_label_set_text(btn_label, locstr("Detailed explanation"));
    lv_obj_add_event_cb(btn, detail_open_cb, LV_EVENT_CLICKED, ref);
    lv_obj_add_event_cb(btn, detail_free_cb, LV_EVENT_DELETE, ref);
    /* Reached with right, not with down: the setting's row owns the focus. */
    settings_navigation_exclude(btn);
    if (dial != NULL) {
        dial->explain_btn = btn;
    }
}

/** Nudge the value the cursor is on, while editing. Drives the widget directly rather
 *  than letting pref_slider handle keys: the slider is deliberately NOT in the focus
 *  group any more, so it never sees a key of its own. */
static void dial_edit_step(dial_view_t *dial, int dir) {
    lv_obj_t *boxes[3], *ctrls[3];
    dial_parts(dial, boxes, ctrls);
    lv_obj_t *ctrl = ctrls[dial->cursor];
    if (ctrl == NULL || dial->ivals == NULL) {
        return;
    }
    const int o = (dial_is_split(dial) && dial->cursor == 1) ? COOP_ORIENTATION_VERTICAL
                                                            : COOP_ORIENTATION_HORIZONTAL;
    int value = dial->ivals[o] + dir * (dial->step > 0 ? dial->step : 1);
    if (value < dial->min) {
        value = dial->min;
    }
    if (value > dial->max) {
        value = dial->max;
    }
    dial->ivals[o] = value;
    if (!dial_is_split(dial)) {
        /* Merged: the two stay equal until they are deliberately parted. */
        dial->ivals[COOP_ORIENTATION_VERTICAL] = dial->ivals[COOP_ORIENTATION_HORIZONTAL];
    }
    dial_controls_sync(dial);
    dial_title_update(dial);
    coop_tunables_apply(app_configuration->coop_orientation);
}

/** Flip the switch the cursor is on. */
static void dial_toggle(dial_view_t *dial) {
    if (dial->bvals == NULL) {
        return;
    }
    const int o = (dial_is_split(dial) && dial->cursor == 1) ? COOP_ORIENTATION_VERTICAL
                                                            : COOP_ORIENTATION_HORIZONTAL;
    dial->bvals[o] = !dial->bvals[o];
    if (!dial_is_split(dial)) {
        dial->bvals[COOP_ORIENTATION_VERTICAL] = dial->bvals[COOP_ORIENTATION_HORIZONTAL];
    }
    dial_controls_sync(dial);
    dial_title_update(dial);
    coop_tunables_apply(app_configuration->coop_orientation);
}

static void dial_row_key_cb(lv_event_t *e) {
    dial_view_t *dial = lv_event_get_user_data(e);
    const uint32_t key = lv_event_get_key(e);
    lv_obj_t *boxes[3], *ctrls[3];
    const int n = dial_parts(dial, boxes, ctrls);

    if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
        const int dir = key == LV_KEY_RIGHT ? 1 : -1;
        if (dial->editing) {
            dial_edit_step(dial, dir);
        } else {
            const int next = dial->cursor + dir;
            if (next < 0 || next >= n) {
                /* Ends of the row: claim the key anyway. Falling through would hand the
                 * focus to the settings screen's own left/right, which means "leave this
                 * control" -- and leaving sideways from the middle of a setting is not
                 * something anyone asked for. */
                lv_event_stop_processing(e);
                return;
            }
            dial->cursor = next;
            dial_cursor_paint(dial);
        }
        lv_event_stop_processing(e);
        return;
    }
    if (key == LV_KEY_ESC && dial->editing) {
        dial->editing = false;
        dial_cursor_paint(dial);
        lv_event_stop_processing(e);
        return;
    }
    /* UP and DOWN are deliberately untouched, even while editing: they are how the list
     * is walked, and a setting that swallowed them would strand the focus on itself. */
}

static void dial_row_click_cb(lv_event_t *e) {
    dial_view_t *dial = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (indev != NULL && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
        return;                 /* a pointer aims at the control itself */
    }
    lv_obj_t *boxes[3], *ctrls[3];
    dial_parts(dial, boxes, ctrls);
    if (ctrls[dial->cursor] == NULL) {
        lv_event_send(dial->explain_btn, LV_EVENT_CLICKED, NULL);
        return;
    }
    if (dial->bvals != NULL) {
        dial_toggle(dial);
        return;
    }
    dial->editing = !dial->editing;
    dial_cursor_paint(dial);
}

static void dial_row_focus_cb(lv_event_t *e) {
    dial_view_t *dial = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_DEFOCUSED) {
        dial->editing = false;
    }
    dial_cursor_paint(dial);
}

/**
 * KEEP THE KNOB INSIDE THE BAR.
 *
 * LVGL centres the knob ON the value (lv_slider.c: position_knob subtracts half the knob
 * width), so at either end of the travel half of it lies outside the slider's own box and
 * is trimmed -- which on screen reads as the bar spilling out of its space rather than
 * being cut. Padding the PARENT does not help, because the knob leaves the slider itself.
 * Padding the slider's MAIN part does: lv_bar insets the indicator by it and the knob
 * travels within that, so both ends stay whole at every value.
 */
static void dial_slider_inset(lv_obj_t *slider) {
    lv_obj_set_style_pad_hor(slider, LV_DPX(7), LV_PART_MAIN);
}

static dial_view_t *dial_begin(coop_pane_t *pane, lv_obj_t *parent, const char *label) {
    if (pane->dial_count >= COOP_PANE_MAX_DIALS) {
        return NULL;
    }
    dial_view_t *dial = &pane->dials[pane->dial_count++];
    SDL_memset(dial, 0, sizeof(*dial));
    dial->label = label;
    /* A BUTTON, so the settings screen adds it to the focus group -- a plain container is
     * not group_def and would be walked straight past. Styled flat: it is a region, not
     * something to press. */
    dial->row = lv_btn_create(parent);
    lv_obj_remove_style_all(dial->row);
    lv_obj_set_width(dial->row, LV_PCT(100));
    lv_obj_set_height(dial->row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(dial->row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dial->row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_flex_flow(dial->row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(dial->row, LV_DPX(4), 0);
    lv_obj_set_style_pad_row(dial->row, LV_DPX(2), 0);
    lv_obj_set_style_radius(dial->row, LV_DPX(6), 0);
    lv_obj_set_style_bg_opa(dial->row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(dial->row, ml_color_hex(ML_COLOR_SURFACE_HI), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_opa(dial->row, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(dial->row, dial_row_key_cb, LV_EVENT_KEY, dial);
    lv_obj_add_event_cb(dial->row, dial_row_click_cb, LV_EVENT_SHORT_CLICKED, dial);
    lv_obj_add_event_cb(dial->row, dial_row_focus_cb, LV_EVENT_FOCUSED, dial);
    lv_obj_add_event_cb(dial->row, dial_row_focus_cb, LV_EVENT_DEFOCUSED, dial);

    dial->title = pref_title_label(dial->row, label);
    return dial;
}

static void adv_int(coop_pane_t *pane, lv_obj_t *parent, const char *label, const char *brief,
                    const char *detail, int values[2], int min, int max, int step,
                    const char *zero_text, bool per_side) {
    dial_view_t *dial = dial_begin(pane, parent, label);
    if (dial == NULL) {
        return;
    }
    dial->ivals = values;
    dial->zero_text = zero_text;
    dial->min = min;
    dial->max = max;
    dial->step = step;
    dial->per_side = per_side;

    dial->merged_row = dial_row(dial->row);
    {
        lv_obj_t *col = dial_column(dial->merged_row, NULL, &dial->readout[0]);
        dial->part_box[0] = col;
        dial->ctrl[0] = pref_slider(col, &values[COOP_ORIENTATION_HORIZONTAL], min, max, step);
        lv_obj_set_width(dial->ctrl[0], LV_PCT(100));
        dial_slider_inset(dial->ctrl[0]);
        settings_navigation_exclude(dial->ctrl[0]);
        dial_hook(dial->ctrl[0], dial, pane, true, true);
    }

    dial->split_row = dial_row(dial->row);
    static const char *col_names[2] = {translatable("Horizontal"), translatable("Vertical")};
    for (int i = 0; i < 2; ++i) {
        const int o = i == 0 ? COOP_ORIENTATION_HORIZONTAL : COOP_ORIENTATION_VERTICAL;
        if (i == 1) {
            dial_divider(dial->split_row);
        }
        lv_obj_t *col = dial_column(dial->split_row, locstr(col_names[i]), &dial->readout[1 + i]);
        dial->part_box[1 + i] = col;
        dial->ctrl[1 + i] = pref_slider(col, &values[o], min, max, step);
        lv_obj_set_width(dial->ctrl[1 + i], LV_PCT(100));
        dial_slider_inset(dial->ctrl[1 + i]);
        /* OUT OF THE FOCUS GROUP. The setting's own row is the stop; a slider that stayed
         * in the group would put itself back in the down/up path, which is the whole
         * problem this replaced. It still works under a pointer -- clicking does not need
         * group membership. */
        settings_navigation_exclude(dial->ctrl[1 + i]);
        dial_hook(dial->ctrl[1 + i], dial, pane, false, true);
    }
    dial_explain(dial, dial->row, label, brief, detail);
    dial_shape_update(dial);
}

static void adv_bool(coop_pane_t *pane, lv_obj_t *parent, const char *label, const char *brief,
                     const char *detail, bool values[2]) {
    dial_view_t *dial = dial_begin(pane, parent, label);
    if (dial == NULL) {
        return;
    }
    dial->bvals = values;

    /* A CHECKBOX SAYS ITS OWN STATE, so it needs no value line above it -- just the
     * column name when the two are split, and nothing at all when they are one. */
    dial->merged_row = dial_row(dial->row);
    dial->ctrl[0] = pref_checkbox(dial->merged_row, locstr("Enabled"),
                                  &values[COOP_ORIENTATION_HORIZONTAL], false);
    lv_obj_set_flex_grow(dial->ctrl[0], 1);
    dial->part_box[0] = dial->ctrl[0];
    settings_navigation_exclude(dial->ctrl[0]);
    dial_hook(dial->ctrl[0], dial, pane, true, false);

    dial->split_row = dial_row(dial->row);
    static const char *col_names[2] = {translatable("Horizontal"), translatable("Vertical")};
    for (int i = 0; i < 2; ++i) {
        const int o = i == 0 ? COOP_ORIENTATION_HORIZONTAL : COOP_ORIENTATION_VERTICAL;
        if (i == 1) {
            dial_divider(dial->split_row);
        }
        dial->ctrl[1 + i] = pref_checkbox(dial->split_row, locstr(col_names[i]), &values[o], false);
        lv_obj_set_flex_grow(dial->ctrl[1 + i], 1);
        dial->part_box[1 + i] = dial->ctrl[1 + i];
        settings_navigation_exclude(dial->ctrl[1 + i]);
        dial_hook(dial->ctrl[1 + i], dial, pane, false, false);
    }
    dial_explain(dial, dial->row, label, brief, detail);
    dial_shape_update(dial);
}

static void split_toggle_cb(lv_event_t *e) {
    coop_pane_t *pane = lv_event_get_user_data(e);
    dials_refresh(pane);
}

static void defaults_cb(lv_event_t *e) {
    coop_pane_t *pane = lv_event_get_user_data(e);
    settings_coop_advanced_defaults(app_configuration);
    dials_refresh(pane);
    coop_tunables_apply(app_configuration->coop_orientation);
    settings_save(app_configuration);
}

static void build_advanced(coop_pane_t *pane, lv_obj_t *view) {
    lv_obj_t *adv = pref_section(view, locstr("Advanced"), false);

    pref_desc_label(adv, locstr("Everything co-op was tuned with. These are written as the same "
                                "/tmp switches the streaming code already reads, so they apply "
                                "from the next stream -- and a test script that writes those "
                                "switches still overrides what is set here."), false);

    /* THE THINGS THAT ACTUALLY BREAK CO-OP, first, because this is the section where
     * someone is about to change one of them. On the main page it sat under settings
     * most people never need and read as background; here it is the warning you get
     * before touching a dial. Every one of these cost a debugging session at least once. */
    pref_header(adv, locstr("Known ways to break co-op"));
    /* FOCUSABLE ON PURPOSE. A plain note is not a focus stop, so walking down the list
     * jumped from the control above it to the control below and the pane scrolled past
     * this without ever resting on it -- the one block here that is a warning rather than
     * a setting was the one block nobody ever saw. */
    pref_desc_label(adv, locstr(
            "Host frame rate floors must be LOW and EQUAL (30/30). Umbra treats the floor as a "
            "maximum frame time and encodes a DUPLICATE frame when the game has not produced one, "
            "so a high floor buys delivered-rate counters with latency. Unequal floors also "
            "manufacture oversupply on one side.\n\n"
            "NVIDIA hosts need constrained encoding. Umbra turns it on for co-op streams by itself "
            "while \"Co-op tuning on the host PCs\" is ticked; without it every composed picture "
            "carries a corrupt band across the seam.\n\n"
            "A throughput counter is not how it feels. Frames delivered, errors=0 and a climbing "
            "ok= count have all been true while the panel was black."), true);

    lv_obj_t *split = pref_checkbox(adv, locstr("Separate horizontal and vertical"),
                                    &app_configuration->coop_split_orientations, false);
    lv_obj_add_event_cb(split, split_toggle_cb, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(adv, locstr("Each dial keeps a value per orientation. While they agree you "
                                "get one control; tick this to set them apart. A dial that has "
                                "already been parted shows both columns whether this is ticked "
                                "or not."), false);

    /* The column headings, shown while the split is on. Each dial's own title also names
     * its two values, so a dial scrolled away from this row still reads correctly. */
    pane->split_head = dial_row(adv);
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *head = lv_label_create(pane->split_head);
        lv_label_set_text_static(head, i == 0 ? locstr("Horizontal") : locstr("Vertical"));
        lv_obj_set_flex_grow(head, 1);
        lv_obj_set_style_text_align(head, LV_TEXT_ALIGN_CENTER, 0);
    }

    /* --- picture ------------------------------------------------------------- */
    pref_header(adv, locstr("Picture"));
    adv_int(pane, adv, locstr("Picture quality (kbps)"),
            locstr("How much data each PC uses to send its half of the picture."),
            locstr("Video is compressed before it is sent over your network, and this number is "
                   "how much data per second each PC is allowed to use. Higher numbers keep more "
                   "detail, especially in fast movement, but need a network that can carry them; "
                   "if your network cannot keep up you get stutter and blocky patches instead of "
                   "a better picture.\n\n"
                   "In split-screen both PCs send at this rate at the same time, so the total on "
                   "your network is double the number set here.\n\n"
                   "Left unset, each PC sends HALF the bitrate in Video settings: each sends half "
                   "the picture, so the two together use the same total as a normal stream at "
                   "the same quality. That is usually what you want; set a number here only to "
                   "give co-op more or less than that."),
            app_configuration->coop_bitrate, 0, 150000, 5000, locstr("half of video setting"), false);
    adv_int(pane, adv, locstr("Frames per second"),
            locstr("The most frames per second each PC is asked to send."),
            locstr("Frames per second is how many complete pictures each PC sends every second. "
                   "More is smoother.\n\n"
                   "This is a ceiling, not a target: each PC sends as many frames as it can "
                   "manage up to this number, and the two are deliberately NOT tied together. If "
                   "one PC is having a hard time, only that player's half slows down -- the "
                   "other half keeps running at full speed.\n\n"
                   "Left unset, split-screen uses the frame rate from Video settings."),
            app_configuration->coop_fps, 0, 240, 5, locstr("video setting"), false);
    adv_int(pane, adv, locstr("Each player's picture size"),
            locstr("How many pixels each player's half of the screen is."),
            locstr("In split-screen the display shows two pictures at once, one from each PC, and each "
                   "PC only draws its own half. This setting is the size of that half.\n\n"
                   "The shape depends on how the screen is split. Top and bottom gives each "
                   "player a wide, short picture -- the full width of the display and half its height. "
                   "Side by side gives each player a tall, narrow one -- half the width and the "
                   "full height. That is why the same setting reads differently for the two "
                   "splits, and why each can be set separately.\n\n"
                   "Bigger looks sharper but asks much more of both PCs, and the two splits do "
                   "not cost the same: a wide, short picture is harder work for the graphics "
                   "card, a tall, narrow one is harder for the processor. If one player's half "
                   "is stuttering, this is the first thing to bring down.\n\n"
                   "Left unset, it follows the resolution in Video settings."),
            app_configuration->coop_height, 0, 2160, 360, locstr("video setting"), true);

    /* --- rate ---------------------------------------------------------------- */
    pref_header(adv, locstr("Speed"));
    adv_int(pane, adv, locstr("Force both PCs to the same speed"),
            locstr("Normally off, so each PC runs as fast as it can."),
            locstr("Normally the two PCs are independent: each sends frames as fast as it "
                   "manages, and if one is slower it is only that player's half that slows down. "
                   "That is how it is meant to run.\n\n"
                   "Turning this on forces both PCs to one fixed speed, which is how this used to "
                   "work before the halves were made independent. It is kept so the old behaviour "
                   "can be compared against the new one, not because it is better -- it makes "
                   "both halves as slow as the slower PC.\n\n"
                   "Leave it off unless you are deliberately testing that comparison."),
            app_configuration->coop_fps_cap, 0, 240, 5, locstr("off"), false);
    adv_int(pane, adv, locstr("Second PC's speed when forced"),
            locstr("Only used when the setting above is on. The second PC is deliberately a "
                   "little slower."),
            locstr("This only does anything when \"force both PCs to the same speed\" is on.\n\n"
                   "When the two are locked together, the second PC must run slightly SLOWER "
                   "than the first, never faster. The client keeps a short queue of the second PC's "
                   "frames while it waits to combine them with the first PC's. If the second PC "
                   "runs fast, that queue fills up and old frames have to be thrown away -- and "
                   "throwing one away damages the picture for a while afterwards, because each "
                   "frame is built partly from the one before it. If it runs slightly slow the "
                   "queue simply runs dry now and then, and the client shows that player's last "
                   "frame for a moment longer, which nobody notices.\n\n"
                   "Measured here: with the second PC running about 3% fast, roughly nine frames "
                   "a second were being thrown away and that player's half showed about a third "
                   "more smearing than the other, on identical content.\n\n"
                   "Left unset it runs 5 frames per second below the shared speed, which is the "
                   "safe direction."),
            app_configuration->coop_fps_peer, 0, 240, 5, locstr("5 below"), false);

    /* --- composition --------------------------------------------------------- */
    pref_header(adv, locstr("How the halves are joined"));
    adv_int(pane, adv, locstr("Pieces per frame"),
            locstr("How each PC divides up its picture before sending. Leave at 1."),
            locstr("Each PC can cut its picture into pieces before sending it. The client takes one "
                   "piece from each PC and joins them into the single picture you see.\n\n"
                   "1 is correct: each PC sends its half as one piece, and the client joins the two. "
                   "This is the arrangement everything else here was tested with.\n\n"
                   "2 exists only for an older design that has been replaced, where each PC drew "
                   "the WHOLE screen and cut it in two so the client could take the half it wanted. "
                   "Setting it to 2 switches back to that, which wastes both PCs' effort drawing "
                   "picture nobody sees.\n\n"
                   "If this is left unset the app uses the value meant for ordinary single-PC "
                   "streaming, which is the wrong shape for joining two pictures together."),
            app_configuration->coop_slices, 0, 8, 1, locstr("wrong for co-op"), false);
    adv_int(pane, adv, locstr("Frames held from the second PC"),
            locstr("How many of the second PC's frames the client keeps while waiting to pair them up."),
            locstr("The two PCs do not send frames in step with each other, so the client keeps a "
                   "short queue of the second PC's frames and takes one each time it builds a "
                   "combined picture.\n\n"
                   "Too few and the queue cannot hold a burst: when the second PC gets ahead the "
                   "client has nowhere to put the extra frames and has to throw them away, which "
                   "damages that half of the picture for a moment.\n\n"
                   "Too many and the queue becomes a delay -- the frame the client is showing from "
                   "that PC is older than the one it is showing from the other, so that player's "
                   "half feels behind.\n\n"
                   "The built-in value has been tested and there is no known reason to change "
                   "it."),
            app_configuration->coop_queue_depth, 0, 16, 1, locstr("built-in"), false);

    /* --- concealment --------------------------------------------------------- */
    pref_header(adv, locstr("When a PC misses a frame"));
    adv_bool(pane, adv, locstr("Hold the last picture"),
             locstr("If one PC has nothing new to send, keep showing what it sent last. Leave on."),
             locstr("The two PCs do not produce frames at the same moments, so quite often the client "
                    "is ready to draw a combined picture and one of them has nothing new.\n\n"
                    "With this on, that player's half simply keeps showing its last picture until "
                    "a new one arrives. That is exactly right: if nothing on that PC has changed "
                    "-- a paused game, a still menu -- the picture SHOULD be identical. You only "
                    "notice a hold if it lasts long enough to look like a freeze.\n\n"
                    "With it off the client has to fall back to a cruder repair that reuses the "
                    "previous picture in a way it was not built for, and the result smears on "
                    "anything that is moving while still areas stay clean. Measured here: that "
                    "fallback happened hundreds of times a minute where the correct behaviour "
                    "happened a couple of dozen times in a whole session.\n\n"
                    "Turn it off only to see that difference for yourself."),
             app_configuration->coop_skip_conceal);
    adv_bool(pane, adv, locstr("Keep up with the faster PC"),
             locstr("Lets the picture update when the second PC is running faster than the first. "
                    "Leave on."),
             locstr("The client builds each combined picture when the FIRST PC sends a frame. If the "
                    "second PC is running faster, its frames pile up faster than they are being "
                    "used.\n\n"
                    "With this on, the client also builds a picture when the second PC gets ahead, so "
                    "the screen updates at whichever PC is faster rather than being held back to "
                    "the slower one, and that queue of waiting frames drains instead of growing.\n\n"
                    "With it off the queue sits full, and everything in it is old: measured here, "
                    "a full queue was about a third of a second of delay on that player's half, "
                    "which was reported simply as \"really high latency\"."),
             app_configuration->coop_catchup);
    /* BOTH OF THESE DO NOTHING IN EITHER SUPPORTED MODE, and saying so is the point of
     * listing them: their call sites both require `mine.slice_count == 2`, the retired
     * full-height design where each host encoded the whole frame in two slices. With
     * pieces-per-frame at 1 -- the measured configuration -- neither is ever reached. */
    pref_header(adv, locstr("Old design (does nothing unless pieces per frame is 2)"));
    adv_bool(pane, adv, locstr("Old repair method"),
             locstr("Part of a replaced design. Has no effect with the normal settings."),
             locstr("This belongs to the older arrangement where each PC drew the whole screen "
                    "and cut it in two. It repairs a missing half by sending only the top piece "
                    "and letting the decoder fill in the rest from the previous picture.\n\n"
                    "It has been replaced by \"hold the last picture\", which is both more exact "
                    "and better tested. It is left here so the old behaviour can still be "
                    "compared.\n\n"
                    "With \"pieces per frame\" at 1 -- the normal setting -- this does nothing at "
                    "all, because there is no second piece for it to work with."),
             app_configuration->coop_repeat_conceal);
    adv_bool(pane, adv, locstr("Old repeat method"),
             locstr("Part of a replaced design. Has no effect with the normal settings."),
             locstr("Another repair from the older arrangement: instead of holding the second "
                    "PC's half, it re-sends that PC's previous picture with its numbering "
                    "adjusted so the client accepts it.\n\n"
                    "The drawback is that any movement in that picture gets applied a second "
                    "time, so things drift slightly out of place.\n\n"
                    "Like the setting above, it does nothing while \"pieces per frame\" is 1."),
             app_configuration->coop_repeat_pane);

    /* --- audio --------------------------------------------------------------- */
    pref_header(adv, locstr("Sound"));
    adv_int(pane, adv, locstr("Second PC's sound buffer (ms)"),
            locstr("How much of the second PC's sound is stored before playing. Bigger is "
                   "steadier, smaller is less delayed."),
            locstr("Both PCs' sound is mixed together and played through the selected audio "
                   "output. The second PC's "
                   "sound arrives over the network, so a small amount of it is stored up before "
                   "playing, to cover the moments when the network is uneven.\n\n"
                   "A bigger buffer copes better with an unsteady network but delays that "
                   "player's sound slightly. A smaller one is more immediate but can crackle or "
                   "drop out if anything hiccups.\n\n"
                   "The number is in milliseconds -- thousandths of a second. The built-in value "
                   "is 40, about a twenty-fifth of a second."),
            app_configuration->coop_audio_depth_ms, 0, 400, 5, locstr("built-in (40)"), false);

    /* --- diagnostics --------------------------------------------------------- */
    pref_header(adv, locstr("Testing"));
    adv_bool(pane, adv, locstr("Test split-screen with one PC"),
             locstr("Shows this PC's screen in both halves, so split-screen can be tried without "
                    "a second PC."),
             locstr("Split-screen normally needs two PCs streaming at once. With this on, the "
                    "second half is filled with a live copy of the first PC's screen instead, so "
                    "you can see the split working, check the join down the middle and try the "
                    "settings without setting up a second machine.\n\n"
                    "It is a genuine live picture, not a recording -- which matters, because a "
                    "recording would look perfectly normal while telling you nothing about what "
                    "the PC is actually doing right now.\n\n"
                    "This applies to the top-and-bottom split. The side-by-side split does the "
                    "same thing on its own when only one PC is streaming, so it needs no setting."),
             app_configuration->coop_self_mirror);
    adv_bool(pane, adv, locstr("Log frame timing"),
             locstr("Writes timing information to the log file. Useful only when investigating a "
                    "problem."),
             locstr("Records how evenly frames arrive from each PC and writes it to the app's log "
                    "file.\n\n"
                    "It changes nothing about the picture. It is there for working out WHY "
                    "something stutters -- whether frames are arriving unevenly, and from which "
                    "PC. Leave it off unless you are chasing a problem, since it adds writing to "
                    "the log for no benefit otherwise."),
             app_configuration->coop_measure);
    adv_bool(pane, adv, locstr("Developer event pump"),
             locstr("Leave this off. It exists for one unfinished experiment and can interfere "
                    "with video playback."),
             locstr("This switches on a background helper that was added for an experiment with a "
                    "different video path, which needed a particular internal message "
                    "to be delivered.\n\n"
                    "It works by draining a queue of messages that other parts of the client "
                    "also rely on, so it can take messages that were meant for something else. "
                    "The code itself says it must not run in normal use.\n\n"
                    "There is no reason to turn this on unless you are working on that experiment."),
             app_configuration->coop_pump);

    lv_obj_t *defaults = lv_btn_create(adv);
    lv_obj_set_width(defaults, LV_PCT(100));
    lv_obj_set_height(defaults, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(defaults, LV_DPX(6), 0);
    lv_obj_t *defaults_label = lv_label_create(defaults);
    lv_label_set_text(defaults_label, locstr("Restore advanced defaults"));
    lv_obj_add_event_cb(defaults, defaults_cb, LV_EVENT_CLICKED, pane);
    pref_desc_label(adv, locstr("Puts every dial above back to the shipped value, both "
                                "orientations. Nothing else on this screen is touched."), false);
}

/* ----------------------------------------------------------------------- pane */

static void orientation_changed_cb(lv_event_t *e) {
    (void) e;
    /* The dials are per orientation and only the current one is written out, so the
     * switches on disk have to be re-made the moment the orientation changes. */
    coop_tunables_apply(app_configuration->coop_orientation);
}

/**
 * Build the machine list the two "default computer" dropdowns share.
 *
 * FIRST ENTRY IS "LAST USED", AND IT IS THE DEFAULT. Someone with three PCs paired does
 * not necessarily want a fixed pair -- the previous behaviour (open on whatever ran last)
 * is a perfectly good answer and is what everyone has today. An empty uuid means exactly
 * that, so leaving these alone changes nothing anywhere.
 *
 * Names are COPIED. The dropdown holds the pointers for as long as the pane lives, while
 * SERVER_DATA is explicitly documented as something not to hold a reference to
 * (backend/types.h) -- it is replaced whenever a host updates.
 */
static void build_pc_entries(coop_pane_t *pane) {
    int hosts = 0;
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        if (cur->server != NULL) {
            hosts++;
        }
    }
    const size_t slots = (size_t) hosts + 1;               /* + "last used" */
    pane->pc_entries = lv_mem_alloc(sizeof(pref_dropdown_string_entry_t) * slots);
    pane->pc_entry_uuid = lv_mem_alloc(sizeof(*pane->pc_entry_uuid) * slots);
    pane->pc_entry_name = lv_mem_alloc(sizeof(char *) * slots);
    if (pane->pc_entries == NULL || pane->pc_entry_uuid == NULL || pane->pc_entry_name == NULL) {
        pane->pc_entries_len = 0;
        return;
    }
    lv_memset_00(pane->pc_entries, sizeof(pref_dropdown_string_entry_t) * slots);
    lv_memset_00(pane->pc_entry_uuid, sizeof(*pane->pc_entry_uuid) * slots);

    pane->pc_entry_name[0] = lv_mem_alloc(strlen(locstr("Whatever ran last")) + 1);
    strcpy(pane->pc_entry_name[0], locstr("Whatever ran last"));
    pane->pc_entries[0].name = pane->pc_entry_name[0];
    pane->pc_entries[0].value = pane->pc_entry_uuid[0];     /* "" */
    pane->pc_entries[0].fallback = true;                    /* anything unrecognised lands here */
    pane->pc_entries_len = 1;

    for (const pclist_t *cur = pcmanager_servers(pcmanager);
         cur != NULL && (size_t) pane->pc_entries_len < slots; cur = cur->next) {
        if (cur->server == NULL) {
            continue;
        }
        const int at = pane->pc_entries_len;
        const char *name = cur->server->hostname != NULL ? cur->server->hostname : "?";
        pane->pc_entry_name[at] = lv_mem_alloc(strlen(name) + 1);
        if (pane->pc_entry_name[at] == NULL) {
            break;
        }
        strcpy(pane->pc_entry_name[at], name);
        /* Copied out of the struct rather than through uuidstr_tostr(), which ALLOCATES
         * a string its callers here would have to free -- and none of the ones nearby
         * do. Fixed width, so this cannot truncate. */
        SDL_memcpy(pane->pc_entry_uuid[at], cur->id.data, UUIDSTR_LENGTH);
        pane->pc_entry_uuid[at][UUIDSTR_LENGTH] = '\0';
        pane->pc_entries[at].name = pane->pc_entry_name[at];
        pane->pc_entries[at].value = pane->pc_entry_uuid[at];
        pane->pc_entries_len++;
    }
}

/** One dropdown's host. Freed with the widget, like dial_change_t above. */
typedef struct gpu_row_t {
    uuidstr_t uuid;
} gpu_row_t;

static void gpu_row_free_cb(lv_event_t *e) {
    lv_mem_free(lv_event_get_user_data(e));
}

static void gpu_row_changed_cb(lv_event_t *e) {
    const gpu_row_t *row = lv_event_get_user_data(e);
    const uint16_t sel = lv_dropdown_get_selected(lv_event_get_current_target(e));
    /* Index order must match the option string below. "Not set" first, so a list that has
     * never been touched reads as a question rather than as a claim about the hardware. */
    static const coop_gpu_vendor_t by_index[] = {COOP_GPU_UNKNOWN, COOP_GPU_NVIDIA, COOP_GPU_AMD,
                                                 COOP_GPU_INTEL, COOP_GPU_OTHER};
    if (sel < (uint16_t) (sizeof(by_index) / sizeof(by_index[0]))) {
        pcmanager_set_host_gpu(pcmanager, &row->uuid, (int) by_index[sel]);
    }
}

/**
 * WHAT GPU IS IN EACH PC -- the answer to the question the co-op launch screen asks.
 *
 * Shown for every paired PC, whether or not it has been answered, because the place a user
 * looks to CHANGE an answer is not the place they were asked. A host running the Umbra
 * build reports its own encoder and the row says so instead: measurement is not
 * something to offer as an editable opinion.
 */
static void build_gpu_rows(coop_pane_t *pane, lv_obj_t *view) {
    (void) pane;
    pref_header(view, locstr("Graphics cards"));
    pref_desc_label(view, locstr("Co-op composes one HEVC picture from both PCs. Two NVIDIA "
                                 "cards share it with their own encoders; when either PC has an AMD "
                                 "card both use Umbra's GPU HEVC encoder instead. Umbra reports its "
                                 "card; for any other host Eclipse asks once and remembers the "
                                 "answer here."), false);
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        /* Known PCs only: hosts.ini keeps no other, so a choice made here would not survive. */
        if (cur->server == NULL || !cur->known) {
            continue;
        }
        char title[96];
        const char *name = cur->server->hostname != NULL ? cur->server->hostname : "?";
        if (cur->server->encoderVendorKnown) {
            /* Reported, so there is nothing to choose. Say which machine and what it said. */
            snprintf(title, sizeof(title), locstr("%s - %s (reported by the PC)"), name,
                     coop_gpu_vendor_name(coop_gpu_vendor_parse(cur->server->encoderVendor)));
            pref_title_label(view, title);
            continue;
        }
        snprintf(title, sizeof(title), "%s", name);
        pref_title_label(view, title);
        lv_obj_t *dd = lv_dropdown_create(view);
        lv_dropdown_set_options(dd, locstr("Not set\nNVIDIA\nAMD\nIntel\nOther"));
        lv_obj_set_width(dd, LV_PCT(100));
        switch ((coop_gpu_vendor_t) cur->gpu_vendor) {
            case COOP_GPU_NVIDIA: lv_dropdown_set_selected(dd, 1); break;
            case COOP_GPU_AMD: lv_dropdown_set_selected(dd, 2); break;
            case COOP_GPU_INTEL: lv_dropdown_set_selected(dd, 3); break;
            case COOP_GPU_OTHER: lv_dropdown_set_selected(dd, 4); break;
            default: lv_dropdown_set_selected(dd, 0); break;
        }
        gpu_row_t *row = lv_mem_alloc(sizeof(gpu_row_t));
        row->uuid = cur->id;
        lv_obj_add_event_cb(dd, gpu_row_changed_cb, LV_EVENT_VALUE_CHANGED, row);
        lv_obj_add_event_cb(dd, gpu_row_free_cb, LV_EVENT_DELETE, row);
    }
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container) {
    coop_pane_t *pane = (coop_pane_t *) self;
    commons_log_info("Coop", "coop pane create_obj: entered");
    lv_obj_t *view = pref_pane_container(container);
    commons_log_info("Coop", "coop pane create_obj: container %p", (void *) view);
    lv_obj_set_layout(view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* Start the hub sweep before drawing: it never blocks, so anything already listening
     * is usually recorded by the time the columns are built. */
    coop_pi_scan_begin(&pane->pi_scan, NULL);
    coop_pi_scan_poll(&pane->pi_scan);
    remember_pi_pads(pane);
    pane->pi_poll = lv_timer_create(pi_poll_cb, 500, pane);
    /* Before anything draws: the default-computer dropdowns read this list, and whether
     * that section exists at all depends on how long it is. */
    build_pc_entries(pane);
    commons_log_info("Coop", "coop pane create_obj: scans done, drawing");

    pref_desc_label(view, locstr("These are the co-op dials. Each note below is something "
                                 "measured on this setup, not general advice."), false);

    /* THE CONTRACT WITH THE VIDEO SETTINGS, stated once where the user looks for it. Co-op needs
     * no setup: every value below is either taken from Video settings, derived from it, or forced
     * because co-op cannot work otherwise -- and each derived or forced one has an override further
     * down. A fresh install streams co-op with nothing on this page touched. */
    pref_header(view, locstr("How co-op uses your Video settings"));
    pref_desc_label(view, locstr(
            "Co-op needs no setup: once two PCs are paired, start it from the computer list.\n\n"
            "Resolution and frame rate: the same as Video settings. The whole screen is your Video "
            "resolution and each PC sends its half. Override: Advanced > Picture.\n\n"
            "Bitrate: HALF of the Video bitrate for each PC, so both together use the same total as "
            "a normal stream. Override: Advanced > Picture quality.\n\n"
            "HDR, colour and the other Video settings: exactly as set there, when both PCs can do it.\n\n"
            "Codec and encoder: always HEVC, chosen for the pair of graphics cards rather than by the "
            "Video codec setting. Override: Encoder, below.\n\n"
            "Sound: stereo from both PCs, mixed. Surround cannot be mixed, so co-op always asks for "
            "stereo; Audio below picks which player you hear.\n\n"
            "Host settings: Umbra applies its co-op tuning to co-op streams only, never to a normal "
            "stream. Override: Encoder, below."), false);

    pref_header(view, locstr("Default controllers"));
    pref_desc_label(view, locstr("Which PC each controller belongs to. Everything not spoken for "
                                 "sits under Available; a PC can hold as many as you like. Every "
                                 "controller this app has ever seen is listed, connected or not, "
                                 "and the co-op launch screen can still override any of it for "
                                 "one session."), false);
    build_controllers(pane, view);

    pref_header(view, locstr("Split orientation"));
    lv_obj_t *orientation = pref_dropdown_int(view, orientation_entries,
                                              sizeof(orientation_entries) / sizeof(orientation_entries[0]),
                                              &app_configuration->coop_orientation, NULL);
    lv_obj_set_width(orientation, LV_PCT(100));
    lv_obj_add_event_cb(orientation, orientation_changed_cb, LV_EVENT_VALUE_CHANGED, pane);

    /* MEASURED, not folklore: the two modes load opposite parts of the machine, which is
     * why a per-PC frame rate difference between them is expected and is NOT a fault. */
    pref_desc_label(view, locstr("Vertical gives each player a full-height half of the screen; "
                                 "horizontal gives each a full-width band. Both hosts must be "
                                 "streaming for the split to appear.\n"
                                 "The two modes load OPPOSITE hardware: horizontal is GPU-heavy, "
                                 "vertical is CPU-heavy. A PC that is faster in one mode can be "
                                 "slower in the other -- that is the mode, not a fault."), false);


    /* DEFAULT COMPUTERS -- only worth a control when there are more than two. With two
     * paired PCs the pair is the only pair, and a "default" would be a setting whose
     * every value means the same thing. */
    if (pane->pc_entries_len - 1 > 2) {   /* entries = the PCs, plus "whatever ran last" */
        pref_header(view, locstr("Default computers"));
        lv_obj_t *def_primary = pref_dropdown_string(view, pane->pc_entries, pane->pc_entries_len,
                                                     &app_configuration->coop_default_primary_uuid);
        lv_obj_set_width(def_primary, LV_PCT(100));
        lv_obj_t *def_secondary = pref_dropdown_string(view, pane->pc_entries, pane->pc_entries_len,
                                                       &app_configuration->coop_default_secondary_uuid);
        lv_obj_set_width(def_secondary, LV_PCT(100));
        pref_desc_label(view, locstr("Which two PCs the co-op launch screen opens on. Left them on "
                                     "\"Whatever ran last\" and it follows your last session, which "
                                     "is what it has always done. The launch screen can still "
                                     "change either one for a single session without touching "
                                     "this."), false);
    }


    build_gpu_rows(pane, view);

    pref_header(view, locstr("Encoder"));
    lv_obj_t *encoder = pref_dropdown_int(view, encoder_entries,
                                          sizeof(encoder_entries) / sizeof(encoder_entries[0]),
                                          &app_configuration->coop_encoder, NULL);
    lv_obj_set_width(encoder, LV_PCT(100));
    /* FORCED BY CO-OP, OVERRIDABLE, AND SAID WHY NOT TO. The Video codec setting cannot apply:
     * both halves are one HEVC picture, so both encoders must produce matching coding blocks. */
    pref_desc_label(view, locstr("Recommended: Automatic. Leave it unless Eclipse cannot tell what "
                                 "graphics cards your PCs have.\n\n"
                                 "Both halves of the screen are ONE HEVC picture, so both PCs must "
                                 "encode it the same way -- that is why co-op ignores the Video codec "
                                 "setting. Automatic picks from the pair: NVIDIA + NVIDIA use their own "
                                 "encoders; NVIDIA + AMD and AMD + AMD use Umbra's GPU HEVC encoder on "
                                 "both, because an NVIDIA and an AMD encoder split the picture into "
                                 "different block sizes and one half comes out flat grey.\n\n"
                                 "\"Each PC's own encoder\" on a pair with an AMD card gives that grey "
                                 "or corrupt half. \"GPU HEVC encoder on both PCs\" works on any pair "
                                 "but uses the graphics card harder than NVIDIA's own encoder. The "
                                 "co-op launch screen says which one a session uses."), false);

    pref_checkbox(view, locstr("Co-op tuning on the host PCs"), &app_configuration->coop_host_tuning, false);
    pref_desc_label(view, locstr("Recommended: on. Umbra then applies the settings co-op needs -- "
                                 "constrained encoding, which keeps the seam between the two halves "
                                 "clean -- to co-op streams only, so your normal streams are never "
                                 "affected. Off leaves each PC on its normal settings, and on NVIDIA "
                                 "the seam then shows blocky damage. A host without Umbra ignores "
                                 "this either way."), false);

    {
        /* THE SAME "WHAT DID WE ACTUALLY FIND" LINE THE INPUT PANE CARRIES. Detection is the
         * difference between this dropdown being a choice and being a guess, so it is stated
         * rather than left for the user to infer from a co-op run that comes out wrong. */
        const char *aurora_host = aurora_apollo_host_name();
        char found[160];
        if (aurora_host != NULL) {
            snprintf(found, sizeof(found), locstr("Umbra detected on %s."), aurora_host);
        } else {
            snprintf(found, sizeof(found), "%s",
                     locstr("Umbra not detected on any paired PC, so GPU makes are unknown "
                            "and Automatic uses the core path."));
        }
        pref_desc_label(view, found, false);
    }


    pref_header(view, locstr("Audio"));
    lv_obj_t *audio = pref_dropdown_int(view, audio_entries,
                                        sizeof(audio_entries) / sizeof(audio_entries[0]),
                                        &app_configuration->coop_audio_mode, NULL);
    lv_obj_set_width(audio, LV_PCT(100));
    pref_desc_label(view, locstr("Mixing is the default: the client has one selected audio output and couch "
                                 "co-op is a shared soundstage. The secondary host ships PCM to "
                                 "the primary, which mixes it -- picking a single player does not "
                                 "make the other host stop sending, so it saves nothing.\n\n"
                                 "When this PC is one of the two co-op PCs its game sound is mixed the "
                                 "same way: Umbra sends it to its virtual speakers (\"Steam Streaming "
                                 "Speakers\") and Eclipse plays the mix on the speakers you were using. "
                                 "Without that virtual device this PC plays its own sound and only the "
                                 "other PC's goes through this setting."), false);
    pref_title_label(view, locstr("Play shared sounds once"));
    lv_obj_t *filter = pref_dropdown_int(view, filter_entries, sizeof(filter_entries) / sizeof(filter_entries[0]),
                                         &app_configuration->coop_audio_filter, NULL);
    lv_obj_set_width(filter, LV_PCT(100));
    lv_obj_add_event_cb(filter, on_filter_changed, LV_EVENT_VALUE_CHANGED, pane);
    pane->filter_ahead_label = pref_title_label(view, "");
    pane->filter_ahead_slider = pref_slider(view, &app_configuration->coop_audio_filter_ahead_ms, 0, 100,
                                            FILTER_AHEAD_STEP_MS);
    lv_obj_set_width(pane->filter_ahead_slider, LV_PCT(100));
    lv_obj_add_event_cb(pane->filter_ahead_slider, on_filter_ahead_changed, LV_EVENT_VALUE_CHANGED, pane);
    filter_ahead_refresh(pane, app_configuration->coop_audio_filter);
    pref_desc_label(view, locstr("Both games often play the same narration, cutscene sound or music, and the two "
                                 "copies arrive a little apart, so you hear it twice. The filter compares the two "
                                 "PCs' sound in six frequency ranges and mutes the later copy in each range where "
                                 "the two match. Anything else the later PC plays in a muted range is muted with it "
                                 "for that moment, and the two PCs' volumes are evened out, by up to 6 dB each.\n\n"
                                 "Accurate (recommended) holds both PCs' sound back by the look-ahead plus 5 ms so it "
                                 "can hear each copy start before removing it: cleaner, with far less switching "
                                 "between the two copies. More look-ahead removes copies more completely; less "
                                 "plays sooner. Fast holds the sound back only 5 ms and guesses when a copy is "
                                 "coming, which lets more of it through. The picture's timing never changes. "
                                 "Player 2's sound also passes a short buffer (Advanced) before it is mixed.\n\n"
                                 "Some games choose their music, or other sound clips, at random on each PC, so the "
                                 "two PCs can play different clips at the same moment (Halo: Combat Evolved does this "
                                 "with its music). Those are not copies, so the filter cannot merge them and you hear "
                                 "both, such as two drum patterns at once. If music sounds doubled or messy, turn the "
                                 "game's music off on one of the two PCs.\n\n"
                                 "Where Eclipse feeds the speakers itself (Windows, Linux with PulseAudio), Accurate "
                                 "currently runs as Fast. Only applies while both PCs' sound is streamed and mixed."),
                    false);

    build_advanced(pane, view);
    dials_refresh(pane);

    return view;
}

/*
 * CO-OP SESSION CONFIGURATION.
 *
 * WHY THIS IS NOT AN APP ENTRY. Co-op used to appear as an application ("Co-op Pane")
 * in each PC's game list, which reads as "a game you launch on that PC" and is exactly
 * what it is not: a co-op session spans TWO hosts, and an entry filed under one of them
 * has nowhere to name the other. So the entry point moved up a level -- co-op is chosen
 * under SELECT SERVER, and this dialog is where the session is described before it runs.
 * The Apollo-side "Co-op Pane" entry still exists and is still what gets launched on
 * each host; it just stops being the thing the user picks.
 *
 * EVERY POINTING METHOD IS FIRST-CLASS, because this has to work on TVs that are not the
 * LG G5. Each section is one focusable widget: d-pad and analog stick move between
 * sections with up/down and act inside one with left/right, OK commits, and nothing
 * needs text entry. A POINTER (the LG magic remote, or a mouse) is not a second-class
 * path bolted on -- it acts on whatever is under the cursor, and any click also takes
 * focus so the stick carries on from where the pointer left off.
 *
 * A CLICK AND AN OK PRESS ARE DIFFERENT EVENTS even though LVGL raises
 * LV_EVENT_SHORT_CLICKED for both, and conflating them is what made this feel erratic:
 * a pointer has already aimed, so a click means "act on THAT", while a keypad has no aim,
 * so OK means "pick up what the cursor is on, then steer". See click_from_pointer().
 *
 * CONTROLLERS BELONG TO A MACHINE, ROLES ARE A SEPARATE AXIS. A controller is assigned to
 * a PC because the person holding it is playing on that PC; which PC is player 1 is a
 * different question, about screen halves. The two must not move together -- see
 * swap_roles() and the machine-index note on coop_dialog_t.assign.
 *
 * THE SPLIT-SCREEN SYMBOLS ARE DRAWN, NOT FONT GLYPHS. The Material icon font in this
 * tree is SUBSETTED to 21 glyphs (see material_icons_regular_symbols.h) with no
 * generator script checked in, and none of them is a split screen. More importantly a
 * glyph could not do the job anyway: the primary/secondary symbols have to show WHICH
 * HALF that host drives, and follow the orientation chosen above them, so the fill has
 * to be state. Two child rectangles with a gap between them read as a divided screen at
 * this size and cost nothing.
 */

#include "app.h"
#include "coop.dialog.h"
#include "launcher.controller.h"

#include "app_launch.h"
#include "app_settings.h"
#include "backend/apploader/apploader.h"
#include "backend/aurora_host_caps.h"
#include "backend/pcmanager.h"
#include "platform/input_names.h"
#include "platform/this_pc.h"
#include "stream/input/coop_assign.h"
#include "stream/input/coop_pi_pads.h"
#include "stream/input/coop_pointer.h"
#include "stream/video/coop_session.h"
#include "stream/audio/coop_audio.h"
#include "stream/video/coop_tunables.h"
#include "ui/streaming/pin_autoentry.h"
#include "ui/streaming/streaming.controller.h"
#include "ui/ui_hover.h"
#include "logging.h"

#include "lvgl.h"
#include "lvgl/util/lv_app_utils.h"
#include "lvgl/theme/lv_theme_moonlight.h"

#include "util/i18n.h"

#include <SDL.h>

#include <stdio.h>

/** Which half a symbol should fill in. */
#define SPLIT_FILL_NONE (-1)

/* ONE GRID FOR ALL THREE SECTIONS: a heading column followed by the choices -- two
 * machine-width cards on every row, the controller columns included. */
#define SECTION_LABEL_PCT 24
#define SECTION_COL_PCT   36
/* A CONTROLLER GOES TO ONE PC. There was a third "Both" column; the user does not want it
 * (2026-09-26), so the two columns are the two machines, and they take the same width as the
 * Computers cards above them so each controller column sits under its PC. */
#define PAD_SECTION_COL_PCT SECTION_COL_PCT
#define PAD_ROUTE_COUNT     2
#define COOP_PI_ROUTE_ACK_TIMEOUT_MS 250

_Static_assert(COOP_ASSIGN_PRIMARY == COOP_PAD_PRIMARY &&
               COOP_ASSIGN_SECONDARY == COOP_PAD_SECONDARY &&
               COOP_ASSIGN_BOTH == COOP_PAD_BOTH,
               "co-op assignment and persisted route values must stay identical");

typedef struct coop_dialog_t {
    lv_obj_t *msgbox;
    launcher_fragment_t *launcher;
    app_t *global;

    /** THE TWO MACHINES, IN A FIXED ORDER THAT NEVER CHANGES while the dialog is
     *  open. Controllers are assigned to a MACHINE, not to a player slot: the person
     *  on the sofa is playing on a particular PC, and choosing which PC is player 1
     *  must not silently hand their controller to the other machine. Role is a
     *  separate axis -- see primary_machine. */
    uuidstr_t machine_uuid[2];
    char machine_name[2][64];
    /** Which machine is player 1. Swapping the roles toggles ONLY this. */
    int primary_machine;
    /** False when fewer than two paired PCs exist -- the dialog then explains itself
     *  instead of offering a launch that could only fail. */
    bool hosts_ok;

    coop_assign_pad_t pads[COOP_ASSIGN_MAX_PADS];
    /** Pi-attached pads, parallel to pads[] by index: pi_of[i] is the Pi entry for
     *  pads[i], or -1 when that pad is a local SDL one. Routing a Pi pad means telling
     *  the Pi where to forward, so the address has to survive to launch. */
    int pi_of[COOP_ASSIGN_MAX_PADS];
    /** Stable GameStream controller id for local SDL pads, parallel to pads[].
     *  A disconnected/reconnected device can occupy a sparse app_input slot, so the
     *  compact dialog row index is not a valid coop_pad_pane[] index. Pi pads are -1. */
    int gs_id_of[COOP_ASSIGN_MAX_PADS];
    /** The mouse (0) and the keyboard (1) as assignable items, parallel to pads[]; -1 for a real
     *  controller. Present only with "assign mouse and keyboard like controllers" on (#37). */
    int kbm_of[COOP_ASSIGN_MAX_PADS];
    coop_pi_scan_t pi_scan;
    /** NOTE: .route here is a MACHINE INDEX (0/1 into machine_uuid). It is converted
     *  to a primary/secondary pane at launch, once, using primary_machine. Keeping machine and role apart is what
     *  makes the controller columns independent of the player 1 / player 2 choice. */
    coop_assign_result_t assign[COOP_ASSIGN_MAX_PADS];
    size_t pad_count;

    /** COOP_ORIENTATION_HORIZONTAL / _VERTICAL, applied on launch. */
    int orientation;

    /** What each machine's ENCODER is, as the host itself reported it, parallel to
     *  machine_uuid. Kept per machine rather than only as a resolved plan because the
     *  banner has to name which PC is which -- "install it on Skynet" is actionable,
     *  "one of your PCs" is not. */
    coop_gpu_vendor_t machine_vendor[2];
    bool machine_vendor_known[2];
    /** The codec path this pairing gets, and why. Recomputed whenever the machines
     *  change, snapshotted into the session at launch. */
    coop_stream_plan_t plan;

    /** THE MACHINE CHOOSER, WHICH ONLY EXISTS WHEN THERE IS A CHOICE. With exactly two
     *  paired PCs the pair IS the answer and a row asking which two would be a control
     *  with one setting. Above two, which machines take part is the first question the
     *  dialog has to ask -- everything else on the screen is about those two. */
    bool pc_choosable;
    lv_obj_t *pc_row;
    lv_obj_t *pc_card[2];
    lv_obj_t *pc_name[2];
    /** Which column the d-pad is acting on, 0 or 1. */
    int pc_cursor;

    /** THE GPU QUESTION: which HOST is being asked about, and the msgbox asking it.
     *
     *  A UUID RATHER THAN A COLUMN INDEX, because the question covers every PC this app knows
     *  about, not only the two in the current pairing. Any of them can be swapped into a
     *  column later -- or become the pairing next time -- and being asked mid-launch about a
     *  machine you just selected is worse than being asked about all of them once. */
    uuidstr_t gpu_ask_uuid;
    bool gpu_ask_active;
    lv_obj_t *gpu_msgbox;
    /** True only when the question was raised BY A LAUNCH -- the backstop path, for a PC
     *  swapped into a column after the dialog opened. Asked at open (the normal case) it is
     *  false, and answering simply returns the user to the dialog with the banner corrected. */
    bool gpu_ask_then_launch;

    /** The mode banner, kept so changing a machine can rewrite it -- swapping in a PC of
     *  another make changes the codec, and a banner that still described the old pair
     *  would be worse than none. NULL while there is nothing to say. */
    lv_obj_t *banner;

    /* Controllers section. */
    lv_obj_t *pad_box;
    lv_obj_t *pad_col[PAD_ROUTE_COUNT];
    lv_obj_t *pad_col_title[PAD_ROUTE_COUNT];
    lv_obj_t *pad_item[COOP_ASSIGN_MAX_PADS];
    int pad_cursor;
    /** True once a controller has been picked up: the d-pad then moves IT rather
     *  than the cursor. This is the "keypress selects, d-pad moves" the feature asks
     *  for, and it is why moving a pad cannot happen by accident. */
    bool pad_held;

    /* Orientation section: indexed by COOP_ORIENTATION_*, laid out vertical-first. */
    lv_obj_t *orient_row;
    lv_obj_t *orient_card[2];
    lv_obj_t *orient_icon[2];

    /* Roles section: indexed by role, 0 = primary. */
    lv_obj_t *role_row;
    lv_obj_t *role_card[2];
    lv_obj_t *role_name[2];
    lv_obj_t *role_icon[2];

    /** Rescans for controllers while the dialog is open, so a pad switched on after it
     *  opened turns up on its own instead of needing the screen reopened. */
    lv_timer_t *rescan;

    /* --- waking a sleeping host --------------------------------------------
     * CO-OP MUST BE ABLE TO START WITH EITHER PC (OR BOTH) ASLEEP. A single-host
     * stream can lean on the launcher's manual Wake button, but co-op spans two
     * machines and there is no screen on which to press it twice; the dialog has to
     * do the waking itself, then hold the launch until both answer. Without this the
     * session went straight to connect and failed against whichever PC was asleep. */
    lv_timer_t *wake_timer;
    lv_obj_t *wake_label;
    /** Wall-clock deadline (lv_tick) for the whole wake, not per host. */
    uint32_t wake_deadline;
    /** Ticks of the next re-query, so a slow host is not hammered every frame. */
    uint32_t wake_next_poll;
    /** Resolved at the moment Start was pressed, so the wait cannot be confused by
     *  the roles being swapped underneath it. */
    uuidstr_t wake_primary, wake_secondary;
    /** In-flight app-list lookup after both hosts have answered. Kept separate from
     *  wake_timer because app ids are host-local and must be resolved before either
     *  stream is armed. */
    struct coop_app_resolution_t *app_resolution;
    /** A PC is busy with another app and the user is being asked / the quit is in flight. */
    struct coop_busy_t *busy;
    /** Start buttons at the front of the action row: 2 in CHOOSE mode (app_settings.h host_monitor_mode) --
     *  "Start co-op" and "Start, monitors off" -- else 1. Every enable/disable covers them all. */
    uint16_t start_btns;
    /** The start pressed was the monitors-off one; applied in launch_session. */
    bool monitor_off_choice;
} coop_dialog_t;

static void refresh_pads(coop_dialog_t *dlg);

static void refresh_orientation(coop_dialog_t *dlg);

static void refresh_roles(coop_dialog_t *dlg);
static void refresh_pcs(coop_dialog_t *dlg);
static void refresh_mode_banner(coop_dialog_t *dlg);
static void refresh_machine_facts(coop_dialog_t *dlg);

static void launch_session(coop_dialog_t *dlg);
static void gpu_ask_answered_cb(lv_event_t *e);

static void start_stream(app_t *global, const uuidstr_t *primary, const uuidstr_t *secondary,
                         int primary_app_id, int secondary_app_id, int orientation,
                         const coop_stream_plan_t *plan);

static void pad_item_click_cb(lv_event_t *e);

/* ------------------------------------------------------------------ symbols */

/**
 * A little monitor divided in two. @p parent gets a fixed-size object holding two
 * child rectangles; the gap between them is the divider, which is legible at icon
 * size without drawing an actual line.
 */
static lv_obj_t *split_icon_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h) {
    lv_obj_t *icon = lv_obj_create(parent);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, w, h);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    /* DECORATION, NOT A TARGET. A plain object is CLICKABLE by default in LVGL, so the
     * symbol used to swallow clicks aimed at the card it sits on -- clicking the
     * picture of the split screen did nothing at all -- and the pointer highlighted it
     * as if it were something to press. */
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(icon, lv_dpx(3), 0);
    lv_obj_set_style_border_width(icon, lv_dpx(2), 0);
    lv_obj_set_style_border_color(icon, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_border_opa(icon, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(icon, lv_dpx(3), 0);
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *half = lv_obj_create(icon);
        lv_obj_remove_style_all(half);
        lv_obj_clear_flag(half, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(half, lv_dpx(1), 0);
        lv_obj_set_style_bg_opa(half, LV_OPA_COVER, 0);
    }
    return icon;
}

/**
 * Lay the two halves out for @p orientation and fill @p filled_half (0 = the half
 * player 1 drives, 1 = player 2's, SPLIT_FILL_NONE = show both equally).
 *
 * VERTICAL means a vertical DIVIDER, i.e. left/right panes -- matching
 * COOP_ORIENTATION_VERTICAL and the wording in the co-op settings pane. Getting this
 * backwards would be a picture that contradicts its own label.
 */
static void split_icon_update(lv_obj_t *icon, int orientation, int filled_half) {
    /* PERCENTAGES, NOT MEASURED COORDINATES. This runs while the dialog is still being
     * built, and lv_obj_get_width() reads coords that layout has not computed yet -- it
     * would return 0 here and the halves would be invisible. Percentage sizing is
     * resolved by the layout pass instead, so the symbol is correct on first paint.
     * 47 + 47 leaves the remaining 6% as the divider gap. */
    const bool vertical = orientation == COOP_ORIENTATION_VERTICAL;
    static const lv_align_t v_align[2] = {LV_ALIGN_LEFT_MID, LV_ALIGN_RIGHT_MID};
    static const lv_align_t h_align[2] = {LV_ALIGN_TOP_MID, LV_ALIGN_BOTTOM_MID};

    for (int i = 0; i < 2; ++i) {
        lv_obj_t *half = lv_obj_get_child(icon, i);
        if (half == NULL) {
            continue;
        }
        if (vertical) {
            lv_obj_set_size(half, LV_PCT(47), LV_PCT(100));
        } else {
            lv_obj_set_size(half, LV_PCT(100), LV_PCT(47));
        }
        lv_obj_align(half, vertical ? v_align[i] : h_align[i], 0, 0);
        const bool lit = filled_half == SPLIT_FILL_NONE || filled_half == i;
        lv_obj_set_style_bg_color(half, lit ? lv_palette_main(LV_PALETTE_BLUE)
                                            : lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_set_style_bg_opa(half, lit ? LV_OPA_COVER : LV_OPA_30, 0);
    }
}

/* -------------------------------------------------------------- host lookup */

static const char *host_display_name(const pclist_t *node) {
    if (node == NULL || node->server == NULL || node->server->hostname == NULL) {
        return "?";
    }
    return node->server->hostname;
}

/* ---------------------------------------------------------------- app lookup */

typedef struct coop_app_resolution_t coop_app_resolution_t;

typedef void (*coop_app_resolution_cb)(coop_app_resolution_t *resolution, bool success,
                                       int failed_host, int code, const char *detail,
                                       void *userdata);

typedef struct coop_app_query_t {
    coop_app_resolution_t *resolution;
    int host;
} coop_app_query_t;

/** One asynchronous app-list query per host. The two lists are intentionally not
 *  merged: an application id is scoped to the host that returned it. */
struct coop_app_resolution_t {
    apploader_t *loader[2];
    coop_app_query_t query[2];
    uuidstr_t host[2];
    char host_name[2][64];
    int app_id[2];
    /* What each PC was ALREADY running when its list came in (0 = nothing), and that app's name
     * -- a host busy with a different app refuses the co-op launch (issue #33). */
    int running_id[2];
    char running_name[2][64];
    unsigned complete_mask;
    bool finished;
    coop_app_resolution_cb callback;
    void *userdata;
};

static void coop_app_resolution_destroy(coop_app_resolution_t *resolution) {
    if (resolution == NULL) {
        return;
    }
    for (int i = 0; i < 2; ++i) {
        if (resolution->loader[i] != NULL) {
            /* task is intentionally opaque and apploader retains its last pointer
             * after completion. Cancel only a genuinely in-flight lookup so a stale
             * task address can never alias a later executor job. */
            if (apploader_state(resolution->loader[i]) == APPLOADER_STATE_LOADING) {
                apploader_cancel(resolution->loader[i]);
            }
            apploader_destroy(resolution->loader[i]);
            resolution->loader[i] = NULL;
        }
    }
    SDL_free(resolution);
}

static void coop_app_resolution_finish(coop_app_resolution_t *resolution, bool success,
                                       int failed_host, int code, const char *detail) {
    if (resolution->finished) {
        return;
    }
    resolution->finished = true;
    resolution->callback(resolution, success, failed_host, code, detail, resolution->userdata);
    coop_app_resolution_destroy(resolution);
}

static void coop_app_list_loaded(apploader_list_t *apps, void *userdata) {
    coop_app_query_t *query = userdata;
    coop_app_resolution_t *resolution = query->resolution;
    const int host = query->host;
    const int preferred = app_configuration != NULL && app_configuration->coop_app_id > 0
                          ? app_configuration->coop_app_id : COOP_APP_ID_DEFAULT;

    /* apploader_list_t is contiguous rather than linked. Build a tiny linked view for
     * the shared exact-name resolver without taking ownership of any names. */
    APP_LIST *head = NULL;
    APP_LIST *tail = NULL;
    for (size_t i = 0; apps != NULL && i < apps->count; ++i) {
        APP_LIST *item = &apps->items[i].base;
        item->next = NULL;
        if (tail != NULL) {
            tail->next = item;
        } else {
            head = item;
        }
        tail = item;
    }
    resolution->app_id[host] = coop_session_resolve_app_id(head, preferred);
    const int running = pcmanager_server_current_app(pcmanager, &resolution->host[host]);
    resolution->running_id[host] = running;
    resolution->running_name[host][0] = '\0';
    for (const APP_LIST *it = head; it != NULL && running != 0; it = it->next) {
        if (it->id == running && it->name != NULL) {
            SDL_strlcpy(resolution->running_name[host], it->name, sizeof(resolution->running_name[host]));
            break;
        }
    }
    apploader_list_free(apps);
    if (resolution->app_id[host] <= 0) {
        coop_app_resolution_finish(resolution, false, host, 0, NULL);
        return;
    }
    resolution->complete_mask |= 1u << host;
    if (resolution->complete_mask == 0x3u) {
        coop_app_resolution_finish(resolution, true, -1, 0, NULL);
    }
}

static void coop_app_list_failed(int code, const char *error, void *userdata) {
    coop_app_query_t *query = userdata;
    coop_app_resolution_finish(query->resolution, false, query->host, code, error);
}

static coop_app_resolution_t *coop_app_resolution_start(app_t *global,
                                                        const uuidstr_t *primary,
                                                        const uuidstr_t *secondary,
                                                        coop_app_resolution_cb callback,
                                                        void *userdata) {
    coop_app_resolution_t *resolution = SDL_calloc(1, sizeof(*resolution));
    if (resolution == NULL) {
        return NULL;
    }
    resolution->host[0] = *primary;
    resolution->host[1] = *secondary;
    resolution->callback = callback;
    resolution->userdata = userdata;
    const uuidstr_t *hosts[2] = {primary, secondary};
    const apploader_cb_t callbacks = {
            .data = coop_app_list_loaded,
            .error = coop_app_list_failed,
    };
    for (int i = 0; i < 2; ++i) {
        const pclist_t *node = pcmanager_node(pcmanager, hosts[i]);
        SDL_strlcpy(resolution->host_name[i], host_display_name(node),
                    sizeof(resolution->host_name[i]));
        resolution->query[i].resolution = resolution;
        resolution->query[i].host = i;
        resolution->loader[i] = apploader_create(global, hosts[i], &callbacks,
                                                  &resolution->query[i]);
    }
    apploader_load(resolution->loader[0]);
    apploader_load(resolution->loader[1]);
    return resolution;
}

/**
 * The codec path for a pair of hosts, read fresh from what those hosts are reporting
 * RIGHT NOW.
 *
 * Called again at every launch rather than reusing what the dialog computed when it
 * opened, because a host that was ASLEEP when the popup appeared has no server data at
 * all -- its vendor is unknown until it answers. Waking it is a normal part of this
 * dialog (wake_tick_cb launches once both are up), so the open-time answer is exactly
 * the one most likely to be stale.
 */
static coop_stream_plan_t plan_for_pair(const uuidstr_t *primary, const uuidstr_t *secondary) {
    return coop_session_plan_for_pair(primary, secondary, NULL);
}

/** How many machines this dialog could put in a column. Deliberately the same test the
 *  selection below uses (`server != NULL`), so "is there a choice?" and "what can be
 *  chosen?" can never disagree. */
static int eligible_pc_count(void) {
    int n = 0;
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        if (cur->server != NULL) {
            n++;
        }
    }
    return n;
}

/**
 * Work out which two PCs take part. Preference order: the DEFAULT PAIR from settings if
 * one is set, then what was used last, then the PC currently selected in the launcher,
 * then simply the first paired ones. A remembered PC that is no longer paired is skipped
 * rather than carried forward, so unpairing a machine cannot leave co-op pointing at
 * nothing -- and the same skip is what makes a stale DEFAULT harmless.
 *
 * This is the OPENING position, not the final answer: with more than two PCs paired the
 * dialog shows a chooser and the user can move either column (refresh_machine_facts
 * re-derives everything that depends on it).
 */
static void resolve_hosts(coop_dialog_t *dlg) {
    uuidstr_t found[2];
    int n = 0;

    /* PREFERENCE ORDER, MOST DELIBERATE FIRST. A stated DEFAULT outranks what happened
     * last: someone who set one did so precisely to stop the last session deciding the
     * next. An empty default falls through, so the behaviour for everybody who has never
     * opened that setting is exactly what it was. */
    const char *want[4] = {
            app_configuration->coop_default_primary_uuid,
            app_configuration->coop_default_secondary_uuid,
            app_configuration->coop_primary_uuid,
            app_configuration->coop_secondary_uuid,
    };
    for (int w = 0; w < 4 && n < 2; ++w) {
        if (want[w] == NULL || want[w][0] == '\0') {
            continue;
        }
        for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
            if (cur->server == NULL || !uuidstr_t_equals_s(&cur->id, want[w])) {
                continue;
            }
            if (n == 1 && uuidstr_t_equals_t(&found[0], &cur->id)) {
                break;                      /* same PC twice is not a co-op session */
            }
            found[n++] = cur->id;
            break;
        }
    }
    /* The selected PC is the natural player 1 when nothing has been remembered. */
    if (n == 0) {
        for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
            if (cur->server != NULL && cur->selected) {
                found[n++] = cur->id;
                break;
            }
        }
    }
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL && n < 2; cur = cur->next) {
        if (cur->server == NULL) {
            continue;
        }
        if (n == 1 && uuidstr_t_equals_t(&found[0], &cur->id)) {
            continue;
        }
        found[n++] = cur->id;
    }

    dlg->hosts_ok = n == 2;
    for (int i = 0; i < 2; ++i) {
        if (i < n) {
            dlg->machine_uuid[i] = found[i];
        } else {
            SDL_memset(&dlg->machine_uuid[i], 0, sizeof(dlg->machine_uuid[i]));
        }
    }
    /* THE CHOOSER APPEARS ONLY WHEN THERE IS SOMETHING TO CHOOSE. Counted over exactly
     * the machines the chooser can offer -- the same `server != NULL` set the fallback
     * above picks from -- so the row cannot appear listing fewer PCs than it claims, or
     * stay hidden while a third one is selectable. */
    dlg->pc_choosable = eligible_pc_count() > 2;
    refresh_machine_facts(dlg);
}

/**
 * Re-read everything that DEPENDS on which two machines are in play: their names, what
 * each one's encoder is, and therefore the codec and HDR plan.
 *
 * Split out of resolve_hosts because the pairing is no longer decided once. With more
 * than two PCs paired the user can change a column while the dialog is open, and every
 * one of these facts moves with it -- a different PC can be a different GPU make, which
 * is a different codec, which is a different banner.
 */
static void refresh_machine_facts(coop_dialog_t *dlg) {
    for (int i = 0; i < 2; ++i) {
        const bool present = !uuidstr_is_empty(&dlg->machine_uuid[i]);
        const pclist_t *node = present ? pcmanager_node(pcmanager, &dlg->machine_uuid[i]) : NULL;
        if (!present) {
            SDL_strlcpy(dlg->machine_name[i], locstr("(none)"), sizeof(dlg->machine_name[i]));
            dlg->machine_vendor[i] = COOP_GPU_UNKNOWN;
            dlg->machine_vendor_known[i] = false;
            continue;
        }
        SDL_strlcpy(dlg->machine_name[i], host_display_name(node), sizeof(dlg->machine_name[i]));
        /* STRAIGHT OFF THE HOST, no inference. `encoderVendorKnown` is false for stock
         * Sunshine, for an Apollo without the patch, and for a host we have only reached
         * over the unpaired HTTP probe -- all three are "it did not say", which
         * coop_stream_plan() treats differently from any answer. */
        const SERVER_DATA *server = node != NULL ? node->server : NULL;
        dlg->machine_vendor[i] = server != NULL ? coop_gpu_vendor_parse(server->encoderVendor)
                                                : COOP_GPU_UNKNOWN;
        dlg->machine_vendor_known[i] = server != NULL && server->encoderVendorKnown;
    }
    dlg->plan = plan_for_pair(&dlg->machine_uuid[0], &dlg->machine_uuid[1]);
}

/* --------------------------------------------------------------- controllers */

/* Machine 0 and machine 1 remain stable when the player roles are swapped. */
static const int pad_route_order[PAD_ROUTE_COUNT] = {
        0, 1,
};

static int pad_route_position(int route) {
    for (int i = 0; i < PAD_ROUTE_COUNT; ++i) {
        if (pad_route_order[i] == route) {
            return i;
        }
    }
    return 0;
}

static int pad_route_step(int route, int direction) {
    int position = pad_route_position(route) + direction;
    if (position < 0 || position >= PAD_ROUTE_COUNT) {
        return route;
    }
    return pad_route_order[position];
}

/**
 * Which of the two participating machines has claimed @p name in the settings, as a
 * coop_assign CHOICE.
 *
 * A DEFAULT IS A CHOICE, NOT A FALLBACK. The settings screen assigns controllers to a
 * PC, so a pad the user has already placed must go there whenever that PC is in the
 * session -- not be dealt out by the spread and then have to be moved back by hand every
 * time. Claimed by neither leaves it to the spread -- and so does a pad claimed by BOTH,
 * which the settings screen no longer lets anyone make and which could only come from an
 * older conf: a controller goes to one PC, and the spread picks which rather than guessing.
 */
static int pad_default_choice(const coop_dialog_t *dlg, const char *name) {
    bool claimed[2] = {false, false};
    for (int m = 0; m < 2; ++m) {
        const pclist_t *node = pcmanager_node(pcmanager, &dlg->machine_uuid[m]);
        claimed[m] = node != NULL && pcmanager_node_has_pad(node, name);
    }
    if (claimed[0] && claimed[1]) {
        return COOP_ASSIGN_UNSET;
    }
    if (claimed[0]) {
        return 0;
    }
    if (claimed[1]) {
        return 1;
    }
    return COOP_ASSIGN_UNSET;
}

/**
 * Collect the controllers ACTUALLY CONNECTED and let coop_assign spread them. Only
 * connected pads are offered -- a picker listing four fixed slots was the old model
 * and it could not say which of them exists.
 */
static void collect_pads(coop_dialog_t *dlg) {
    dlg->pad_count = 0;
    for (size_t i = 0; i < COOP_ASSIGN_MAX_PADS; ++i) {
        dlg->pi_of[i] = -1;
        dlg->gs_id_of[i] = -1;
        dlg->kbm_of[i] = -1;
    }

    const app_input_t *input = &dlg->global->input;
    for (size_t i = 0; i < input->max_num_gamepads && dlg->pad_count < COOP_ASSIGN_MAX_PADS; ++i) {
        const app_gamepad_state_t *gamepad = &input->gamepads[i];
        SDL_GameController *pad = gamepad->controller;
        if (pad == NULL) {
            continue;
        }
        coop_assign_pad_t *slot = &dlg->pads[dlg->pad_count];
        SDL_memset(slot, 0, sizeof(*slot));
        const char *name = SDL_GameControllerName(pad);
        SDL_strlcpy(slot->name, name != NULL ? name : "Controller", sizeof(slot->name));
        slot->source = COOP_ASSIGN_SRC_TV;
        slot->pinned_host = COOP_ASSIGN_UNSET;
        slot->choice = pad_default_choice(dlg, slot->name);
        /* The old four numbered slots, still honoured for a conf written before
         * controllers belonged to a PC -- and only where nothing has claimed this one. */
        slot->settings_default = gamepad->gs_id >= 0 && gamepad->gs_id < 4
                                 ? app_configuration->coop_pad_pane[gamepad->gs_id]
                                 : COOP_ASSIGN_UNSET;
        dlg->gs_id_of[dlg->pad_count] = gamepad->gs_id;
        dlg->pad_count++;
    }

    /* THE PI'S PADS ARE NOT SDL DEVICES and never will be -- the Pi reads the evdev
     * node itself and forwards the state straight to a PC, so nothing about them is
     * visible to this process. Whatever the running scan has heard back so far is used
     * as-is; it is never waited on, so opening the dialog costs nothing even with no
     * Pi on the network. */
    for (size_t i = 0; i < dlg->pi_scan.count && dlg->pad_count < COOP_ASSIGN_MAX_PADS; ++i) {
        coop_assign_pad_t *slot = &dlg->pads[dlg->pad_count];
        SDL_memset(slot, 0, sizeof(*slot));
        SDL_strlcpy(slot->name, dlg->pi_scan.pads[i].name, sizeof(slot->name));
        slot->source = COOP_ASSIGN_SRC_PI;
        /* NOT pinned: being on the Pi is exactly what makes it movable -- the Pi can
         * forward to either host, which is the whole reason it belongs on this list. */
        slot->pinned_host = COOP_ASSIGN_UNSET;
        slot->choice = pad_default_choice(dlg, slot->name);
        slot->settings_default = COOP_ASSIGN_UNSET;
        /* A hub pad is invisible to SDL, so this is one of the two places it can be
         * learned; without it a controller that only ever appears through the Pi could
         * never be given a default. */
        if (settings_remember_pad(app_configuration, slot->name)) {
            settings_save(app_configuration);
        }
        dlg->pi_of[dlg->pad_count] = (int) i;
        dlg->pad_count++;
    }

    coop_assign_compute(dlg->pads, dlg->pad_count, 2, dlg->assign);

    /* THE MOUSE AND THE KEYBOARD, AFTER the controllers and OUTSIDE the spread: dealing them
     * out with the pads would move a pad to the other PC to even up the totals. Each goes where
     * the settings put it, else to this PC when it is one of the two, else to the first PC. */
    if (app_configuration->coop_kbm_assign) {
        int fallback = 0;
        for (int m = 0; m < 2; ++m) {
            const pclist_t *node = pcmanager_node(pcmanager, &dlg->machine_uuid[m]);
            if (node != NULL && node->server != NULL && local_host_is_this_pc(node->server->serverInfo.address)) {
                fallback = m;
                break;
            }
        }
        static const char *const names[2] = {COOP_KBM_MOUSE, COOP_KBM_KEYBOARD};
        for (int k = 0; k < 2 && dlg->pad_count < COOP_ASSIGN_MAX_PADS; ++k) {
            const size_t i = dlg->pad_count;
            coop_assign_pad_t *slot = &dlg->pads[i];
            SDL_memset(slot, 0, sizeof(*slot));
            SDL_strlcpy(slot->name, names[k], sizeof(slot->name));
            slot->source = COOP_ASSIGN_SRC_TV;
            slot->pinned_host = COOP_ASSIGN_UNSET;
            slot->choice = pad_default_choice(dlg, names[k]);
            slot->settings_default = COOP_ASSIGN_UNSET;
            dlg->kbm_of[i] = k;
            /* The device's own name where Windows can tell it, like a controller's, with its kind. */
            input_device_label(k == 1, k == 0 ? locstr("Mouse") : locstr("Keyboard"), dlg->assign[i].label,
                               sizeof(dlg->assign[i].label));
            dlg->assign[i].route = slot->choice != COOP_ASSIGN_UNSET ? slot->choice : fallback;
            dlg->pad_count++;
        }
    }
}

/** Create or destroy label objects so there is exactly one per connected pad. The
 *  list is rebuilt live, so this runs whenever the pad set changes. */
static void rebuild_pad_items(coop_dialog_t *dlg) {
    for (size_t i = 0; i < dlg->pad_count; ++i) {
        if (dlg->pad_item[i] != NULL) {
            continue;
        }
        lv_obj_t *item = lv_label_create(dlg->pad_col[0]);
        lv_label_set_long_mode(item, LV_LABEL_LONG_DOT);
        lv_obj_set_width(item, LV_PCT(100));
        lv_obj_set_style_text_font(item, lv_theme_get_font_small(item), 0);
        lv_obj_set_style_pad_all(item, lv_dpx(2), 0);
        lv_obj_set_style_radius(item, lv_dpx(3), 0);
        /* Clickable so a pointer can move a controller without the pick-up step. */
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        ui_hover_mark(item);
        lv_obj_add_event_cb(item, pad_item_click_cb, LV_EVENT_SHORT_CLICKED, dlg);
        dlg->pad_item[i] = item;
    }
    for (size_t i = dlg->pad_count; i < COOP_ASSIGN_MAX_PADS; ++i) {
        if (dlg->pad_item[i] != NULL) {
            lv_obj_del(dlg->pad_item[i]);
            dlg->pad_item[i] = NULL;
        }
    }
    if (dlg->pad_cursor >= (int) dlg->pad_count) {
        dlg->pad_cursor = dlg->pad_count > 0 ? (int) dlg->pad_count - 1 : 0;
    }
}

/**
 * Re-enumerate and redraw, KEEPING WHAT THE USER ALREADY CHOSE.
 *
 * A rescan that reset the assignments would be worse than no rescan at all: it would
 * silently undo a deliberate choice every couple of seconds. So each pad that is still
 * present keeps its machine, matched on name plus source, and only genuinely new pads
 * are dealt out by the assignment engine.
 */
static void rescan_pads(coop_dialog_t *dlg) {
    coop_assign_pad_t before[COOP_ASSIGN_MAX_PADS];
    coop_assign_result_t before_assign[COOP_ASSIGN_MAX_PADS];
    bool before_used[COOP_ASSIGN_MAX_PADS] = {false};
    const size_t before_count = dlg->pad_count;
    SDL_memcpy(before, dlg->pads, sizeof(before));
    SDL_memcpy(before_assign, dlg->assign, sizeof(before_assign));

    collect_pads(dlg);

    for (size_t i = 0; i < dlg->pad_count; ++i) {
        for (size_t j = 0; j < before_count; ++j) {
            if (!before_used[j] && dlg->pads[i].source == before[j].source &&
                SDL_strcmp(dlg->pads[i].name, before[j].name) == 0) {
                dlg->assign[i].route = before_assign[j].route;
                before_used[j] = true;
                break;
            }
        }
    }
    rebuild_pad_items(dlg);
    refresh_pads(dlg);
}

static void rescan_timer_cb(lv_timer_t *timer) {
    coop_dialog_t *dlg = timer->user_data;
    /* Re-probe periodically so a Pi or a pad powered on later is found, and drain
     * whatever has already answered. Both are non-blocking. */
    static int tick = 0;
    if (++tick % 8 == 0) {
        const pclist_t *node = pcmanager_node(pcmanager, &dlg->machine_uuid[0]);
        coop_pi_scan_probe(&dlg->pi_scan, node != NULL && node->server != NULL
                                          ? node->server->serverInfo.address : NULL);
    }
    const bool pi_changed = coop_pi_scan_poll(&dlg->pi_scan);

    /* SDL pads are counted directly -- plugging one in is a local event with no
     * network round trip, so noticing it costs a comparison. */
    size_t sdl_now = 0;
    const app_input_t *input = &dlg->global->input;
    for (size_t i = 0; i < input->max_num_gamepads; ++i) {
        if (input->gamepads[i].controller != NULL) {
            sdl_now++;
        }
    }
    size_t sdl_before = 0;
    for (size_t i = 0; i < dlg->pad_count; ++i) {
        if (dlg->pi_of[i] < 0 && dlg->kbm_of[i] < 0) {
            sdl_before++;
        }
    }
    if (pi_changed || sdl_now != sdl_before) {
        rescan_pads(dlg);
    }
    /* The mouse and keyboard rows name the devices in use; the keyboard is only known once a key
     * is pressed (platform/input_names.h), so re-read the names while the dialog is open. */
    if (tick % 8 == 4) {
        bool renamed = false;
        for (size_t i = 0; i < dlg->pad_count; ++i) {
            if (dlg->kbm_of[i] < 0) {
                continue;
            }
            char label[sizeof(dlg->assign[i].label)];
            input_device_label(dlg->kbm_of[i] == 1, dlg->kbm_of[i] == 0 ? locstr("Mouse") : locstr("Keyboard"), label,
                               sizeof(label));
            if (SDL_strcmp(label, dlg->assign[i].label) != 0) {
                SDL_strlcpy(dlg->assign[i].label, label, sizeof(dlg->assign[i].label));
                renamed = true;
            }
        }
        if (renamed) {
            refresh_pads(dlg);
        }
    }
}

/* ------------------------------------------------------------------ redraw */

static void refresh_pads(coop_dialog_t *dlg) {
    /* MACHINE names, not player roles: these columns must not move when the roles
     * are swapped. */
    lv_label_set_text(dlg->pad_col_title[0], dlg->machine_name[0]);
    lv_label_set_text(dlg->pad_col_title[1], dlg->machine_name[1]);
    for (size_t i = 0; i < dlg->pad_count; ++i) {
        lv_obj_t *item = dlg->pad_item[i];
        const int column = pad_route_position(dlg->assign[i].route);
        if (lv_obj_get_parent(item) != dlg->pad_col[column]) {
            lv_obj_set_parent(item, dlg->pad_col[column]);
        }
        /* SAY WHERE IT IS PLUGGED IN. A Pi Xbox pad and a TV Xbox pad report the same
         * device name, so the de-duplication rule alone would show them as "Xbox
         * Wireless Controller" and "... 2" -- two identical-looking rows with no way to
         * tell which one is in your hand. The source is the distinguishing fact. */
        if (dlg->pi_of[i] >= 0) {
            /* Plain text, not an LV_SYMBOL_*: those live in the TEXT font, which is
             * subsetted in this build, and a missing glyph would render as tofu. */
            char labelled[COOP_ASSIGN_LABEL_MAX + 40];
            snprintf(labelled, sizeof(labelled), "%s (hub)", dlg->assign[i].label);
            lv_label_set_text(item, labelled);
        } else {
            lv_label_set_text(item, dlg->assign[i].label);
        }
        const bool on_cursor = (int) i == dlg->pad_cursor;
        /* Held reads differently from merely under the cursor: while held the d-pad
         * moves the pad, and the user must be able to see that without guessing. */
        lv_obj_set_style_bg_opa(item, on_cursor ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(item, dlg->pad_held ? lv_palette_main(LV_PALETTE_AMBER)
                                                      : lv_palette_main(LV_PALETTE_BLUE), 0);
        if (on_cursor) {
            lv_obj_set_style_text_color(item, lv_color_black(), 0);
            /* The columns cap their height and scroll, so with enough controllers the
             * cursor can be on a row that is not in view -- and an invisible cursor is
             * indistinguishable from a d-pad that stopped working. */
            lv_obj_scroll_to_view(item, LV_ANIM_OFF);
        } else {
            /* Drop the override rather than picking a colour: the theme's text colour
             * is the one that is legible on this background in both themes. */
            lv_obj_remove_local_style_prop(item, LV_STYLE_TEXT_COLOR, 0);
        }
    }
}

static void refresh_orientation(coop_dialog_t *dlg) {
    for (int i = 0; i < 2; ++i) {
        const bool sel = i == dlg->orientation;
        lv_obj_set_style_border_color(dlg->orient_card[i],
                                      sel ? lv_palette_main(LV_PALETTE_BLUE)
                                          : lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_set_style_border_width(dlg->orient_card[i], sel ? lv_dpx(3) : lv_dpx(1), 0);
        lv_obj_set_style_bg_opa(dlg->orient_card[i], sel ? LV_OPA_20 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(dlg->orient_card[i], lv_palette_main(LV_PALETTE_BLUE), 0);
        split_icon_update(dlg->orient_icon[i], i, SPLIT_FILL_NONE);
    }
    /* VERTICAL IS DIMMED, NOT HIDDEN, IN COMPATIBILITY MODE. H.264 has no tiles, so a
     * left/right split cannot be built at all -- but hiding the option would make the screen
     * change shape depending on which PCs are paired, and leave someone who knows the feature
     * exists looking for it. Dimmed with the banner saying why is the honest version.
     *
     * The selection itself is NOT blocked: coop_orientation_for_mode() clamps at launch, so
     * choosing it costs a line of explanation rather than a broken session. */
    const bool vertical_possible = true;
    lv_obj_set_style_opa(dlg->orient_card[COOP_ORIENTATION_VERTICAL],
                         vertical_possible ? LV_OPA_COVER : LV_OPA_40, 0);
    /* The banner explains the dimming, and the orientation is part of what it describes. */
    refresh_mode_banner(dlg);
}

/**
 * Redraw the machine chooser. Only ever called when it exists -- with two PCs paired
 * there is no row to redraw.
 */
static bool host_online(const uuidstr_t *uuid);

static void refresh_pcs(coop_dialog_t *dlg) {
    if (dlg->pc_row == NULL) {
        return;
    }
    for (int side = 0; side < 2; ++side) {
        /* SAY WHEN A CHOSEN PC IS ASLEEP (issue #26): the dialog reopens on the last pair, and
         * Start will wake a PC it cannot reach -- the card is the only place to see that first. */
        if (!uuidstr_is_empty(&dlg->machine_uuid[side]) && !host_online(&dlg->machine_uuid[side])) {
            char name[96];
            SDL_snprintf(name, sizeof(name), "%s (%s)", dlg->machine_name[side], locstr("offline"));
            lv_label_set_text(dlg->pc_name[side], name);
        } else {
            lv_label_set_text(dlg->pc_name[side], dlg->machine_name[side]);
        }
        /* Same cursor language as the controller list: the column the d-pad is acting
         * on is FILLED, not merely outlined. An outline reads as "focused" on a screen
         * where three sections can be outlined at once. */
        const bool on_cursor = side == dlg->pc_cursor;
        lv_obj_set_style_bg_opa(dlg->pc_card[side], on_cursor ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(dlg->pc_card[side], lv_palette_main(LV_PALETTE_BLUE), 0);
        if (on_cursor) {
            lv_obj_set_style_text_color(dlg->pc_name[side], lv_color_black(), 0);
        } else {
            lv_obj_remove_local_style_prop(dlg->pc_name[side], LV_STYLE_TEXT_COLOR, 0);
        }
    }
}

/**
 * Move column @p side to the next machine that is not already in the OTHER column.
 *
 * Cycles rather than opening a list: the whole row is two cards on a screen driven by a
 * d-pad, and with three or four PCs paired -- which is what this feature is for -- a
 * list would be a second modal over a modal to choose between three names.
 *
 * EVERYTHING DOWNSTREAM MOVES WITH IT. The controller columns are titled by machine, the
 * player 1 / player 2 cards name a machine, and the codec plan is derived from both
 * machines' GPUs, so all three are re-derived here rather than at launch. A dialog that
 * showed the old pair's banner while pointing at a new pair would be lying at exactly
 * the moment the user is deciding.
 */
static void cycle_pc(coop_dialog_t *dlg, int side) {
    if (!dlg->pc_choosable || side < 0 || side > 1) {
        return;
    }
    const uuidstr_t *const other = &dlg->machine_uuid[1 - side];
    const pclist_t *first_eligible = NULL;
    const pclist_t *next = NULL;
    bool seen_current = false;
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        if (cur->server == NULL || uuidstr_t_equals_t(&cur->id, other)) {
            continue;                       /* the same PC cannot hold both panes */
        }
        if (first_eligible == NULL) {
            first_eligible = cur;
        }
        if (seen_current) {
            next = cur;
            break;
        }
        if (uuidstr_t_equals_t(&cur->id, &dlg->machine_uuid[side])) {
            seen_current = true;
        }
    }
    /* Past the end, come back to the top. `first_eligible` is also the answer when the
     * current machine is no longer in the list at all -- a PC unpaired while the dialog
     * was open -- which would otherwise leave the column stuck. */
    if (next == NULL) {
        next = first_eligible;
    }
    if (next == NULL || uuidstr_t_equals_t(&next->id, &dlg->machine_uuid[side])) {
        return;                             /* nothing else to move to */
    }
    dlg->machine_uuid[side] = next->id;
    refresh_machine_facts(dlg);
    refresh_pcs(dlg);
    refresh_pads(dlg);
    refresh_roles(dlg);
    refresh_mode_banner(dlg);
}

static void refresh_roles(coop_dialog_t *dlg) {
    for (int role = 0; role < 2; ++role) {
        const int machine = role == 0 ? dlg->primary_machine : 1 - dlg->primary_machine;
        lv_label_set_text(dlg->role_name[role], dlg->machine_name[machine]);
        /* The filled half FOLLOWS the orientation chosen above: player 1 is the left
         * pane when the split is vertical and the top one when it is horizontal. */
        split_icon_update(dlg->role_icon[role], dlg->orientation, role);
    }
}

/* ------------------------------------------------------------------- input */

/**
 * A section acts only while it OWNS the focus.
 *
 * LVGL already delivers LV_EVENT_KEY to the focused object alone
 * (lv_group_send_data), so this is belt and braces -- but it is cheap and it makes
 * the rule explicit rather than relying on a dispatch detail two libraries away. It
 * also covers the pointer case, where a click can arrive at a row that is not the
 * focused one.
 */
static bool section_has_focus(lv_obj_t *row) {
    lv_group_t *group = lv_obj_get_group(row);
    return group != NULL && lv_group_get_focused(group) == row;
}

/** Clicking anywhere in a section takes focus, so the d-pad continues where the
 *  pointer left off. Without this a magic-remote click and a later d-pad press
 *  would act on two different sections. */
static void section_click_focus(lv_obj_t *row) {
    lv_group_t *group = lv_obj_get_group(row);
    if (group != NULL && lv_group_get_focused(group) != row) {
        lv_group_focus_obj(row);
    }
}

/**
 * Did this click come from a POINTER, or from OK on a remote/controller?
 *
 * LVGL raises LV_EVENT_SHORT_CLICKED for both, and the two want different things.
 * A pointer has already aimed at something, so a click means "act on THAT". A
 * keypad has no aim, so OK means "pick up what the cursor is on, then steer". The
 * controllers box was toggling the pick-up state for pointer clicks too, which is
 * why clicking it looked like it sometimes selected the box and sometimes did
 * nothing -- pick-up has no visible meaning when there is a cursor.
 */
static bool click_from_pointer(void) {
    lv_indev_t *indev = lv_indev_get_act();
    return indev != NULL && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER;
}

/**
 * THE CONTROLLERS ARE IN THREE COLUMNS, so navigation has to be spatial.
 *
 * Walking the flat pad list with up/down moved the cursor SIDEWAYS whenever two
 * consecutive pads belonged to different machines -- the commonest case of all, since
 * the assignment engine spreads them -- so a DOWN press visibly went right. Up/down now
 * step within the column the cursor is in and left/right cross between them.
 *
 * READ FROM THE LIVE TREE, NOT FROM THE INDEX. Moving a pad to the other machine
 * re-parents its label, which appends it to the end of that column, so pad order and
 * what is on screen part company the moment anything is assigned. The children of the
 * column ARE the drawn order.
 */
static int pad_of_obj(coop_dialog_t *dlg, const lv_obj_t *obj) {
    for (size_t i = 0; i < dlg->pad_count; ++i) {
        if (dlg->pad_item[i] == obj) {
            return (int) i;
        }
    }
    return -1;
}

/** The pad one row below (@p step 1) or above (-1) the cursor in its own column, or -1
 *  when the column ends there. */
static int pad_step(coop_dialog_t *dlg, int step) {
    lv_obj_t *cur = dlg->pad_item[dlg->pad_cursor];
    lv_obj_t *col = cur != NULL ? lv_obj_get_parent(cur) : NULL;
    if (col == NULL) {
        return -1;
    }
    const int32_t want = (int32_t) lv_obj_get_index(cur) + step;
    if (want < 0 || want >= (int32_t) lv_obj_get_child_cnt(col)) {
        return -1;
    }
    return pad_of_obj(dlg, lv_obj_get_child(col, (uint32_t) want));
}

/** The pad in visual @p column nearest the cursor's own row, or -1 if the
 *  cursor is already there or that column is empty. Keeping the row is what makes
 *  crossing read as a step sideways rather than a jump to the top. */
static int pad_cross(coop_dialog_t *dlg, int column) {
    lv_obj_t *cur = dlg->pad_item[dlg->pad_cursor];
    lv_obj_t *col = dlg->pad_col[column];
    if (cur == NULL || col == NULL || lv_obj_get_parent(cur) == col) {
        return -1;
    }
    const uint32_t count = lv_obj_get_child_cnt(col);
    if (count == 0) {
        return -1;
    }
    uint32_t row = lv_obj_get_index(cur);
    if (row >= count) {
        row = count - 1;
    }
    return pad_of_obj(dlg, lv_obj_get_child(col, row));
}

/** Hand focus to the section above or below this one. */
static void pad_box_leave(coop_dialog_t *dlg, bool down) {
    lv_group_t *group = lv_obj_get_group(dlg->pad_box);
    if (down) {
        lv_group_focus_next(group);
    } else {
        lv_group_focus_prev(group);
    }
}

static void pad_box_key_cb(lv_event_t *e) {
    coop_dialog_t *dlg = lv_event_get_user_data(e);
    const uint32_t key = lv_event_get_key(e);
    if (!section_has_focus(dlg->pad_box)) {
        return;
    }
    if (dlg->pad_count == 0) {
        /* AN EMPTY BOX MUST STILL BE PASSED THROUGH. With nothing to move the cursor
         * between, up and down were simply swallowed and focus was stuck here for good
         * -- and having no controller connected yet is the normal state when the dialog
         * is first opened. */
        if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
            pad_box_leave(dlg, key == LV_KEY_DOWN);
        }
        return;
    }
    if (dlg->pad_held) {
        /* Holding a pad: left/right moves it between machine 0 and machine 1.
         * Up/down are deliberately inert so a pad cannot be dropped somewhere
         * unseen. */
        if (key == LV_KEY_LEFT) {
            dlg->assign[dlg->pad_cursor].route =
                    pad_route_step(dlg->assign[dlg->pad_cursor].route, -1);
        } else if (key == LV_KEY_RIGHT) {
            dlg->assign[dlg->pad_cursor].route =
                    pad_route_step(dlg->assign[dlg->pad_cursor].route, 1);
        } else {
            return;
        }
        refresh_pads(dlg);
        return;
    }
    /* Not holding: move as the list is drawn, and step OUT of the section at the top or
     * bottom of a column so the d-pad never traps focus inside the box. */
    int next;
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        const bool down = key == LV_KEY_DOWN;
        next = pad_step(dlg, down ? 1 : -1);
        if (next < 0) {
            pad_box_leave(dlg, down);
            return;
        }
    } else if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
        const int current_column = pad_route_position(dlg->assign[dlg->pad_cursor].route);
        const int direction = key == LV_KEY_RIGHT ? 1 : -1;
        next = -1;
        for (int target_column = current_column + direction;
             target_column >= 0 && target_column < PAD_ROUTE_COUNT;
             target_column += direction) {
            next = pad_cross(dlg, target_column);
            if (next >= 0) {
                break;
            }
        }
        if (next < 0) {
            return;                 /* nothing over there: stay put rather than wrap */
        }
    } else {
        return;
    }
    dlg->pad_cursor = next;
    refresh_pads(dlg);
}

static void pad_box_click_cb(lv_event_t *e) {
    coop_dialog_t *dlg = lv_event_get_user_data(e);
    section_click_focus(dlg->pad_box);
    /* A POINTER CLICK ON THE BOX ONLY SELECTS IT. Anything more would be guesswork:
     * the click landed on the frame, a heading or empty space, not on a controller,
     * so there is no target to act on. Pick-up stays a keypad gesture -- and a keypad
     * only ever clicks the object it has already focused. */
    if (click_from_pointer() || dlg->pad_count == 0) {
        return;
    }
    dlg->pad_held = !dlg->pad_held;
    refresh_pads(dlg);
}

/** POINTER PATH: clicking a controller moves it to the other machine. With
 *  the magic remote there is no "pick up, then steer" -- the target is already under
 *  the cursor, so the pick-up step would be a pointless extra click. */
static void pad_item_click_cb(lv_event_t *e) {
    coop_dialog_t *dlg = lv_event_get_user_data(e);
    lv_obj_t *item = lv_event_get_target(e);
    for (size_t i = 0; i < dlg->pad_count; ++i) {
        if (dlg->pad_item[i] != item) {
            continue;
        }
        dlg->pad_cursor = (int) i;
        dlg->pad_held = false;
        const int position = pad_route_position(dlg->assign[i].route);
        dlg->assign[i].route = pad_route_order[(position + 1) % PAD_ROUTE_COUNT];
        section_click_focus(dlg->pad_box);
        refresh_pads(dlg);
        return;
    }
}

static void orient_key_cb(lv_event_t *e) {
    coop_dialog_t *dlg = lv_event_get_user_data(e);
    const uint32_t key = lv_event_get_key(e);
    if (!section_has_focus(dlg->orient_row)) {
        return;
    }
    if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
        dlg->orientation = key == LV_KEY_LEFT ? COOP_ORIENTATION_VERTICAL
                                              : COOP_ORIENTATION_HORIZONTAL;
        refresh_orientation(dlg);
        refresh_roles(dlg);           /* the role symbols follow the orientation */
    } else if (key == LV_KEY_UP) {
        lv_group_focus_prev(lv_obj_get_group(dlg->orient_row));
    } else if (key == LV_KEY_DOWN) {
        lv_group_focus_next(lv_obj_get_group(dlg->orient_row));
    }
}

static void orient_click_cb(lv_event_t *e) {
    coop_dialog_t *dlg = lv_event_get_user_data(e);
    section_click_focus(dlg->orient_row);
    dlg->orientation = dlg->orientation == COOP_ORIENTATION_VERTICAL
                       ? COOP_ORIENTATION_HORIZONTAL : COOP_ORIENTATION_VERTICAL;
    refresh_orientation(dlg);
    refresh_roles(dlg);
}

/** POINTER PATH: clicking a card picks THAT orientation rather than toggling, because
 *  with a pointer the user aimed at a specific one. */
static void orient_card_click_cb(lv_event_t *e) {
    coop_dialog_t *dlg = lv_event_get_user_data(e);
    lv_obj_t *card = lv_event_get_target(e);
    for (int i = 0; i < 2; ++i) {
        if (dlg->orient_card[i] != card) {
            continue;
        }
        dlg->orientation = i;
        section_click_focus(dlg->orient_row);
        refresh_orientation(dlg);
        refresh_roles(dlg);
        return;
    }
}

/**
 * Swap which PC is player 1 -- and NOTHING ELSE.
 *
 * THE TWO CHOICES ARE INDEPENDENT. A controller belongs to a MACHINE, because the
 * person holding it is playing on that PC; which PC happens to be player 1 is a
 * separate question about screen halves and who the primary session is. This used to
 * relabel the controller columns as well, so choosing player 2 appeared to hand
 * everyone's controller to the other machine. Only the role symbols and names move.
 */
static void swap_roles(coop_dialog_t *dlg) {
    if (!dlg->hosts_ok) {
        return;
    }
    dlg->primary_machine = 1 - dlg->primary_machine;
    refresh_roles(dlg);
}

static void pc_key_cb(lv_event_t *e) {
    coop_dialog_t *dlg = lv_event_get_user_data(e);
    const uint32_t key = lv_event_get_key(e);
    if (!section_has_focus(dlg->pc_row)) {
        return;
    }
    /* LEFT/RIGHT chooses WHICH column, OK changes the machine in it. The other way
     * round -- left/right cycling the machine under a fixed cursor -- makes the two
     * columns unreachable from each other and hides that the row has two halves. */
    if (key == LV_KEY_LEFT) {
        dlg->pc_cursor = 0;
        refresh_pcs(dlg);
    } else if (key == LV_KEY_RIGHT) {
        dlg->pc_cursor = 1;
        refresh_pcs(dlg);
    } else if (key == LV_KEY_ENTER) {
        cycle_pc(dlg, dlg->pc_cursor);
    } else if (key == LV_KEY_UP) {
        lv_group_focus_prev(lv_obj_get_group(dlg->pc_row));
    } else if (key == LV_KEY_DOWN) {
        lv_group_focus_next(lv_obj_get_group(dlg->pc_row));
    }
}

static void pc_click_cb(lv_event_t *e) {
    coop_dialog_t *dlg = lv_event_get_user_data(e);
    section_click_focus(dlg->pc_row);
}

/** POINTER PATH: clicking a column both selects it and advances it, because a click on
 *  a card is a statement about THAT card -- the same reasoning as role_card_click_cb. */
static void pc_card_click_cb(lv_event_t *e) {
    coop_dialog_t *dlg = lv_event_get_user_data(e);
    lv_obj_t *card = lv_event_get_target(e);
    section_click_focus(dlg->pc_row);
    const int side = card == dlg->pc_card[1] ? 1 : 0;
    dlg->pc_cursor = side;
    cycle_pc(dlg, side);
}

static void role_key_cb(lv_event_t *e) {
    coop_dialog_t *dlg = lv_event_get_user_data(e);
    const uint32_t key = lv_event_get_key(e);
    if (!section_has_focus(dlg->role_row)) {
        return;
    }
    if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
        swap_roles(dlg);
    } else if (key == LV_KEY_UP) {
        lv_group_focus_prev(lv_obj_get_group(dlg->role_row));
    } else if (key == LV_KEY_DOWN) {
        lv_group_focus_next(lv_obj_get_group(dlg->role_row));
    }
}

static void role_click_cb(lv_event_t *e) {
    coop_dialog_t *dlg = lv_event_get_user_data(e);
    section_click_focus(dlg->role_row);
    swap_roles(dlg);
}

/** POINTER PATH: clicking a role card makes THAT host player 1, so a click on
 *  "Secondary" promotes it rather than blindly toggling. */
static void role_card_click_cb(lv_event_t *e) {
    coop_dialog_t *dlg = lv_event_get_user_data(e);
    lv_obj_t *card = lv_event_get_target(e);
    section_click_focus(dlg->role_row);
    if (card == dlg->role_card[1]) {
        swap_roles(dlg);
    }
}

/**
 * BACK, FROM ANY SECTION.
 *
 * LVGL delivers LV_EVENT_CANCEL to the FOCUSED OBJECT (lv_indev.c), and the handler
 * that closes a dialog lives on the msgbox -- so back only ever worked while the button
 * row happened to be focused, because the button row is the one child with
 * EVENT_BUBBLE. Focus a section and the remote's back button, and a controller's B, did
 * nothing at all. Every section therefore answers CANCEL itself.
 *
 * IT UNDOES THE GESTURE FIRST. Holding a controller is a state the user is in; back
 * puts it down rather than throwing the whole dialog away, which is what "cancel" means
 * at that moment and what it does everywhere else in the app.
 */
static void section_cancel_cb(lv_event_t *e) {
    coop_dialog_t *dlg = lv_event_get_user_data(e);
    if (dlg->pad_held) {
        dlg->pad_held = false;
        refresh_pads(dlg);
        return;
    }
    lv_msgbox_close_async(dlg->msgbox);
}

/**
 * ACCEPT EVERYTHING AS IT STANDS. A long press works identically on the LG remote and
 * on a controller -- both send one OK/A press that LVGL times -- whereas a double
 * click has no LVGL event, needs a timing window this app has never tuned, and is
 * genuinely awkward on a remote. Suppressed while a controller is held so the gesture
 * that moves a pad cannot also start the session.
 */
static void accept_all_cb(lv_event_t *e) {
    coop_dialog_t *dlg = lv_event_get_user_data(e);
    if (dlg->pad_held) {
        return;
    }
    launch_session(dlg);
}

/* -------------------------------------------------------------- waking hosts */

/** Long enough for a cold boot from S5, which is the slow case this has to cover;
 *  a resume from S3 answers in a few seconds.
 *
 *  90 s was NOT long enough, and only looked it. Measured on PC2 on 2026-08-12, a
 *  cold boot answered serverinfo 85 s after the magic packet -- this timeout cleared
 *  it by five seconds, on a machine with nothing else to do. Any slower boot (a
 *  Windows update, a disk check, the second host booting at the same time) would have
 *  failed the whole co-op launch with both PCs on their way up. Sized off the same
 *  measurement as WOL_COLD_MS in worker/wol.c; keep the two together. */
#define COOP_WAKE_TIMEOUT_MS 180000
/** A host that is still booting must not be re-queried every frame. */
#define COOP_WAKE_POLL_MS 2500

static bool host_online(const uuidstr_t *uuid) {
    const SERVER_STATE *state = pcmanager_state(pcmanager, uuid);
    return state != NULL && (state->code & SERVER_STATE_ONLINE) != 0;
}

/* "ONLINE" HERE IS A CACHE WITH NO EXPIRY, AND IT HAS BEEN WRONG IN BOTH DIRECTIONS.
 *
 * pcmanager has no periodic refresh -- nothing polls hosts on a timer -- so
 * host_online() returns whatever the last explicit query left behind, which can be
 * minutes or hours old. Measured 2026-08-12: a launch pressed three seconds after PC2
 * went down read PC2 as ONLINE (stale) and PC1 as OFFLINE (also stale, opposite way).
 * It therefore sent the magic packet to the machine that was already up, sent nothing
 * to the one that was rebooting, and launched straight into it:
 *
 *     [WOL@136993] host=192.0.2.5    ... rc=0        <- PC1, which never needed it
 *     [WOL@137033] host=192.0.2.5    woke=YES after=38ms
 *     (no WOL line at all for 192.0.2.16, the host that was actually down)
 *     -> "Failed to launch session: cURL error: Timeout was reached (code -5)"
 *
 * And the wait loop could never recover from it, because it only re-queries hosts it
 * already believes are OFFLINE: a host wrongly believed online is never asked again.
 *
 * So the launch now asks both hosts and waits for the ANSWERS before deciding anything.
 * These are module-static rather than fields on the dialog on purpose -- the callback
 * arrives later, on the main loop, and must not hold a pointer to a dialog the user may
 * have cancelled in the meantime. */
static uuidstr_t g_wake_uuid[2];
static bool g_wake_fresh[2];
static bool g_wake_sent[2];

static void wake_update_done(int result, const char *error, const uuidstr_t *uuid, void *userdata) {
    (void) result;
    (void) error;
    (void) userdata;
    /* Fresh means ANSWERED, not "answered online". A host that is off answers this
     * query with a failure, and that failure is exactly the fact we are waiting for. */
    for (int i = 0; i < 2; ++i) {
        if (uuid != NULL && uuidstr_t_equals_t(uuid, &g_wake_uuid[i])) {
            g_wake_fresh[i] = true;
        }
    }
}

static void wake_ask_both(const uuidstr_t *primary, const uuidstr_t *secondary) {
    g_wake_uuid[0] = *primary;
    g_wake_uuid[1] = *secondary;
    g_wake_fresh[0] = g_wake_fresh[1] = false;
    pcmanager_request_update(pcmanager, primary, wake_update_done, NULL);
    pcmanager_request_update(pcmanager, secondary, wake_update_done, NULL);
}

static void wake_stop(coop_dialog_t *dlg) {
    if (dlg->wake_timer != NULL) {
        lv_timer_del(dlg->wake_timer);
        dlg->wake_timer = NULL;
    }
}

/** Hide the configuration rows while waking so the dialog says one thing at a time.
 *  They are only hidden, never destroyed: a wake that times out has to put the screen
 *  back exactly as it was so the choices are not lost. */
static void wake_sections_hidden(coop_dialog_t *dlg, bool hidden) {
    lv_obj_t *rows[] = {dlg->pad_box, dlg->orient_row, dlg->role_row};
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        if (rows[i] == NULL) {
            continue;
        }
        if (hidden) {
            lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ---- A PC busy with another app (issue #33) ------------------------------------------ */

typedef struct coop_busy_t {
    coop_dialog_t *dlg;          /* NULL once the co-op dialog is gone */
    uuidstr_t host[2];
    int app_id[2];               /* the resolved co-op app on each PC */
    unsigned busy_mask;          /* which PCs run something else */
    int pending;                 /* quit requests still in flight */
    bool failed;
    char error[160];
    lv_obj_t *confirm;
} coop_busy_t;

/** Enable or disable every start button together (dlg->start_btns). */
static void start_btns_disabled(coop_dialog_t *dlg, lv_obj_t *btns, bool disabled) {
    if (btns == NULL) {
        return;
    }
    for (uint16_t i = 0; i < (dlg->start_btns > 0 ? dlg->start_btns : 1); ++i) {
        if (disabled) {
            lv_btnmatrix_set_btn_ctrl(btns, i, LV_BTNMATRIX_CTRL_DISABLED);
        } else {
            lv_btnmatrix_clear_btn_ctrl(btns, i, LV_BTNMATRIX_CTRL_DISABLED);
        }
    }
}

static void busy_enable_start(coop_dialog_t *dlg) {
    start_btns_disabled(dlg, lv_msgbox_get_btns(dlg->msgbox), false);
}

static void busy_finish(coop_busy_t *busy) {
    coop_dialog_t *dlg = busy->dlg;
    if (dlg != NULL) {
        dlg->busy = NULL;
        if (busy->failed) {
            if (dlg->wake_label != NULL) {
                lv_label_set_text_fmt(dlg->wake_label, locstr("Could not quit the running app: %s"),
                                      busy->error[0] != '\0' ? busy->error : locstr("no reason given"));
            }
            wake_sections_hidden(dlg, false);
            busy_enable_start(dlg);
        } else {
            const coop_stream_plan_t plan = plan_for_pair(&busy->host[0], &busy->host[1]);
            lv_msgbox_close_async(dlg->msgbox);
            start_stream(dlg->global, &busy->host[0], &busy->host[1], busy->app_id[0], busy->app_id[1],
                         dlg->orientation, &plan);
        }
    }
    SDL_free(busy);
}

/* A FAILED QUIT IS RE-CHECKED BEFORE IT COUNTS. Apollo answers the quit only after the host's
 * session-end commands have run, and on PC2 those outlast the HTTP timeout: 09-27 the dialog said
 * "Could not quit the running app: cURL error: Timeout was reached" while PC2 had in fact gone FREE.
 * So ask the PC again; free (or already on the co-op app) means the quit worked. */
static void busy_recheck_done(int result, const char *error, const uuidstr_t *uuid, void *userdata) {
    (void) error;
    coop_busy_t *busy = userdata;
    int h = -1;
    for (int i = 0; i < 2; ++i) {
        if (uuid != NULL && uuidstr_t_equals_t(uuid, &busy->host[i])) {
            h = i;
        }
    }
    const int running = uuid != NULL ? pcmanager_server_current_app(pcmanager, uuid) : -1;
    if (result != 0 || h < 0 || (running != 0 && running != busy->app_id[h])) {
        busy->failed = true;
    }
    if (--busy->pending == 0) {
        busy_finish(busy);
    }
}

static void busy_quit_done(int result, const char *error, const uuidstr_t *uuid, void *userdata) {
    coop_busy_t *busy = userdata;
    if (result != 0 && uuid != NULL) {
        SDL_strlcpy(busy->error, error != NULL ? error : "", sizeof(busy->error));
        busy->pending++;
        pcmanager_request_update(pcmanager, uuid, busy_recheck_done, busy);
    } else if (result != 0) {
        busy->failed = true;
        SDL_strlcpy(busy->error, error != NULL ? error : "", sizeof(busy->error));
    }
    if (--busy->pending == 0) {
        busy_finish(busy);
    }
}

static void busy_confirm_cb(lv_event_t *e) {
    coop_busy_t *busy = lv_event_get_user_data(e);
    lv_obj_t *confirm = lv_event_get_current_target(e);
    const uint16_t picked = lv_btnmatrix_get_selected_btn(lv_msgbox_get_btns(confirm));
    busy->confirm = NULL;
    lv_msgbox_close_async(confirm);
    coop_dialog_t *dlg = busy->dlg;
    if (dlg == NULL) {
        SDL_free(busy);
        return;
    }
    if (picked != 0) {
        /* Cancel: nothing was touched; Start works again. */
        dlg->busy = NULL;
        busy_enable_start(dlg);
        SDL_free(busy);
        return;
    }
    busy->pending = 1;           /* held until every request is queued, so none can finish early */
    for (int h = 0; h < 2; ++h) {
        if ((busy->busy_mask & (1u << h)) && pcmanager_quitapp(pcmanager, &busy->host[h], busy_quit_done, busy)) {
            busy->pending++;
        }
    }
    if (dlg->wake_label != NULL) {
        lv_label_set_text(dlg->wake_label, locstr("Closing the running app..."));
        wake_sections_hidden(dlg, false);
    }
    busy_quit_done(0, NULL, NULL, busy);
}

/** True when a PC runs a different app and the user is now being asked about it. */
static bool busy_ask(coop_dialog_t *dlg, const coop_app_resolution_t *resolution) {
    unsigned mask = 0;
    for (int h = 0; h < 2; ++h) {
        if (resolution->running_id[h] != 0 && resolution->running_id[h] != resolution->app_id[h]) {
            mask |= 1u << h;
        }
    }
    if (mask == 0) {
        return false;
    }
    coop_busy_t *busy = SDL_calloc(1, sizeof(coop_busy_t));
    if (busy == NULL) {
        return false;
    }
    busy->dlg = dlg;
    busy->busy_mask = mask;
    for (int h = 0; h < 2; ++h) {
        busy->host[h] = resolution->host[h];
        busy->app_id[h] = resolution->app_id[h];
    }
    char text[512];
    size_t n = 0;
    for (int h = 0; h < 2 && n < sizeof(text); ++h) {
        if (!(mask & (1u << h))) {
            continue;
        }
        n += (size_t) snprintf(text + n, sizeof(text) - n, locstr("%s is already running \"%s\".\n"),
                               resolution->host_name[h][0] != '\0' ? resolution->host_name[h] : locstr("A PC"),
                               resolution->running_name[h][0] != '\0' ? resolution->running_name[h] : locstr("another app"));
    }
    if (n < sizeof(text)) {
        snprintf(text + n, sizeof(text) - n, "%s",
                 locstr("\nCo-op cannot start while it runs. Quit it there and start co-op?"));
    }
    static const char *btns[] = {translatable("Quit it and start"), translatable("Cancel"), ""};
    busy->confirm = lv_msgbox_create_i18n(NULL, locstr("App already running"), text, btns, false);
    lv_obj_add_event_cb(busy->confirm, busy_confirm_cb, LV_EVENT_VALUE_CHANGED, busy);
    lv_obj_center(busy->confirm);
    dlg->busy = busy;
    return true;
}

/** Does this host build the co-op pane in itself (Umbra, serverinfo UmbraCaps bit 2)? Then a missing
 *  pane is not something the user should add by hand: Umbra adds it whenever its virtual display
 *  driver works, so the fault to name is the driver. A stock host must still be told to add it. */
static bool host_builds_coop_pane(const uuidstr_t *uuid) {
    const pclist_t *node = pcmanager_node(pcmanager, uuid);
    return node != NULL && node->server != NULL && node->server->umbraCapsKnown &&
           (node->server->umbraCaps & UMBRA_CAP_COOP_PANE) != 0;
}

static void coop_app_resolution_done(coop_app_resolution_t *resolution, bool success,
                                     int failed_host, int code, const char *detail,
                                     void *userdata) {
    coop_dialog_t *dlg = userdata;
    /* The resolver frees itself after this callback. Clear the dialog's non-owning
     * pointer first so closing the dialog cannot try to cancel it a second time. */
    dlg->app_resolution = NULL;
    if (!success) {
        const char *who = failed_host >= 0 && failed_host < 2
                          ? resolution->host_name[failed_host] : locstr("One PC");
        if (dlg->wake_label != NULL) {
            if (code == 0 && failed_host >= 0 && failed_host < 2 &&
                host_builds_coop_pane(&resolution->host[failed_host])) {
                lv_label_set_text_fmt(
                        dlg->wake_label,
                        locstr("Umbra on %s has no co-op pane because its virtual display driver is not "
                               "working. Reinstall Umbra on that PC, restart it, then try again."), who);
            } else if (code == 0) {
                lv_label_set_text_fmt(
                        dlg->wake_label,
                        locstr("%s does not have an application named exactly '%s'. Add it in "
                               "Umbra or Sunshine, then try again."),
                        who, COOP_SESSION_APP_NAME);
            } else if (detail != NULL && detail[0] != '\0') {
                lv_label_set_text_fmt(
                        dlg->wake_label,
                        locstr("Could not load the app list from %s (%s). Check that it is paired "
                               "and Umbra or Sunshine is reachable, then try again."),
                        who, detail);
            } else {
                lv_label_set_text_fmt(
                        dlg->wake_label,
                        locstr("Could not load the app list from %s. Check that it is paired and "
                               "Umbra or Sunshine is reachable, then try again."), who);
            }
        }
        wake_sections_hidden(dlg, false);
        start_btns_disabled(dlg, lv_msgbox_get_btns(dlg->msgbox), false);
        return;
    }

    const int primary_app_id = resolution->app_id[0];
    const int secondary_app_id = resolution->app_id[1];
    /* A PC ALREADY RUNNING A DIFFERENT APP REFUSES THE LAUNCH ("Bad status 400: An app is already
     * running on this host") -- on 09-27 PC2 still held its Virtual Display session and both co-op
     * pairs with it "failed to open" with nothing on screen saying why (issue #33). Ask first. */
    if (busy_ask(dlg, resolution)) {
        return;
    }
    const coop_stream_plan_t plan = plan_for_pair(&dlg->wake_primary, &dlg->wake_secondary);
    lv_msgbox_close_async(dlg->msgbox);
    start_stream(dlg->global, &dlg->wake_primary, &dlg->wake_secondary,
                 primary_app_id, secondary_app_id, dlg->orientation, &plan);
}

static void begin_app_resolution(coop_dialog_t *dlg) {
    if (dlg->app_resolution != NULL) {
        return;
    }
    if (dlg->wake_label != NULL) {
        const pclist_t *primary = pcmanager_node(pcmanager, &dlg->wake_primary);
        const pclist_t *secondary = pcmanager_node(pcmanager, &dlg->wake_secondary);
        lv_label_set_text_fmt(dlg->wake_label, locstr("Checking '%s' on %s and %s..."),
                              COOP_SESSION_APP_NAME, host_display_name(primary),
                              host_display_name(secondary));
    }
    dlg->app_resolution = coop_app_resolution_start(
            dlg->global, &dlg->wake_primary, &dlg->wake_secondary,
            coop_app_resolution_done, dlg);
    if (dlg->app_resolution == NULL) {
        if (dlg->wake_label != NULL) {
            lv_label_set_text(dlg->wake_label,
                              locstr("Could not check the co-op applications. Try again."));
        }
        wake_sections_hidden(dlg, false);
        start_btns_disabled(dlg, lv_msgbox_get_btns(dlg->msgbox), false);
    }
}

static void wake_failed(coop_dialog_t *dlg) {
    wake_stop(dlg);
    const bool p = host_online(&dlg->wake_primary);
    const bool s = host_online(&dlg->wake_secondary);
    /* NAME THE MACHINE THAT DID NOT COME BACK. "Could not start" sends someone to
     * look at the wrong PC; this is the difference between a two-minute check and an
     * evening of guessing. */
    const char *who = !p && !s ? locstr("Neither PC")
                               : (!p ? dlg->machine_name[dlg->primary_machine]
                                     : dlg->machine_name[1 - dlg->primary_machine]);
    if (dlg->wake_label != NULL) {
        lv_label_set_text_fmt(dlg->wake_label,
                              locstr("%s did not wake. Check that Wake-on-LAN is enabled "
                                     "on it, then try again."), who);
    }
    wake_sections_hidden(dlg, false);
    start_btns_disabled(dlg, lv_msgbox_get_btns(dlg->msgbox), false);
}

static void wake_tick_cb(lv_timer_t *timer) {
    coop_dialog_t *dlg = timer->user_data;
    const bool p = host_online(&dlg->wake_primary);
    const bool s = host_online(&dlg->wake_secondary);
    /* Until both have ANSWERED since Start was pressed, `p` and `s` are the old cache
     * and must not be acted on -- neither to launch, nor to decide who needs a packet. */
    const bool answered = g_wake_fresh[0] && g_wake_fresh[1];

    if (answered && p && s) {
        wake_stop(dlg);
        begin_app_resolution(dlg);
        return;
    }

    /* SEND THE PACKET ONCE A HOST IS CONFIRMED DOWN, not before. This used to happen in
     * wake_begin() off the cached state, which is how the magic packet went to the wrong
     * machine; now nothing is sent until that host has actually failed a query. */
    if (answered) {
        const uuidstr_t *hosts[2] = {&dlg->wake_primary, &dlg->wake_secondary};
        const bool up[2] = {p, s};
        for (int i = 0; i < 2; ++i) {
            if (!up[i] && !g_wake_sent[i]) {
                g_wake_sent[i] = true;
                pcmanager_send_wol(pcmanager, hosts[i], NULL, NULL);
                pin_autoentry_mark_woken(hosts[i]);
                commons_log_info("COOP", "host %d confirmed offline -- magic packet sent", i);
            }
        }
    }
    /* SIGNED difference, so the comparison stays correct across the 32-bit tick
     * wrap. `lv_tick_get() > deadline` is the version that hangs forever once a
     * device has been up for 49 days. */
    if ((int32_t) (lv_tick_get() - dlg->wake_deadline) >= 0) {
        wake_failed(dlg);
        return;
    }

    /* SAY HOW LONG IT HAS BEEN, because the wait is now up to three minutes and a cold
     * boot spends its first ~45 s in BIOS POST, where nothing on the network answers.
     * A dialog that shows one unchanging sentence for that long is indistinguishable
     * from one that has hung, and that is how a wake that was working gets cancelled by
     * hand. Naming the host still missing is the other half: when the first PC comes up
     * the message narrows, which shows progress no counter can. */
    if (dlg->wake_label != NULL) {
        const int32_t started = (int32_t) dlg->wake_deadline - COOP_WAKE_TIMEOUT_MS;
        const int secs = (int) (((int32_t) lv_tick_get() - started) / 1000);
        char msg[160];
        /* Say what is actually happening. Before both hosts have answered we are not
         * waking anything yet, and claiming otherwise is what made the old dialog's
         * "Waking..." unfalsifiable -- it said the same thing whether a packet had gone
         * out or not. */
        const char *what = !answered ? locstr("Checking both PCs...")
                           : (!p && !s) ? locstr("Waking both PCs and signing in...")
                                        : locstr("Waking the other PC and signing in...");
        snprintf(msg, sizeof(msg), "%s (%ds)", what, secs);
        if (strcmp(lv_label_get_text(dlg->wake_label), msg) != 0) {
            lv_label_set_text(dlg->wake_label, msg);
        }
    }

    if ((int32_t) (lv_tick_get() - dlg->wake_next_poll) < 0) {
        return;                                 /* not time to re-query yet */
    }
    dlg->wake_next_poll = lv_tick_get() + COOP_WAKE_POLL_MS;
    /* RE-QUERY, don't just wait. A machine that has come back is not noticed until
     * something asks it; discovery alone can take far longer than a person is willing
     * to sit in front of a "waking" message.
     *
     * BOTH hosts, every poll -- including ones that read online. That asymmetry is the
     * bug above: a host believed online was never asked again, so a machine that went
     * down just before the launch stayed "up" forever and the session was launched into
     * it. Re-asking an online host costs one serverinfo per 2.5 s. */
    pcmanager_request_update(pcmanager, &dlg->wake_primary, wake_update_done, NULL);
    pcmanager_request_update(pcmanager, &dlg->wake_secondary, wake_update_done, NULL);
}

/**
 * Ask both machines whether they are really there, wake whichever is not, and hold the
 * dialog until both have answered and are up.
 *
 * @return ALWAYS true -- the launch is always deferred to wake_tick_cb, because the
 *         cached online state this used to branch on has no expiry and has been wrong
 *         in both directions at once. The caller must not launch.
 */
static bool wake_begin(coop_dialog_t *dlg) {
    const bool p = host_online(&dlg->wake_primary);
    const bool s = host_online(&dlg->wake_secondary);

    /* ARM THE LOGIN FOR BOTH HOSTS, ALWAYS -- **"ONLINE" DOES NOT MEAN "USABLE"**.
     *
     * This used to be done only for the machines we were about to WoL, which quietly
     * assumed the only way to reach a lock screen is to have been asleep. It is not.
     * A PC that is awake, signed in and LOCKED with its display timed out answers
     * Apollo's status query perfectly well, so host_online() says true, we skip it
     * here -- and it then streams its lock screen for the whole session. From the
     * couch that is indistinguishable from a machine that never woke, which is
     * exactly how it was reported on 2026-08-08: "both PCs were still asleep".
     * Measured that day on PC2: awake, `explorer` AND `LogonUI` both running (signed
     * in but locked), display blanked, answering ping and SSH the entire time.
     *
     * Worth being precise about the states, because two of them look identical from
     * the couch and only one of them is sleep:
     *   - S3          CPU off, RAM refreshed, NIC dark   -> needs the magic packet
     *   - awake+locked everything running, NIC up, screen off -> needs only the PIN
     * PC2 has no S0ix (firmware does not offer it), so there is no third case where
     * it is "asleep" yet still answering the network.
     *
     * THE TRADE-OFF, STATED: Eclipse cannot tell locked from unlocked -- Apollo does
     * not report it -- so a host that was already unlocked now gets the credential
     * typed at it too, which lands in whatever has focus. Accepted because
     * pin_autoentry is opt-in and OFF by default, and because at the start of a co-op
     * session nobody is sitting at either PC, so locked is the overwhelming case.
     * pin_autoentry_arm() is itself a no-op when the feature is off. */
    pin_autoentry_arm(&dlg->wake_primary);
    pin_autoentry_arm(&dlg->wake_secondary);

    /* ALWAYS DEFER, even when both look up. The cached state has no expiry (see
     * wake_ask_both above) and reading it here is what launched a session into a
     * rebooting PC2 while sending the magic packet to a PC1 that was already awake.
     * The tick decides instead, once both hosts have actually answered -- which for two
     * healthy machines is one poll, so the cost of the extra certainty is a moment on
     * "Checking both PCs...". */
    (void) p;
    (void) s;
    g_wake_sent[0] = g_wake_sent[1] = false;
    wake_ask_both(&dlg->wake_primary, &dlg->wake_secondary);

    wake_sections_hidden(dlg, true);
    if (dlg->wake_label == NULL) {
        dlg->wake_label = lv_label_create(lv_msgbox_get_content(dlg->msgbox));
        lv_label_set_long_mode(dlg->wake_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(dlg->wake_label, LV_PCT(100));
    }
    lv_obj_clear_flag(dlg->wake_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(dlg->wake_label, locstr("Checking both PCs..."));
    /* The Start button is disabled rather than hidden: pressing it again would send a
     * second wake and start a second wait over the top of this one. */
    start_btns_disabled(dlg, lv_msgbox_get_btns(dlg->msgbox), true);

    dlg->wake_deadline = lv_tick_get() + COOP_WAKE_TIMEOUT_MS;
    dlg->wake_next_poll = lv_tick_get() + COOP_WAKE_POLL_MS;
    wake_stop(dlg);
    dlg->wake_timer = lv_timer_create(wake_tick_cb, 500, dlg);
    return true;
}

/* ------------------------------------------------------------------ launch */

/**
 * ASK WHAT THE GPU IS, ONCE, WHEN NOBODY CAN TELL US.
 *
 * Co-op has to know each host's encoder family before it can choose a codec the two panes can
 * share, and a stock Apollo or Sunshine host does not report it. The alternatives were both
 * worse than asking: guessing from capability flags misreads a pre-Turing NVIDIA as AMD and an
 * Intel Arc as NVIDIA, and defaulting silently is how someone ends up staring at a corrupt
 * picture with nothing on screen that mentions GPUs at all.
 *
 * ONE QUESTION PER MACHINE, AND ONLY THE FIRST TIME. The answer goes into hosts.ini beside the
 * pad list, so it is asked once per PC for the life of the install, and can be changed in
 * Settings afterwards. A host that later reports its own encoder overrules the answer.
 *
 * Returns true when it has taken over: the launch continues from the button handler.
 */
static bool gpu_ask_begin(coop_dialog_t *dlg) {
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        if (cur->server == NULL) {
            continue;
        }
        /* NEVER A PC THAT HAS NOT BEEN PAIRED. hosts.ini keeps only known (paired) PCs, so an
         * answer about a merely discovered one was lost at the next start and asked again. A PC
         * is asked once it is paired. (Umbra 0.5.0 does not send EncoderVendor, so that includes
         * Umbra hosts -- measured 2026-10-10: both paired Umbra PCs show user answers.) */
        if (!cur->known) {
            continue;
        }
        if (cur->server->encoderVendorKnown || cur->gpu_vendor != COOP_GPU_UNKNOWN) {
            continue;                       /* already answered, by the host or by the user */
        }
        /* EVERY KNOWN PC, NOT JUST THIS PAIRING. A machine that has ever been connected can be
         * put in a column at any time, so leaving it unanswered only moves the question to a
         * worse moment. An unanswered PC is picked up again on the next open, which is what
         * makes a newly paired machine ask on its own. */
        dlg->gpu_ask_uuid = cur->id;
        dlg->gpu_ask_active = true;
        static const char *gpu_btns[] = {translatable("NVIDIA"), translatable("AMD"),
                                         translatable("Intel"), translatable("Other"), ""};
        char text[320];
        snprintf(text, sizeof(text),
                 locstr("Which graphics card is in %s?\n\n"
                        "Co-op has to build one picture out of both PCs, and two different makes "
                        "of GPU cannot share it the same way two of the same make can. This is "
                        "asked once per PC and can be changed later in Settings > Co-op."),
                 cur->server->hostname != NULL ? cur->server->hostname : "this PC");
        dlg->gpu_msgbox = lv_msgbox_create_i18n(NULL, locstr("Graphics card"), text, gpu_btns, false);
        lv_obj_set_user_data(dlg->gpu_msgbox, dlg);
        lv_obj_add_event_cb(dlg->gpu_msgbox, gpu_ask_answered_cb, LV_EVENT_VALUE_CHANGED, dlg);
        lv_obj_center(dlg->gpu_msgbox);
        return true;
    }
    dlg->gpu_ask_active = false;
    return false;
}

static void gpu_ask_answered_cb(lv_event_t *e) {
    coop_dialog_t *dlg = lv_event_get_user_data(e);
    lv_obj_t *msgbox = lv_event_get_current_target(e);
    const uint16_t picked = lv_btnmatrix_get_selected_btn(lv_msgbox_get_btns(msgbox));
    static const coop_gpu_vendor_t by_button[] = {COOP_GPU_NVIDIA, COOP_GPU_AMD,
                                                  COOP_GPU_INTEL, COOP_GPU_OTHER};
    if (dlg->gpu_ask_active && picked < (uint16_t) (sizeof(by_button) / sizeof(by_button[0]))) {
        /* STORED BEFORE ANYTHING ELSE HAPPENS. Saved straight to hosts.ini by the setter, so a
         * launch that then fails for an unrelated reason does not ask the same question again.
         * A dismissal stores nothing, which is deliberate: the PC stays unanswered and the
         * question comes back next time rather than a guess being recorded as an answer. */
        pcmanager_set_host_gpu(pcmanager, &dlg->gpu_ask_uuid, (int) by_button[picked]);
    }
    dlg->gpu_ask_active = false;
    dlg->gpu_msgbox = NULL;
    lv_msgbox_close_async(msgbox);
    /* Everything the answer feeds -- the plan, the banner, the codec -- is re-derived before
     * anything else happens, so the dialog behind this one is already telling the truth by the
     * time it is visible again. */
    refresh_machine_facts(dlg);
    refresh_mode_banner(dlg);
    if (gpu_ask_begin(dlg)) {
        return;                             /* the other machine is unanswered too */
    }
    /* Only a question raised BY a launch resumes one. Asked when the dialog opened, the user
     * is simply back at the dialog, free to change anything before pressing Start. */
    if (dlg->gpu_ask_then_launch) {
        dlg->gpu_ask_then_launch = false;
        launch_session(dlg);
    }
}

static void hub_route_error_dialog_cb(lv_event_t *e) {
    lv_msgbox_close_async(lv_event_get_current_target(e));
}

static void show_hub_route_failure(const coop_dialog_t *dlg, size_t pad,
                                   int machine, bool address_missing) {
    const int pi_idx = dlg->pi_of[pad];
    const char *hub = pi_idx >= 0 ? dlg->pi_scan.pads[pi_idx].pi_addr : locstr("unknown hub");
    const char *target = machine >= 0 && machine < 2
                         ? dlg->machine_name[machine] : locstr("unknown PC");
    const pclist_t *target_node = machine >= 0 && machine < 2
                                  ? pcmanager_node(pcmanager, &dlg->machine_uuid[machine]) : NULL;
    const char *target_addr = target_node != NULL && target_node->server != NULL
                              ? target_node->server->serverInfo.address : NULL;
    char message[512];
    if (address_missing) {
        snprintf(message, sizeof(message),
                 locstr("The controller hub at %s could not route '%s' to %s because Eclipse "
                        "does not have a usable address for that PC. Refresh or reconnect the "
                        "PC, then choose Start co-op to retry. No stream was started and these "
                        "choices were not saved."),
                 hub, dlg->assign[pad].label, target);
    } else {
        snprintf(message, sizeof(message),
                 locstr("The controller hub at %s did not confirm routing '%s' to %s (%s). Check "
                        "that the hub is online and reachable, then choose Start co-op to "
                        "retry. No stream was started and these choices were not saved."),
                 hub, dlg->assign[pad].label, target,
                 target_addr != NULL ? target_addr : locstr("no address"));
    }
    commons_log_warn("COOP", "%s", message);
    static const char *buttons[] = {translatable("OK"), ""};
    lv_obj_t *dialog = lv_msgbox_create_i18n(NULL, locstr("Controller route was not applied"),
                                             message, buttons, false);
    lv_obj_add_event_cb(dialog, hub_route_error_dialog_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(dialog);
}

/** The session's assigned mouse [0] and keyboard [1] as panes (0 primary, 1 peer), -1 when they
 *  follow the pointer. Written at launch, read by start_stream. */
static int g_kbm_pane[2] = {-1, -1};

static void launch_session(coop_dialog_t *dlg) {
    if (!dlg->hosts_ok) {
        return;
    }
    /* Before either stream is armed: both panes read it (the secondary through the environment). */
    app_launch_choose_host_monitor(dlg->monitor_off_choice);
    /* THE ONE PLACE a machine index becomes a pane. Everywhere above, a controller
     * is assigned to machine 0 or machine 1; the stream uses primary/secondary, so
     * conversion happens once against player 1. */
    const uuidstr_t *primary = &dlg->machine_uuid[dlg->primary_machine];
    const uuidstr_t *secondary = &dlg->machine_uuid[1 - dlg->primary_machine];

    /* ROUTE THE PI'S PADS. A Pi pad is not delivered by this process at all -- the Pi
     * forwards it straight to a host -- so the only way to honour the choice made on
     * this screen is to tell the Pi which machine to send to, before the stream starts.
     * This is a gate, not best effort: no acknowledgement means no stream and no
     * in-memory/persisted choices claiming that the route took effect. Validate every
     * destination before changing any hub, then require the exact ACK for each pad. */
    for (size_t i = 0; i < dlg->pad_count; ++i) {
        const int pi_idx = dlg->pi_of[i];
        if (pi_idx < 0) {
            continue;
        }
        /* A controller goes to exactly one PC (there is no "Both" any more), which is also all
         * the hub protocol can carry: one address. */
        const int machine = dlg->assign[i].route;
        const pclist_t *node = pcmanager_node(pcmanager, &dlg->machine_uuid[machine]);
        const char *addr = node != NULL && node->server != NULL
                           ? node->server->serverInfo.address : NULL;
        if (addr == NULL || addr[0] == '\0') {
            show_hub_route_failure(dlg, i, machine, true);
            return;
        }
    }
    for (size_t i = 0; i < dlg->pad_count; ++i) {
        const int pi_idx = dlg->pi_of[i];
        if (pi_idx < 0) {
            continue;
        }
        const int machine = dlg->assign[i].route;
        const pclist_t *node = pcmanager_node(pcmanager, &dlg->machine_uuid[machine]);
        const char *addr = node != NULL && node->server != NULL
                           ? node->server->serverInfo.address : NULL;
        if (addr == NULL || addr[0] == '\0') {
            show_hub_route_failure(dlg, i, machine, true);
            return;
        }
        if (!coop_pi_pads_route(dlg->pi_scan.pads[pi_idx].pi_addr,
                                dlg->pi_scan.pads[pi_idx].name, addr,
                                COOP_PI_ROUTE_ACK_TIMEOUT_MS)) {
            show_hub_route_failure(dlg, i, machine, false);
            return;
        }
    }

    /* Only acknowledged external routes may become the session's configuration.
     * The normal settings flush preserves these values on clean shutdown. */
    app_configuration->coop_orientation = dlg->orientation;
    settings_set_string(&app_configuration->coop_primary_uuid, primary->data);
    settings_set_string(&app_configuration->coop_secondary_uuid, secondary->data);
    for (size_t i = 0; i < dlg->pad_count; ++i) {
        const int gs_id = dlg->gs_id_of[i];
        if (gs_id < 0 || gs_id >= 4) {
            continue;                       /* Pi pads do not enter coop_input */
        }
        app_configuration->coop_pad_pane[gs_id] =
                dlg->assign[i].route == dlg->primary_machine
                ? COOP_PAD_PRIMARY : COOP_PAD_SECONDARY;
    }
    /* The mouse and keyboard, when assigned (#37): machine -> pane, like a pad's route above.
     * Kept for start_stream, which the PIN/HDR relaunch also reaches without this dialog. */
    g_kbm_pane[0] = g_kbm_pane[1] = -1;
    for (size_t i = 0; i < dlg->pad_count; ++i) {
        if (dlg->kbm_of[i] >= 0) {
            g_kbm_pane[dlg->kbm_of[i]] = dlg->assign[i].route == dlg->primary_machine ? 0 : 1;
        }
    }

    /* BACKSTOP ONLY. The question is normally asked when the dialog OPENS, so that the mode
     * banner is true before the user commits and Start does what it says. This catches the one
     * case open cannot: a PC swapped into a column afterwards. Still ahead of the wake step --
     * a question should not be queued behind a machine that takes three minutes to boot. */
    dlg->gpu_ask_then_launch = true;
    if (gpu_ask_begin(dlg)) {
        return;
    }
    dlg->gpu_ask_then_launch = false;

    /* WAKE ANYTHING THAT IS ASLEEP, THEN LAUNCH. Returns true when it took over; the
     * timer finishes the launch once both machines answer. */
    dlg->wake_primary = *primary;
    dlg->wake_secondary = *secondary;
    if (wake_begin(dlg)) {
        return;
    }

    begin_app_resolution(dlg);
}

/**
 * THE LAUNCH ITSELF, with no dialog attached. Split out from launch_session so the
 * PIN/HDR relaunch can come back to a co-op session instead of a single app -- see
 * coop_dialog_relaunch_last().
 */
static void start_stream(app_t *global, const uuidstr_t *primary, const uuidstr_t *secondary,
                         int primary_app_id, int secondary_app_id, int orientation,
                         const coop_stream_plan_t *plan) {
    /* ARM THE MODE ON DISK. The orientation is not just a number the session carries:
     * `/tmp/coop-vmirror` selects tile columns and `/tmp/coop-hh-tiles` selects tile
     * rows, and a session that writes neither runs the retired stacked-halves path. This
     * is the moment the choice is known, and every launch writes the switch it wants AND
     * clears the other -- the rule docs/coop-modes.md rests on. */
    /* THE CODEC DECIDES WHAT SPLITS ARE POSSIBLE, so it is resolved before the mode is armed.
     * H.264 cannot express a left/right split at all -- see coop_orientation_for_mode(). */
    orientation = coop_orientation_for_mode(orientation, plan->mode);

    coop_session_t session;
    SDL_memset(&session, 0, sizeof(session));
    SDL_strlcpy(session.primary_uuid, primary->data, sizeof(session.primary_uuid));
    SDL_strlcpy(session.secondary_uuid, secondary->data, sizeof(session.secondary_uuid));
    session.primary_app_id = primary_app_id;
    session.secondary_app_id = secondary_app_id;
    session.orientation = orientation;
    /* THE CODEC PATH IS DECIDED HERE AND NOWHERE ELSE. Snapshotted into the session for
     * this process, and published into the environment for the secondary -- which is
     * spawned later, from here, and has no dialog of its own to work it out. The two
     * halves disagreeing about the codec is not a degraded picture, it is a splice that
     * cannot happen at all. */
    session.stream_mode = (int) plan->mode;
    session.gpu_hevc = plan->gpu_hevc;
    /* Tri-state on the wire between these two: 1 allow, 2 deny. The deny case has to be
     * carried EXPLICITLY -- an absent answer means "nobody decided", which resolves to
     * the user's own HDR setting, and that is exactly the wrong default for a pairing
     * that just determined one of its machines cannot do 10-bit. */
    session.hdr_allowed = plan->hdr_allowed ? 1 : 2;
    coop_stream_mode_publish(plan->mode, plan->hdr_allowed);
    /* PUBLISH BEFORE ARMING. coop_tunables_arm() -> coop_tunables_apply() writes the dial
     * sentinels, and app start in the secondary resolves its column from what is published
     * here; arming first left a PREVIOUS co-op session (nothing clears one at teardown) as
     * the answer, so a fresh dialog choice could write the other orientation's column. */
    coop_session_set(&session);
    coop_tunables_arm(orientation);

    /* IS ONE OF THE TWO HOSTS THIS PC? Then the pointer must hand over the real cursor at that
     * pane rather than send it anything (coop_pointer_set_local_pane). Decided here, where both
     * hosts are known; never true outside Windows. */
    {
        const pclist_t *p = pcmanager_node(pcmanager, primary);
        const pclist_t *s = pcmanager_node(pcmanager, secondary);
        int local_pane = -1;
        if (p != NULL && p->server != NULL && local_host_is_this_pc(p->server->serverInfo.address)) {
            local_pane = 0;
        } else if (s != NULL && s->server != NULL && local_host_is_this_pc(s->server->serverInfo.address)) {
            local_pane = 1;
        }
        coop_pointer_set_local_pane(local_pane);
        /* AND ITS SOUND GOES THROUGH THE MIX like the other PC's: decided now, before either launch, because
         * Eclipse's output must be pinned to the real speakers before this PC's host moves the default. */
        coop_audio_prepare_local_host(local_pane >= 0);
        coop_pointer_set_pins(app_configuration->coop_kbm_assign ? g_kbm_pane[0] : -1,
                              app_configuration->coop_kbm_assign ? g_kbm_pane[1] : -1);
    }

    /* Co-op is armed for THIS run. It used to be armed only by a webOS launch
     * parameter, which meant the app had to be relaunched from a shell to enter
     * co-op at all -- the single biggest reason it had no UI. */
    app_launch_set_coop_requested(true);

    /* HAND THE SECONDARY'S ARM OVER THE PROCESS BOUNDARY. The secondary is a separate
     * headless process (spawned later, so it inherits this environment) with its own
     * globals -- pin_autoentry_arm() here is invisible to it, and without this the
     * woken second PC streams its lock screen for the whole session. Written every
     * launch, "0" included: a stale "1" from a previous run would type a PIN into a
     * machine that was already logged in, i.e. into whatever game is on screen. Taking
     * the arm (rather than reading it) is what keeps that true across launches. */
    /* Read woken BEFORE take(), which consumes the arm and with it the flag. */
    setenv("COOP_PIN_WOKEN", pin_autoentry_was_woken(secondary) ? "1" : "0", 1);
    setenv("COOP_PIN_AUTOENTRY", pin_autoentry_take(secondary) ? "1" : "0", 1);

    static char coop_app_name[] = COOP_SESSION_APP_NAME;
    streaming_scene_arg_t args = {
            .global = global,
            .uuid = *primary,
            .app = {.name = coop_app_name, .id = session.primary_app_id, .hdr = 0, .next = NULL},
    };
    app_ui_t *ui = &global->ui;
    lv_fragment_t *fragment = lv_fragment_create(&streaming_controller_class, &args);
    lv_obj_t *const *container = lv_fragment_get_container(lv_fragment_manager_get_top(ui->fm));
    lv_fragment_manager_push(ui->fm, fragment, container);
}

typedef struct coop_relaunch_t {
    app_t *global;
    uuidstr_t host[2];
    int orientation;
} coop_relaunch_t;

static coop_app_resolution_t *g_relaunch_resolution;

static void relaunch_error_dialog_cb(lv_event_t *event) {
    lv_msgbox_close_async(lv_event_get_current_target(event));
}

static void coop_relaunch_resolution_done(coop_app_resolution_t *resolution, bool success,
                                          int failed_host, int code, const char *detail,
                                          void *userdata) {
    coop_relaunch_t *relaunch = userdata;
    g_relaunch_resolution = NULL;
    if (success) {
        commons_log_info("COOP", "relaunching co-op session (orientation %d, app ids %d/%d)",
                         relaunch->orientation, resolution->app_id[0], resolution->app_id[1]);
        const coop_stream_plan_t plan = plan_for_pair(&relaunch->host[0], &relaunch->host[1]);
        start_stream(relaunch->global, &relaunch->host[0], &relaunch->host[1],
                     resolution->app_id[0], resolution->app_id[1], relaunch->orientation, &plan);
    } else {
        const char *who = failed_host >= 0 && failed_host < 2
                          ? resolution->host_name[failed_host] : locstr("One PC");
        char message[512];
        if (code == 0) {
            snprintf(message, sizeof(message),
                     locstr("%s no longer has an application named exactly '%s'. Add it in "
                            "Umbra or Sunshine, then start co-op again."),
                     who, COOP_SESSION_APP_NAME);
        } else if (detail != NULL && detail[0] != '\0') {
            snprintf(message, sizeof(message),
                     locstr("Could not load the app list from %s (%s). Check Umbra or Sunshine, "
                            "then start co-op again."), who, detail);
        } else {
            snprintf(message, sizeof(message),
                     locstr("Could not load the app list from %s. Check Umbra or Sunshine, "
                            "then start co-op again."), who);
        }
        commons_log_warn("COOP", "%s", message);
        static const char *buttons[] = {translatable("OK"), ""};
        lv_obj_t *dialog = lv_msgbox_create_i18n(NULL, locstr("Co-op app unavailable"),
                                                 message, buttons, false);
        lv_obj_add_event_cb(dialog, relaunch_error_dialog_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_center(dialog);
    }
    SDL_free(relaunch);
}

bool coop_dialog_relaunch_last(app_t *global) {
    if (global == NULL || app_configuration == NULL) {
        return false;
    }
    if (g_relaunch_resolution != NULL) {
        return true;
    }
    const char *want[2] = {app_configuration->coop_primary_uuid,
                           app_configuration->coop_secondary_uuid};
    uuidstr_t resolved[2];
    for (int i = 0; i < 2; ++i) {
        if (want[i] == NULL || want[i][0] == '\0') {
            return false;
        }
        /* Resolve against the live list rather than trusting the stored string: a PC
         * that was unpaired since the last run must fail here, not at connect time. */
        const pclist_t *found = NULL;
        for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
            if (cur->server != NULL && uuidstr_t_equals_s(&cur->id, want[i])) {
                found = cur;
                break;
            }
        }
        if (found == NULL) {
            return false;
        }
        resolved[i] = found->id;
    }
    coop_relaunch_t *relaunch = SDL_calloc(1, sizeof(*relaunch));
    if (relaunch == NULL) {
        return false;
    }
    relaunch->global = global;
    relaunch->host[0] = resolved[0];
    relaunch->host[1] = resolved[1];
    relaunch->orientation = app_configuration->coop_orientation;
    g_relaunch_resolution = coop_app_resolution_start(global, &resolved[0], &resolved[1],
                                                       coop_relaunch_resolution_done, relaunch);
    if (g_relaunch_resolution == NULL) {
        SDL_free(relaunch);
        return false;
    }
    return true;
}

/* -------------------------------------------------------------- scaffolding */

static void dialog_deleted_cb(lv_event_t *e) {
    coop_dialog_t *dlg = lv_event_get_user_data(e);
    if (dlg->rescan != NULL) {
        lv_timer_del(dlg->rescan);
        dlg->rescan = NULL;
    }
    /* The wake timer holds dlg. Closing the dialog mid-wake (BACK, or the launch it
     * finished with) would otherwise leave it firing into freed memory. */
    wake_stop(dlg);
    if (dlg->app_resolution != NULL) {
        coop_app_resolution_destroy(dlg->app_resolution);
        dlg->app_resolution = NULL;
    }
    if (dlg->busy != NULL) {
        /* Detach, never free under a quit in flight: its callback frees it (busy_finish). */
        coop_busy_t *busy = dlg->busy;
        busy->dlg = NULL;
        dlg->busy = NULL;
        if (busy->confirm != NULL) {
            lv_obj_t *confirm = busy->confirm;
            busy->confirm = NULL;
            lv_obj_remove_event_cb(confirm, busy_confirm_cb);
            lv_msgbox_close_async(confirm);
            if (busy->pending == 0) {
                SDL_free(busy);
            }
        }
    }
    coop_pi_scan_end(&dlg->pi_scan);
    SDL_free(dlg);
}

static void dialog_btn_cb(lv_event_t *e) {
    coop_dialog_t *dlg = lv_event_get_user_data(e);
    lv_obj_t *msgbox = lv_event_get_current_target(e);
    if (msgbox != dlg->msgbox) {
        return;
    }
    const uint16_t pressed = lv_msgbox_get_active_btn(msgbox);
    if (pressed < dlg->start_btns && dlg->hosts_ok) {
        dlg->monitor_off_choice = pressed == 1;
        launch_session(dlg);
    } else {
        lv_msgbox_close_async(msgbox);
    }
}

/** The dialog's own text size. The theme fixes font_normal as a LOCAL style on every
 *  object it touches (lv_theme_moonlight.c apply_cb), so setting a font on the
 *  container would never reach the labels inside it -- each one has to ask. */
static lv_obj_t *dialog_label(lv_obj_t *parent, const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, lv_theme_get_font_small(label), 0);
    if (text != NULL) {
        lv_label_set_text_static(label, text);
    }
    return label;
}

/**
 * A section heading, INSIDE its row rather than on a line of its own.
 *
 * Three headings each cost a full line of a dialog that has to fit in the middle of the
 * screen without scrolling, and they bought nothing horizontally: the sections are two
 * columns of cards with room to spare beside them. Sitting in the row also means the
 * heading is part of the click target instead of a separate one -- a click anywhere in
 * the row, words included, selects the section.
 */
static lv_obj_t *row_heading(lv_obj_t *parent, const char *text) {
    lv_obj_t *label = dialog_label(parent, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(SECTION_LABEL_PCT));
    return label;
}

/**
 * A focusable, keyboard-driven row.
 *
 * ADDED TO THE GROUP BY HAND. The theme auto-adds msgbox children to the dialog's
 * group, but only ones whose class is group_def -- buttons, dropdowns and the like. A
 * plain lv_obj is not, so a row built this way would never receive a key event and the
 * whole dialog would be dead to the remote. @p group comes from the msgbox's own
 * button matrix, which the theme has already placed in the right group.
 */
static lv_obj_t *focusable_row(lv_obj_t *parent, lv_group_t *group, coop_dialog_t *dlg) {
    lv_obj_t *row = lv_obj_create(parent);
    if (group != NULL) {
        lv_group_add_obj(group, row);
    }
    lv_obj_add_event_cb(row, section_cancel_cb, LV_EVENT_CANCEL, dlg);
    /* WHICH BOX OWNS THE INPUT MUST BE VISIBLE. Without this the sections look
     * identical whatever is focused, so a left/right press appears to act on all of
     * them at once -- especially since changing the orientation legitimately redraws
     * the player 1/2 symbols below it. The outline is on the FOCUSED state only, and
     * it is what makes "bound to a single box" true to look at as well as in code. */
    /* FOCUS_KEY, NOT FOCUSED: focus is also taken by a pointer click, and on
     * LV_STATE_FOCUSED the ring then stayed drawn around this row while the cursor
     * highlighted something else -- two things looking selected at once. LVGL sets
     * FOCUS_KEY only for a keypad or encoder (lv_obj.c), which is exactly when this
     * ring is the selection; ui_hover.c keeps it off while the pointer drives. */
    lv_obj_set_style_outline_width(row, lv_dpx(3), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(row, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_opa(row, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(row, lv_dpx(2), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(row, lv_dpx(4), 0);
    lv_obj_set_style_pad_column(row, lv_dpx(4), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    /* START, not SPACE_EVENLY: the columns are placed by percentage so that all three
     * sections line up, which even spacing would undo. */
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

/**
 * THE LINE AT THE TOP OF THE POPUP THAT SAYS WHAT THIS PAIRING WILL ACTUALLY DO.
 *
 * Known NVIDIA/AMD pairs name the automatically selected encoder. Other pairings
 * retain the legacy compatibility notices below.
 *
 * IT NEVER DISABLES THE LAUNCH. Even the worst case here -- a mixed pair whose NVIDIA
 * host has no Main-profile control, which cannot produce a correct picture by any
 * configuration -- is stated and then left to the user. That is a deliberate call: the
 * detection rests on what hosts report about themselves, and a wrong "you cannot do
 * this" would be a far worse failure than a corrupt picture the user asked to see.
 */
static void build_mode_banner(coop_dialog_t *dlg, lv_obj_t *content) {
    const coop_stream_plan_t *plan = &dlg->plan;
    char text[640];
    size_t w = 0;
    text[0] = '\0';

#define BANNER_APPEND(...) \
    do { \
        if (w < sizeof(text)) { \
            const int _n = snprintf(text + w, sizeof(text) - w, __VA_ARGS__); \
            if (_n > 0) { w += (size_t) _n; } \
        } \
    } while (0)

    if (plan->gpu_hevc && plan->forced) {
        BANNER_APPEND(locstr("GPU HEVC encoder on both PCs: forced in Settings > Co-op > Encoder."));
    } else if (plan->gpu_hevc) {
        BANNER_APPEND(locstr("Beta HEVC: selected automatically for this GPU pairing. "
                             "Both PCs use the beta encoder. Updated Umbra is required on both PCs."));
    } else if (plan->forced && !plan->mixed_vendor) {
        BANNER_APPEND(locstr("Each PC's own encoder: forced in Settings > Co-op > Encoder."));
    } else if (plan->vendors_known && dlg->machine_vendor[0] == COOP_GPU_NVIDIA &&
               dlg->machine_vendor[1] == COOP_GPU_NVIDIA) {
        BANNER_APPEND(locstr("HEVC core: both PCs use their NVIDIA hardware encoder."));
    } else if (plan->forced && plan->mixed_vendor) {
        /* The user overrode a mixed pair back onto the core path. That is theirs to do,
         * and it is also the single most likely way to arrive at a broken picture while
         * believing the feature is at fault, so it gets said out loud. */
        BANNER_APPEND(locstr("Each PC's own encoder is forced in Settings, but %s (%s) and %s (%s) "
                             "are different makes of GPU -- expect a corrupt picture."),
                      dlg->machine_name[0], coop_gpu_vendor_name(dlg->machine_vendor[0]),
                      dlg->machine_name[1], coop_gpu_vendor_name(dlg->machine_vendor[1]));
    }





    /* A HOST THAT SAID NO. Umbra reports whether its GPU HEVC encoder is installed (UmbraCaps bit 3);
     * one that is not cannot open its pane, so say which PC and what fixes it before the user
     * finds out from a pane that never appears. */
    if (plan->host_support_missing) {
        if (w > 0) { BANNER_APPEND("\n"); }
        BANNER_APPEND(locstr("One of these PCs reports that Umbra's GPU HEVC encoder is not installed, "
                             "and this pairing needs it on both. Install the current Umbra release on "
                             "that PC; its installer includes the encoder."));
    }

    /* HDR IS REPORTED ON ITS OWN TERMS, and only when the user actually asked for it.
     * The two reasons are NOT the same fact and are never merged into "HDR is off":
     * one is a machine that cannot do it, the other is a codec that has no form for it.
     * Only the first is the pairing overruling a setting the user chose. */
    if (plan->hdr_requested && plan->hdr_blocked_by_host) {
        if (w > 0) { BANNER_APPEND("\n"); }
        BANNER_APPEND(locstr("HDR is off for this session: one of these PCs does not offer a "
                             "10-bit codec. Both halves are one picture, so HDR needs both."));
    }

    for (int i = 0; i < 2; ++i) {
        /* UNTESTED IS NOT UNSUPPORTED, and the wording keeps them apart. Co-op has only
         * ever run on NVIDIA and AMD in this project; anything else is a first. */
        if (dlg->machine_vendor_known[i] && dlg->machine_vendor[i] != COOP_GPU_NVIDIA &&
            dlg->machine_vendor[i] != COOP_GPU_AMD) {
            if (w > 0) { BANNER_APPEND("\n"); }
            BANNER_APPEND(locstr("%s reports an %s GPU. Co-op has never been tested on %s "
                                 "hardware -- treat anything you see as new."),
                          dlg->machine_name[i], coop_gpu_vendor_name(dlg->machine_vendor[i]),
                          coop_gpu_vendor_name(dlg->machine_vendor[i]));
        }
    }

    if (!plan->vendors_known && !plan->forced) {
        if (w > 0) { BANNER_APPEND("\n"); }
        /* Say what is missing and what it costs, not just that something is. */
#ifdef AURORA_GPU_HEVC_EXPERIMENTAL
        BANNER_APPEND(locstr("Map both PCs' GPU types in Settings > Co-op, or install the "
                             "updated Umbra host build for automatic detection. Beta HEVC "
                             "selection needs both GPU types; this session currently uses HEVC core."));
#else
        BANNER_APPEND(locstr("GPU detection needs Umbra on both PCs; without it this session uses "
                             "each PC's own encoder. If either PC has an AMD card, set Settings > "
                             "Co-op > Encoder to the GPU HEVC encoder on both PCs."));
#endif
    }

#undef BANNER_APPEND

    if (w == 0) {
        /* Nothing to say. DELETE it rather than blanking it: an empty label still takes
         * a row's padding, and this dialog is fighting for vertical space. */
        if (dlg->banner != NULL) {
            lv_obj_del(dlg->banner);
            dlg->banner = NULL;
        }
        return;
    }
    if (dlg->banner == NULL) {
        dlg->banner = lv_label_create(content);
        lv_obj_set_style_text_font(dlg->banner, lv_theme_get_font_small(dlg->banner), 0);
        lv_label_set_long_mode(dlg->banner, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(dlg->banner, LV_PCT(100));
        /* FIRST CHILD, ALWAYS. It is created lazily -- a pairing can become worth
         * warning about only after the user moves a column -- so it has to be moved to
         * the top rather than landing wherever it was made. */
        lv_obj_move_to_index(dlg->banner, 0);
    }
    lv_label_set_text(dlg->banner, text);
}

/** Rebuild the banner against the CURRENT pairing. The content object is reached through
 *  the msgbox so callers that only have the dialog (a machine change) need not carry it. */
static void refresh_mode_banner(coop_dialog_t *dlg) {
    if (dlg->msgbox == NULL) {
        return;
    }
    build_mode_banner(dlg, lv_msgbox_get_content(dlg->msgbox));
}

void coop_dialog_open(launcher_fragment_t *launcher) {
    if (launcher == NULL) {
        return;
    }
    coop_dialog_t *dlg = SDL_calloc(1, sizeof(coop_dialog_t));
    if (dlg == NULL) {
        return;
    }
    dlg->launcher = launcher;
    dlg->global = launcher->global;
    dlg->orientation = app_configuration->coop_orientation == COOP_ORIENTATION_VERTICAL
                       ? COOP_ORIENTATION_VERTICAL : COOP_ORIENTATION_HORIZONTAL;
    resolve_hosts(dlg);
    /* Start the Pi sweep BEFORE building the list. It never blocks, so the first
     * answers usually land within a tick or two of the dialog appearing rather than
     * costing a stall up front -- the previous version waited 250 ms on open. */
    {
        const pclist_t *node = pcmanager_node(pcmanager, &dlg->machine_uuid[0]);
        coop_pi_scan_begin(&dlg->pi_scan, node != NULL && node->server != NULL
                                          ? node->server->serverInfo.address : NULL);
    }
    collect_pads(dlg);

    /* CHOOSE MODE (app_settings.h host_monitor_mode): a second start that switches both PCs' monitors off.
     * Offered whatever the pair -- a PC that cannot (not Umbra, or this PC) simply keeps its monitor on. */
    static const char *btn_texts[] = {translatable("Start co-op"), translatable("Cancel"), ""};
    static const char *btn_texts_choice[] = {translatable("Start co-op"), translatable("Start, monitors off"),
                                             translatable("Cancel"), ""};
    dlg->start_btns = app_launch_host_monitor_choice_offered() ? 2 : 1;
    lv_obj_t *msgbox = lv_msgbox_create_i18n(NULL, locstr("Co-op session"), NULL,
                                             dlg->start_btns == 2 ? btn_texts_choice : btn_texts, false);
    dlg->msgbox = msgbox;
    lv_obj_set_user_data(msgbox, dlg);
    lv_obj_add_event_cb(msgbox, dialog_deleted_cb, LV_EVENT_DELETE, dlg);
    lv_obj_add_event_cb(msgbox, dialog_btn_cb, LV_EVENT_VALUE_CHANGED, dlg);
    /* WIDE AND SHORT, FLOATING IN THE MIDDLE. The dialog used to run the full height of
     * the screen with its content scrolling, because everything was stacked: a heading
     * line, then a row, three times over. It now trades that height for width -- the
     * theme's 60% cap has to be lifted by hand or the width below is silently clamped
     * to it -- and the sections are laid out so the whole thing fits inside the middle
     * of the screen with nothing to scroll. The height cap is the backstop for the
     * cases that still cannot fit (a great many controllers), not the normal path. */
    lv_obj_set_style_max_width(msgbox, LV_PCT(80), 0);
    lv_obj_set_width(msgbox, LV_PCT(80));
    /* Keep enough of the viewport available for the fixed action row.  On Windows,
     * high-DPI scaling makes the otherwise identical controls taller than they are
     * on webOS; the old 62% ceiling clipped both actions out of the dialog, leaving
     * mouse users unable to start or cancel a session.  This is still only a ceiling,
     * so platforms where the compact layout fits retain its natural short height.
     * 95%, not 90%: measured 2026-09-26 at 2560x1392 (the user's window) the content ran
     * 14 px past a 90% cap -- a scrollbar and a hidden sliver for want of 1% of the screen.
     * Past this cap the content scrolls between the title and the buttons (the theme's
     * msgbox fit), which keeps the buttons on screen; fonts never shrink to make room. */
    lv_obj_set_style_max_height(msgbox, LV_PCT(95), 0);

    lv_obj_t *content = lv_msgbox_get_content(msgbox);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, lv_dpx(6), 0);
    lv_obj_set_style_pad_gap(content, lv_dpx(3), 0);

    if (!dlg->hosts_ok) {
        /* Two hosts is not a preference, it is what co-op IS. Say so plainly rather
         * than offering a launch that can only fail at connect time. */
        lv_obj_t *warn = lv_label_create(content);
        lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(warn, LV_PCT(100));
        lv_label_set_text_static(warn, locstr("Co-op needs two paired computers. Add or pair a "
                                              "second PC, then open this again."));
        start_btns_disabled(dlg, lv_msgbox_get_btns(msgbox), true);
        lv_obj_center(msgbox);
        return;
    }

    /* The msgbox's button matrix is already in the dialog's modal group, so it is the
     * reliable way to reach that group without reaching into the theme. Rows added
     * after it means d-pad DOWN off the last section lands on "Start co-op". */
    lv_group_t *group = lv_obj_get_group(lv_msgbox_get_btns(msgbox));

    /* --- what this pairing will do ------------------------------------------ */
    /* FIRST, ABOVE THE CONTROLS. It is context for every choice below it -- the
     * orientation and the roles mean something different in a session that is about to
     * stream H.264 -- and it is not focusable, so it never costs a d-pad step. */
    build_mode_banner(dlg, content);

    /* --- which computers ---------------------------------------------------- */
    /* ONLY WHEN THERE ARE MORE THAN TWO. With two paired PCs the pair is not a choice,
     * and a row offering one would be a control that cannot change anything -- worse
     * than absent, because it implies the opposite. Above two it goes FIRST: every other
     * section on this screen is about these two machines. */
    if (dlg->pc_choosable) {
        dlg->pc_row = focusable_row(content, group, dlg);
        row_heading(dlg->pc_row, locstr("Computers"));
        lv_obj_add_event_cb(dlg->pc_row, pc_key_cb, LV_EVENT_KEY, dlg);
        lv_obj_add_event_cb(dlg->pc_row, pc_click_cb, LV_EVENT_SHORT_CLICKED, dlg);
        lv_obj_add_event_cb(dlg->pc_row, accept_all_cb, LV_EVENT_LONG_PRESSED, dlg);
        for (int side = 0; side < 2; ++side) {
            lv_obj_t *card = lv_obj_create(dlg->pc_row);
            lv_obj_remove_style_all(card);
            lv_obj_set_size(card, LV_PCT(SECTION_COL_PCT), LV_SIZE_CONTENT);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_all(card, lv_dpx(4), 0);
            lv_obj_set_style_radius(card, lv_dpx(4), 0);
            lv_obj_set_style_border_width(card, lv_dpx(1), 0);
            lv_obj_set_style_border_color(card, lv_palette_main(LV_PALETTE_GREY), 0);
            lv_obj_t *name = dialog_label(card, NULL);
            lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
            lv_obj_set_width(name, LV_PCT(100));
            lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
            ui_hover_mark(card);
            lv_obj_add_event_cb(card, pc_card_click_cb, LV_EVENT_SHORT_CLICKED, dlg);
            dlg->pc_card[side] = card;
            dlg->pc_name[side] = name;
        }
    }

    /* --- controllers ------------------------------------------------------- */
    dlg->pad_box = focusable_row(content, group, dlg);
    lv_obj_add_event_cb(dlg->pad_box, pad_box_key_cb, LV_EVENT_KEY, dlg);
    lv_obj_add_event_cb(dlg->pad_box, pad_box_click_cb, LV_EVENT_SHORT_CLICKED, dlg);
    lv_obj_add_event_cb(dlg->pad_box, accept_all_cb, LV_EVENT_LONG_PRESSED, dlg);
    /* HEADINGS PINNED, LIST GROWS UNDER THEM. The host names are a fixed header row
     * and the controllers live in a separate body row below, so adding a controller
     * extends the list downward instead of pushing the names around -- which is what
     * happened when the name was just the first child of each column. */
    lv_obj_set_flex_flow(dlg->pad_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(dlg->pad_box, lv_dpx(2), 0);

    lv_obj_t *pad_head = lv_obj_create(dlg->pad_box);
    lv_obj_remove_style_all(pad_head);
    lv_obj_set_width(pad_head, LV_PCT(100));
    lv_obj_set_height(pad_head, LV_SIZE_CONTENT);
    lv_obj_clear_flag(pad_head, LV_OBJ_FLAG_SCROLLABLE);
    /* NOT A CLICK TARGET ITSELF: with this cleared the click falls through to the box,
     * which is what the user aimed at. A plain object is clickable by default, and the
     * strip would otherwise swallow every press that missed a name. */
    lv_obj_clear_flag(pad_head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(pad_head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pad_head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pad_head, lv_dpx(4), 0);
    row_heading(pad_head, locstr("Controllers"));

    lv_obj_t *pad_body = lv_obj_create(dlg->pad_box);
    lv_obj_remove_style_all(pad_body);
    lv_obj_set_width(pad_body, LV_PCT(100));
    lv_obj_set_height(pad_body, LV_SIZE_CONTENT);
    lv_obj_clear_flag(pad_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(pad_body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(pad_body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pad_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(pad_body, lv_dpx(4), 0);
    /* Under the heading, so the columns line up with the names above them. */
    lv_obj_t *body_indent = lv_obj_create(pad_body);
    lv_obj_remove_style_all(body_indent);
    lv_obj_clear_flag(body_indent, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(body_indent, LV_PCT(SECTION_LABEL_PCT), 1);

    for (int column = 0; column < PAD_ROUTE_COUNT; ++column) {
        lv_obj_t *title = dialog_label(pad_head, NULL);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title, LV_PCT(PAD_SECTION_COL_PCT));
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        dlg->pad_col_title[column] = title;

        lv_obj_t *col = lv_obj_create(pad_body);
        lv_obj_remove_style_all(col);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_width(col, LV_PCT(PAD_SECTION_COL_PCT));
        /* Grows with its contents, with a floor so an empty side still reads as a
         * drop target rather than collapsing to nothing -- and a CEILING, so that
         * plugging in a sixth controller cannot push the dialog off the screen. Past
         * the ceiling the column scrolls and refresh_pads keeps the cursor in view. */
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(col, lv_dpx(30), 0);
        lv_obj_set_style_max_height(col, lv_dpx(72), 0);
        lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_AUTO);
        /* remove_style_all() above took the bar's style too: past the ceiling the column
         * scrolled with no sign that it held more controllers. */
        lv_theme_moonlight_add_scrollbar(col);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(col, lv_dpx(3), 0);
        lv_obj_set_style_pad_row(col, lv_dpx(2), 0);
        lv_obj_set_style_border_width(col, lv_dpx(1), 0);
        lv_obj_set_style_border_color(col, lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_set_style_radius(col, lv_dpx(4), 0);
        dlg->pad_col[column] = col;
    }
    rebuild_pad_items(dlg);
    size_t real_pads = 0;
    for (size_t i = 0; i < dlg->pad_count; ++i) {
        real_pads += dlg->kbm_of[i] < 0 ? 1 : 0;
    }
    if (real_pads == 0) {
        /* ONE LINE. The long version wrapped to two and pushed the sections down for a
         * state that is completely normal on opening -- nothing is plugged in yet. */
        lv_obj_t *none = dialog_label(content, locstr("No controllers connected. Both panes "
                                                      "still start."));
        lv_label_set_long_mode(none, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(none, LV_PCT(100));
        lv_obj_set_style_text_opa(none, LV_OPA_70, 0);
    }

    /* --- orientation ------------------------------------------------------- */
    dlg->orient_row = focusable_row(content, group, dlg);
    row_heading(dlg->orient_row, locstr("Display orientation"));
    lv_obj_add_event_cb(dlg->orient_row, orient_key_cb, LV_EVENT_KEY, dlg);
    lv_obj_add_event_cb(dlg->orient_row, orient_click_cb, LV_EVENT_SHORT_CLICKED, dlg);
    lv_obj_add_event_cb(dlg->orient_row, accept_all_cb, LV_EVENT_LONG_PRESSED, dlg);
    /* Vertical first, left to right, so LEFT selects vertical and RIGHT horizontal --
     * the key that moves the highlight matches where the highlight goes. */
    static const int orient_order[2] = {COOP_ORIENTATION_VERTICAL, COOP_ORIENTATION_HORIZONTAL};
    static const char *orient_names[2] = {translatable("Vertical"), translatable("Horizontal")};
    for (int i = 0; i < 2; ++i) {
        const int value = orient_order[i];
        /* THE ICON SITS BESIDE ITS NAME, not above it. Stacked, each card was three
         * lines tall and the two card sections alone filled half the screen. */
        lv_obj_t *card = lv_obj_create(dlg->orient_row);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, LV_PCT(SECTION_COL_PCT), LV_SIZE_CONTENT);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(card, lv_dpx(4), 0);
        lv_obj_set_style_pad_column(card, lv_dpx(5), 0);
        lv_obj_set_style_radius(card, lv_dpx(4), 0);
        lv_obj_t *icon = split_icon_create(card, lv_dpx(30), lv_dpx(20));
        dialog_label(card, locstr(orient_names[i]));
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        ui_hover_mark(card);
        lv_obj_add_event_cb(card, orient_card_click_cb, LV_EVENT_SHORT_CLICKED, dlg);
        dlg->orient_card[value] = card;
        dlg->orient_icon[value] = icon;
    }

    /* --- primary / secondary ----------------------------------------------- */
    dlg->role_row = focusable_row(content, group, dlg);
    row_heading(dlg->role_row, locstr("Player 1 / Player 2"));
    lv_obj_add_event_cb(dlg->role_row, role_key_cb, LV_EVENT_KEY, dlg);
    lv_obj_add_event_cb(dlg->role_row, role_click_cb, LV_EVENT_SHORT_CLICKED, dlg);
    lv_obj_add_event_cb(dlg->role_row, accept_all_cb, LV_EVENT_LONG_PRESSED, dlg);
    static const char *role_names[2] = {translatable("Primary"), translatable("Secondary")};
    for (int role = 0; role < 2; ++role) {
        lv_obj_t *card = lv_obj_create(dlg->role_row);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, LV_PCT(SECTION_COL_PCT), LV_SIZE_CONTENT);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(card, lv_dpx(4), 0);
        lv_obj_set_style_pad_column(card, lv_dpx(5), 0);
        lv_obj_set_style_radius(card, lv_dpx(4), 0);
        lv_obj_set_style_border_width(card, lv_dpx(1), 0);
        lv_obj_set_style_border_color(card, lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_t *icon = split_icon_create(card, lv_dpx(28), lv_dpx(18));
        /* Role and host on two short lines beside the symbol: "Primary" says which
         * half of the screen, the name says which machine drives it. */
        lv_obj_t *text = lv_obj_create(card);
        lv_obj_remove_style_all(text);
        lv_obj_clear_flag(text, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(text, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_grow(text, 1);
        lv_obj_set_height(text, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
        dialog_label(text, locstr(role_names[role]));
        lv_obj_t *name = dialog_label(text, NULL);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, LV_PCT(100));
        lv_obj_set_style_text_opa(name, LV_OPA_70, 0);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        ui_hover_mark(card);
        lv_obj_add_event_cb(card, role_card_click_cb, LV_EVENT_SHORT_CLICKED, dlg);
        dlg->role_card[role] = card;
        dlg->role_icon[role] = icon;
        dlg->role_name[role] = name;
    }

    lv_obj_t *hint = dialog_label(content, dlg->pc_choosable
            ? locstr("On Computers, left/right picks a column and OK changes the PC in it. "
                     "On controllers, OK picks one up; left/right chooses the PC; "
                     "OK puts it down. Hold OK to accept everything.")
            : locstr("OK picks a controller up; left/right chooses the PC; "
                                                  "hold OK to start."));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_opa(hint, LV_OPA_70, 0);

    refresh_pcs(dlg);
    refresh_pads(dlg);
    refresh_orientation(dlg);
    refresh_roles(dlg);
    /* ASK ABOUT ANY UNKNOWN GPU NOW, BEFORE THE USER CAN PRESS START. The banner one row above
     * describes what this pairing will do, and until the question is answered it can only say
     * "detection unavailable" -- which is a worse thing to show someone than a question with
     * four buttons. Raised after the dialog is fully built so answering returns to a finished
     * screen rather than one still being assembled. */
    gpu_ask_begin(dlg);
    /* 250 ms: fast enough that a controller switched on feels like it "just appears",
     * and free to run at that rate because both halves of the check are non-blocking. */
    dlg->rescan = lv_timer_create(rescan_timer_cb, 250, dlg);
    lv_obj_center(msgbox);
}

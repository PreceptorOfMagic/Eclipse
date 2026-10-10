#pragma once

#include "lvgl.h"
#include "coverloader.h"
#include "backend/apploader/apploader.h"
#include "uuidstr.h"

typedef struct app_t app_t;

typedef struct {
    lv_style_t cover;
    lv_style_t btn;
    lv_style_transition_dsc_t tr_pressed;
    lv_style_transition_dsc_t tr_released;
    lv_img_dsc_t fav_indicator_src;
    lv_img_dsc_t defcover_src;
    /** The tile's corner radius -- the one number the frame, the title bar and the cover's own
     *  rounded pixels must all agree on. */
    lv_coord_t radius;
    /** What defcover_src shows once the grid has a size: the placeholder at the tile's size with
     *  its rounded corners in the pixels (coverloader_default_cover_build). NOT directly after
     *  defcover_src: a descriptor followed by its own lv_sdl_img_data_t is the shape
     *  coverloader.c img_set_cover takes for a memcache item (it now also checks the address). */
    lv_sdl_img_data_t defcover_data;
} appitem_styles_t;


typedef struct {
    lv_fragment_t base;
    app_t *global;
    uuidstr_t uuid;

    int def_app;
    bool def_app_launched;
    /* This launch came from the `ares-launch -p '{...}'` payload, not from the UI.
     * launcher_launch_game() needs to know, because it clears the co-op globals on the
     * assumption that reaching it means a single-host launch -- true for a click, false
     * for the payload path, which routes through the same function. */
    bool def_app_from_payload;

    bool show_hidden_apps;

    apploader_t *apploader;
    coverloader_t *coverloader;
    apploader_cb_t apploader_cb;

    apploader_list_t *apploader_apps;
    const char *apploader_error;

    lv_obj_t *applist, *appload, *apperror;
    lv_obj_t *errortitle, *errorhint, *errordetail;
    lv_obj_t *actions;

    lv_obj_t *quit_progress;
    /* An app to start once the running one has quit ("Quit it and start X?"), or 0. */
    int launch_after_quit;

    appitem_styles_t appitem_style;
    int col_count;
    lv_coord_t col_width, col_height;
    int focus_backup;
} apps_fragment_t;

typedef struct {
    int app_id;
    apps_fragment_t *controller;
    const appitem_styles_t *styles;
    lv_obj_t *play_indicator;
    lv_obj_t *title;
} appitem_viewholder_t;

typedef struct {
    app_t *global;
    uuidstr_t host;
    int def_app;
} apps_fragment_arg_t;

extern const lv_fragment_class_t apps_controller_class;

/**
 * Quit the active stream and relaunch the currently-running app fresh, once.
 *
 * Used by PIN auto-entry after a wake login: Apollo only enables the Windows HDR
 * display state at fresh stream setup, and that setup is skipped/rejected on the
 * lock-screen (secure) desktop. Crucially a *resume* does not re-run it either —
 * only a fresh launch does. So this fully quits (host currentGame -> 0) and records
 * the app id in a file-static; when the stream tears down and the apps view is
 * recreated, appload_loaded relaunches that app fresh, bringing HDR up without a
 * manual quit+restart. Takes the app id from the session (the apps controller's
 * obj is destroyed while a stream covers it). No-op (returns false) if there is no
 * session or no running app.
 */
bool apps_controller_reconnect_running_app(app_t *global);

/** True while a PIN HDR reconnect still has retry attempts left. root.c uses this to
 * suppress the "failed to start streaming" dialog on a launch that will be retried. */
bool apps_controller_hdr_relaunch_will_retry(void);

/** Clear any pending PIN HDR reconnect (call when a stream successfully opens). */
void apps_controller_hdr_relaunch_succeeded(void);
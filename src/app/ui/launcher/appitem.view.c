#include "appitem.view.h"
#include "res.h"

#include "draw/sdl/lv_draw_sdl_utils.h"
#include "lvgl/theme/lv_theme_moonlight.h"
#include "lvgl/theme/lv_theme_moonlight_colors.h"
#include "ui/ui_hover.h"
#include "app.h"
#include "ui/ui_input.h"

static void appitem_holder_free_cb(lv_event_t *event);

static void appitem_draw_decor(lv_event_t *e);

static void appitem_draw_border(lv_event_t *e);

static void appitem_selected(lv_event_t *e);

static void appitem_deselected(lv_event_t *e);

lv_obj_t *appitem_view(apps_fragment_t *controller, lv_obj_t *parent) {
    appitem_styles_t *styles = &controller->appitem_style;
    lv_obj_t *item = lv_img_create(parent);
    lv_obj_add_flag(item, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    /* A tile is an image, which LVGL has no reason to think is pressable, so the
     * pointer highlight has to be told. */
    ui_hover_mark(item);
    lv_obj_add_style(item, &styles->cover, 0);
    lv_obj_set_size(item, controller->col_width, controller->col_height);
    lv_img_set_antialias(item, true);

    lv_obj_set_style_outline_opa(item, LV_OPA_COVER, LV_STATE_FOCUS_KEY);

    lv_obj_set_style_transform_pivot_x(item, controller->col_width / 2, 0);
    lv_obj_set_style_transform_pivot_y(item, controller->col_height / 2, 0);

    lv_obj_set_style_transform_zoom(item, 256 * 99 / 100, LV_STATE_PRESSED);
    lv_obj_set_style_transform_zoom(item, 256 * 102 / 100, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_transition(item, &styles->tr_pressed, LV_STATE_PRESSED | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_transition(item, &styles->tr_released, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(item, appitem_draw_decor, LV_EVENT_DRAW_MAIN, styles);
    lv_obj_add_event_cb(item, appitem_draw_border, LV_EVENT_DRAW_POST, NULL);
    lv_obj_add_event_cb(item, appitem_selected, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(item, appitem_deselected, LV_EVENT_DEFOCUSED, NULL);

    lv_obj_t *play_indicator = lv_label_create(item);
    lv_obj_clear_flag(play_indicator, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_style_all(play_indicator);
    lv_obj_add_style(play_indicator, &styles->btn, 0);
    lv_label_set_text(play_indicator, MAT_SYMBOL_PLAY_ARROW);
    lv_obj_center(play_indicator);
    lv_obj_t *title = lv_label_create(item);
    const lv_font_t *font = lv_theme_get_font_small(item);
    lv_obj_set_style_text_font(title, font, 0);
    lv_obj_set_style_text_color(title, ml_color_hex(ML_COLOR_TEXT), 0);
    lv_coord_t th = lv_obj_get_style_text_font(title, 0)->line_height + LV_DPX(10);
    lv_obj_set_size(title, LV_PCT(100), th);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_pad_hor(title, LV_DPX(8), 0);
    lv_obj_set_style_pad_ver(title, LV_DPX(4), 0);
    /* No background of its own: a plain rect has square corners, and the tile does not clip
     * (see appitem_style_init). appitem_draw_decor paints it with the tile's radius. */
    lv_obj_align(title, LV_ALIGN_BOTTOM_MID, 0, 0);

    appitem_viewholder_t *holder = (appitem_viewholder_t *) malloc(sizeof(appitem_viewholder_t));
    memset(holder, 0, sizeof(appitem_viewholder_t));

    holder->controller = controller;
    holder->styles = styles;
    holder->play_indicator = play_indicator;
    holder->title = title;
    lv_obj_set_user_data(item, holder);
    lv_obj_add_event_cb(item, appitem_holder_free_cb, LV_EVENT_DELETE, holder);
    return item;
}

void appitem_style_init(appitem_styles_t *style) {
    lv_style_init(&style->cover);
    lv_style_set_pad_all(&style->cover, 0);
    style->radius = LV_DPX(10);
    lv_style_set_radius(&style->cover, style->radius);
    /* ROUND CORNERS WITHOUT A MASK. On this SDL backend a clip-corner mask rounds nothing: with
     * LV_GPU_SDL_CUSTOM_BLEND_MODE 0 (lv_conf.h) lv_draw_sdl_composite.c reduces it to a plain
     * rectangle, and the one path that does round an image (lv_draw_sdl_img.c draw_img_rounded,
     * taken only when the image rect equals the mask's) drew a black square in every corner on
     * Windows (2026-09-27). The old 1dp style border kept every cover off that path -- and so
     * square -- since the covers were first given a radius. So nothing here clips: each cover
     * carries its own rounded corners in its pixels (coverloader.c canvas_round_corners), the
     * title bar draws its own (appitem_draw_decor), and the 1dp frame is painted on top
     * (appitem_draw_border) instead of being a style border that would inset the cover. */
    lv_style_set_shadow_opa(&style->cover, LV_OPA_40);
    lv_style_set_shadow_width(&style->cover, LV_DPX(16));
    lv_style_set_shadow_ofs_y(&style->cover, LV_DPX(6));
    lv_style_set_shadow_color(&style->cover, ml_color_hex(ML_COLOR_BG));
    lv_style_set_outline_color(&style->cover, ml_color_hex(ML_COLOR_PRIMARY));
    lv_style_set_outline_width(&style->cover, LV_DPX(3));
    lv_style_set_outline_opa(&style->cover, LV_OPA_TRANSP);
    lv_style_set_outline_pad(&style->cover, LV_DPX(2));

    lv_style_init(&style->btn);
    lv_style_set_width(&style->btn, LV_DPX(44));
    lv_style_set_height(&style->btn, LV_DPX(44));
    lv_style_set_radius(&style->btn, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&style->btn, ml_color_hex(ML_COLOR_PRIMARY));
    lv_style_set_bg_opa(&style->btn, LV_OPA_COVER);
    lv_style_set_text_color(&style->btn, ml_color_hex(ML_COLOR_TEXT));
    lv_style_set_border_opa(&style->btn, LV_OPA_TRANSP);
    lv_style_set_text_font(&style->btn, lv_theme_moonlight_get_iconfont_large(lv_scr_act()));
    lv_style_set_shadow_width(&style->btn, LV_DPX(8));
    lv_style_set_shadow_color(&style->btn, ml_color_hex(ML_COLOR_PRIMARY));
    lv_style_set_shadow_opa(&style->btn, LV_OPA_50);
    /* Temporary solution for LVGL 8.3.0 */
    lv_style_set_text_align(&style->btn, LV_TEXT_ALIGN_CENTER);
    lv_style_set_pad_ver(&style->btn, LV_DPX(10));

    static const lv_style_prop_t trans_props[] = {
            LV_STYLE_OUTLINE_OPA, LV_STYLE_TRANSFORM_WIDTH, LV_STYLE_TRANSFORM_HEIGHT, LV_STYLE_TRANSFORM_ZOOM, 0
    };
    lv_style_transition_dsc_init(&style->tr_pressed, trans_props, lv_anim_path_linear, LV_THEME_DEFAULT_TRANSITON_TIME,
                                 0, NULL);
    lv_style_transition_dsc_init(&style->tr_released, trans_props, lv_anim_path_linear, LV_THEME_DEFAULT_TRANSITON_TIME,
                                 70, NULL);

    style->fav_indicator_src.header.w = LV_DPX(48);
    style->fav_indicator_src.header.h = LV_DPX(48);
    style->fav_indicator_src.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    style->fav_indicator_src.data_size = sizeof(lv_sdl_img_data_t);
    style->fav_indicator_src.data = (const uint8_t *) &lv_sdl_img_data_fav_indicator;

    style->defcover_src.header.w = 0;
    style->defcover_src.header.h = 0;
    style->defcover_src.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    style->defcover_src.data_size = sizeof(lv_sdl_img_data_t);
    style->defcover_src.data = (const uint8_t *) &lv_sdl_img_data_defcover;
}

void appitem_style_deinit(appitem_styles_t *style) {
    coverloader_default_cover_free(&style->defcover_src, &style->defcover_data);
    lv_style_reset(&style->cover);
    lv_style_reset(&style->btn);
}

static void appitem_holder_free_cb(lv_event_t *event) {
    appitem_viewholder_t *holder = event->user_data;
    free(holder);
}

static void appitem_draw_decor(lv_event_t *e) {
    appitem_styles_t *styles = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target(e);
    appitem_viewholder_t *holder = lv_obj_get_user_data(target);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);

    /* THE TITLE BAR, drawn here (after the cover, before the label's text) so it can carry the
     * tile's rounded bottom corners itself, as the cover's pixels do. The rect is made one radius
     * TALLER than the bar and clipped back to it, so its rounded top corners fall outside the
     * clip: square where the bar meets the cover, round where it meets the frame. */
    if (!lv_obj_has_flag(holder->title, LV_OBJ_FLAG_HIDDEN)) {
        lv_area_t bar, clip;
        lv_obj_get_coords(holder->title, &bar);
        if (_lv_area_intersect(&clip, &bar, ctx->clip_area)) {
            const lv_coord_t radius = styles->radius;
            lv_area_t shape = bar;
            shape.y1 -= radius;
            lv_draw_rect_dsc_t dsc;
            lv_draw_rect_dsc_init(&dsc);
            dsc.radius = radius;
            dsc.bg_color = ml_color_hex(ML_COLOR_BG);
            dsc.bg_opa = (lv_opa_t) ((255 * 72) / 100);
            const lv_area_t *clip_ori = ctx->clip_area;
            ctx->clip_area = &clip;
            lv_draw_rect(ctx, &dsc, &shape);
            ctx->clip_area = clip_ori;
        }
    }

    const apploader_item_t *item = apploader_list_item_by_id(holder->controller->apploader_apps, holder->app_id);
    if (item == NULL || !item->fav) {
        return;
    }
    lv_area_t coords;
    lv_obj_get_coords(target, &coords);
    lv_area_set_width(&coords, LV_DPX(48));
    lv_area_set_height(&coords, LV_DPX(48));

    lv_draw_img_dsc_t dsc;
    lv_draw_img_dsc_init(&dsc);
    lv_draw_img(ctx, &dsc, &coords, &styles->fav_indicator_src);
}

/** The tile's 1dp frame, painted OVER the cover -- see appitem_style_init for why it is not a
 *  style border. */
static void appitem_draw_border(lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    lv_area_t coords;
    lv_obj_get_coords(target, &coords);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = lv_obj_get_style_radius(target, LV_PART_MAIN);
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.border_width = LV_DPX(1);
    dsc.border_color = ml_color_hex(ML_COLOR_BORDER);
    dsc.border_opa = LV_OPA_COVER;
    lv_draw_rect(lv_event_get_draw_ctx(e), &dsc, &coords);
}

static void appitem_selected(lv_event_t *e) {
    lv_obj_t *item = lv_event_get_current_target(e);
    appitem_viewholder_t *holder = lv_obj_get_user_data(item);
    lv_label_set_long_mode(holder->title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    /* SHOW WHERE OK WILL GO. A focus set by the app (the first tile when a PC's apps appear)
     * arrives with no keypad behind it, so LVGL marks it FOCUSED without FOCUS_KEY and no ring
     * is drawn -- the tile OK would launch was invisible. Unless the pointer is driving (then
     * the hover outline is the selection, ui_hover.c), give it the ring. "Not the pointer"
     * rather than "keys", because before the first input of a run the mode is neither. */
    if (!(app_ui_get_input_mode(&holder->controller->global->ui.input) & UI_INPUT_MODE_POINTER_FLAG)) {
        lv_obj_add_state(item, LV_STATE_FOCUS_KEY);
    }
}

static void appitem_deselected(lv_event_t *e) {
    lv_obj_t *item = lv_event_get_current_target(e);
    appitem_viewholder_t *holder = lv_obj_get_user_data(item);
    lv_label_set_long_mode(holder->title, LV_LABEL_LONG_DOT);
}
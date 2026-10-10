#pragma once

#include "backend/pcmanager.h"

#include <stdbool.h>

#include "libgamestream/client.h"
#include "lv_sdl_img.h"

struct coverloader_t;

typedef struct app_t app_t;
typedef struct coverloader_t coverloader_t;

coverloader_t *coverloader_new(app_t *app);

void coverloader_unref(coverloader_t *loader);

void coverloader_display(coverloader_t *loader, const uuidstr_t *uuid, int id, lv_obj_t *target,
                         lv_coord_t target_width, lv_coord_t target_height);

/** (Re)build the picture for tiles with no cover art at the tile's size, with the tile's rounded
 *  corners in its pixels like every cover (see canvas_round_corners). False = @p src untouched. */
bool coverloader_default_cover_build(lv_img_dsc_t *src, lv_sdl_img_data_t *data, lv_coord_t width,
                                     lv_coord_t height, lv_coord_t radius);

/** Release what coverloader_default_cover_build made, dropping LVGL's cached copy first. */
void coverloader_default_cover_free(lv_img_dsc_t *src, lv_sdl_img_data_t *data);
#include "coverloader.h"
#include "app.h"
#include "appitem.view.h"

#include <stddef.h>

#include "util/bus.h"
#include "util/path.h"

#include "libgamestream/client.h"
#include "libgamestream/errors.h"

#include <SDL_image.h>
#include "draw/sdl/lv_draw_sdl.h"
#include "misc/lv_lru.h"
#include "util/img_loader.h"
#include "refcounter.h"

#include "res.h"

typedef struct memcache_key_t {
    int id;
    lv_coord_t target_width, target_height;
} memcache_key_t;

typedef struct memcache_item_t {
    lv_img_dsc_t src;
    lv_sdl_img_data_t data;
    lv_ll_t objs;
    lv_coord_t target_width, target_height, target_radius;
} memcache_item_t;

typedef struct img_loader_req_t {
    coverloader_t *loader;
    uuidstr_t server_id;
    int id;
    lv_obj_t *target;
    lv_coord_t target_width, target_height;
    /** The tile's corner radius, read on the main thread: the canvas is built on a worker. */
    lv_coord_t target_radius;
    memcache_item_t *src;
    bool finished;
    SDL_Surface *cached;
    img_loader_task_t *task;
    struct img_loader_req_t *prev;
    struct img_loader_req_t *next;
} coverloader_req_t;

#define LINKEDLIST_IMPL
#define LINKEDLIST_TYPE coverloader_req_t
#define LINKEDLIST_PREFIX reqlist
#define LINKEDLIST_DOUBLE 1

#include "linked_list.h"
#include "lazy.h"
#include "draw/sdl/lv_draw_sdl_img.h"
#include "logging.h"

#undef LINKEDLIST_TYPE
#undef LINKEDLIST_PREFIX
#undef LINKEDLIST_DOUBLE
#define LINKEDLIST_IMPL

#ifndef DEBUG
#define DEBUG 0
#endif

static const char *coverloader_cache_dir(coverloader_t *loader);

static GS_CLIENT coverloader_gs_client(coverloader_t *loader);

static void coverloader_cache_item_path(char path[4096], const coverloader_req_t *req);

static bool coverloader_memcache_get(coverloader_req_t *req);

static void coverloader_memcache_put(coverloader_req_t *req);

static bool coverloader_filecache_get(coverloader_req_t *req);

static void coverloader_filecache_put(coverloader_req_t *req);

static bool coverloader_fetch(coverloader_req_t *req);

static bool coverloader_run_on_main(img_loader_t *loader, img_loader_run_on_main_fn fn, void *args);

static void img_loader_start_cb(coverloader_req_t *req);

static void img_loader_result_cb(coverloader_req_t *req);

static void img_loader_cancel_cb(coverloader_req_t *req);

/**
 *
 * @param obj lv_img instance
 * @param src loaded image, NULL will fallback to default cover
 */
static void img_set_cover(lv_obj_t *obj, memcache_item_t *src);

static struct memcache_item_t *memcache_item_new();

static void memcache_item_free(memcache_item_t *item);

static void coverloader_req_free(coverloader_req_t *req);

static int reqlist_find_by_target(coverloader_req_t *p, const void *v);

static bool cover_is_placeholder(const SDL_Surface *surface);

static void target_deleted_cb(lv_event_t *e);

static void target_src_unlink_cb(lv_event_t *e);

static void memcache_item_ref_obj(memcache_item_t *item, lv_obj_t *obj);

static void memcache_item_unref_obj(memcache_item_t *item, const lv_obj_t *obj);

static void purge_img_cache(lv_draw_sdl_ctx_t *ctx, const memcache_item_t *item);

static void purge_src_cache(lv_draw_sdl_ctx_t *ctx, const lv_img_dsc_t *src);

static SDL_Surface *cover_canvas_new(SDL_Surface *decoded, int disp_w, int disp_h, int radius, bool stretch);

static void purge_corners_cache(lv_draw_sdl_ctx_t *ctx, const memcache_item_t *item);

static const img_loader_impl_t coverloader_impl = {
        .memcache_get = coverloader_memcache_get,
        .memcache_put = coverloader_memcache_put,
        .filecache_get = coverloader_filecache_get,
        .filecache_put = coverloader_filecache_put,
        .fetch = coverloader_fetch,
        .run_on_main = coverloader_run_on_main,
};

static const img_loader_cb_t coverloader_cb = {
        .start_cb = img_loader_start_cb,
        .complete_cb = img_loader_result_cb,
        .fail_cb = img_loader_result_cb,
        .cancel_cb = img_loader_cancel_cb,
};

struct coverloader_t {
    img_loader_t *base_loader;
    lv_lru_t *mem_cache;
    lazy_t client;
    lazy_t cache_dir;
    coverloader_req_t *reqlist;
    refcounter_t refcounter;
};

typedef struct subimage_info_t {
    int w, h;
    SDL_Rect rect;
} subimage_info_t;

coverloader_t *coverloader_new(app_t *app) {
    coverloader_t *loader = malloc(sizeof(coverloader_t));
    refcounter_init(&loader->refcounter);
    loader->mem_cache = lv_lru_create(1024 * 1024 * 32, 720 * 1024, (lv_lru_free_t *) memcache_item_free, NULL);
    loader->base_loader = img_loader_create(&coverloader_impl, app->backend.executor);
    lazy_init(&loader->client, (lazy_supplier) app_gs_client_new, app);
    lazy_init(&loader->cache_dir, (lazy_supplier) path_cache, NULL);
    loader->reqlist = NULL;
    return loader;
}

void coverloader_unref(coverloader_t *loader) {
    if (!refcounter_unref(&loader->refcounter)) {
        return;
    }
    GS_CLIENT client = lazy_deinit(&loader->client);
    if (client != NULL) {
        gs_destroy(client);
    }
    char *cache_dir = lazy_deinit(&loader->cache_dir);
    if (cache_dir != NULL) {
        free(cache_dir);
    }
    img_loader_destroy(loader->base_loader);
    lv_lru_del(loader->mem_cache);
    refcounter_destroy(&loader->refcounter);
    free(loader);
}

void coverloader_display(coverloader_t *loader, const uuidstr_t *uuid, int id, lv_obj_t *target,
                         lv_coord_t target_width, lv_coord_t target_height) {
    coverloader_req_t *existing = reqlist_find_by(loader->reqlist, target, reqlist_find_by_target);
    if (existing && existing->task) {
        img_loader_cancel(loader->base_loader, existing->task);
    }

    coverloader_req_t *req = reqlist_new();
    req->loader = loader;
    req->server_id = *uuid;
    req->id = id;
    req->target = target;
    req->target_width = target_width;
    req->target_height = target_height;
    req->target_radius = lv_obj_get_style_radius(target, LV_PART_MAIN);
    req->finished = false;
    lv_obj_add_event_cb(target, target_deleted_cb, LV_EVENT_DELETE, req);
    loader->reqlist = reqlist_append(loader->reqlist, req);
    refcounter_ref(&loader->refcounter);
    img_loader_task_t *task = img_loader_load(loader->base_loader, req, &coverloader_cb);
    /* If no task returned, then the request has been freed already */
    if (!task) { return; }
    req->task = task;
}

static const char *coverloader_cache_dir(coverloader_t *loader) {
    return lazy_obtain(&loader->cache_dir);
}

static GS_CLIENT coverloader_gs_client(coverloader_t *loader) {
    return lazy_obtain(&loader->client);
}

static void coverloader_cache_item_path(char path[4096], const coverloader_req_t *req) {
    const char *cachedir = coverloader_cache_dir(req->loader);
    char basename[128];
    SDL_snprintf(basename, 128, "%s_%d", (char *) &req->server_id, req->id);
    path_join_to(path, 4096, cachedir, basename);
}

static bool coverloader_memcache_get(coverloader_req_t *req) {
    // Uses result cache instead
    memcache_item_t *result = NULL;
    memcache_key_t key;
    memset(&key, 0, sizeof(key));
    key.id = req->id;
    key.target_width = req->target_width;
    key.target_height = req->target_height;

    (void) key.target_width;
    (void) key.target_height;

    lv_lru_get(req->loader->mem_cache, &key, sizeof(key), (void **) &result);
    req->src = result;

    return result != NULL;
}

static void coverloader_memcache_put(coverloader_req_t *req) {
    SDL_Surface *cached = req->cached;
    if (cached == NULL) {
        return;
    }
    memcache_item_t *result = NULL;
    memcache_key_t key = {
            .id = req->id,
            .target_width = req->target_width,
            .target_height = req->target_height
    };
    lv_lru_get(req->loader->mem_cache, &key, sizeof(key), (void **) &result);
    if (result == NULL) {
        lv_draw_sdl_drv_param_t *param = lv_disp_get_default()->driver->user_data;
        SDL_Renderer *renderer = param->renderer;

        result = memcache_item_new();
        result->target_width = req->target_width;
        result->target_height = req->target_height;
        result->target_radius = lv_obj_get_style_radius(req->target, 0);
        lv_img_dsc_t *src = &result->src;
        lv_sdl_img_data_t *data = &result->data;
        data->type = LV_SDL_IMG_TYPE_TEXTURE;
        src->header.cf = LV_IMG_CF_TRUE_COLOR;
        if (SDL_ISPIXELFORMAT_ALPHA(cached->format->format)) {
            src->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
        }
        int value_length = cached->w * cached->h;
        if (cached->userdata) {
            subimage_info_t *info = cached->userdata;
            data->rect = info->rect;
            value_length = info->w * info->h;
            src->header.w = info->w;
            src->header.h = info->h;
        } else {
            src->header.w = cached->w;
            src->header.h = cached->h;
        }
        data->data.texture = SDL_CreateTextureFromSurface(renderer, cached);
        src->data_size = sizeof(lv_sdl_img_data_t);
        src->data = (const uint8_t *) data;
        lv_lru_set(req->loader->mem_cache, &key, sizeof(key), result, value_length);
    }
    req->src = result;
    req->finished = true;
    if (cached->userdata) {
        SDL_free(cached->userdata);
    }
    SDL_FreeSurface(cached);
}

/*
 * ROUNDED CORNERS LIVE IN THE PIXELS. The tile's corner mask cannot round an image on the SDL
 * backend this app builds: with LV_GPU_SDL_CUSTOM_BLEND_MODE 0 (lv_conf.h) lv_draw_sdl_composite.c
 * reduces the mask to a rectangle, and LVGL's rounded-image path (lv_draw_sdl_img.c
 * draw_img_rounded) builds its corner pieces with an SDL_BLENDMODE_MOD render-target pass that came
 * out EMPTY on the Windows client -- a black square in every corner (2026-09-27). So the canvas
 * carries its own transparent, antialiased corners at the tile's radius, the tile does not clip,
 * and LVGL only ever copies it -- the same on every renderer.
 */
static void canvas_round_corners(SDL_Surface *canvas, int radius) {
    if (radius <= 0 || canvas->format->format != SDL_PIXELFORMAT_ARGB8888) {
        return;
    }
    radius = SDL_min(radius, SDL_min(canvas->w, canvas->h) / 2);
    if (SDL_LockSurface(canvas) != 0) {
        return;
    }
    for (int y = 0; y < radius; ++y) {
        for (int x = 0; x < radius; ++x) {
            /* Coverage of this pixel by the corner circle, from its centre's distance. */
            const float dx = (float) radius - ((float) x + 0.5f);
            const float dy = (float) radius - ((float) y + 0.5f);
            float cover = (float) radius + 0.5f - SDL_sqrtf(dx * dx + dy * dy);
            if (cover >= 1.0f) {
                continue;
            }
            if (cover < 0.0f) {
                cover = 0.0f;
            }
            const int xs[2] = {x, canvas->w - 1 - x};
            const int ys[2] = {y, canvas->h - 1 - y};
            for (int i = 0; i < 2; ++i) {
                Uint32 *row = (Uint32 *) ((Uint8 *) canvas->pixels + ys[i] * canvas->pitch);
                for (int j = 0; j < 2; ++j) {
                    Uint32 *px = &row[xs[j]];
                    const Uint32 alpha = (*px >> 24) & 0xFFu;
                    *px = (*px & 0x00FFFFFFu) | ((Uint32) ((float) alpha * cover + 0.5f) << 24);
                }
            }
        }
    }
    SDL_UnlockSurface(canvas);
}

/** The tile's picture: @p decoded letterboxed (or, with @p stretch, stretched) onto a 2x canvas of
 *  disp_w x disp_h with the tile's rounded corners in its alpha. Takes ownership of @p decoded;
 *  NULL on failure. */
static SDL_Surface *cover_canvas_new(SDL_Surface *decoded, int disp_w, int disp_h, int radius, bool stretch) {
    const int sw = decoded->w, sh = decoded->h;
    if (sw < 1 || sh < 1) {
        SDL_FreeSurface(decoded);
        return NULL;
    }

    /* 2× canvas for sharper downscale on 4K panels; LVGL maps it to disp_w × disp_h. */
    int tex_w = disp_w * 2;
    int tex_h = disp_h * 2;
    if (tex_w < 1) {
        tex_w = 1;
    }
    if (tex_h < 1) {
        tex_h = 1;
    }

    const double scale_w = tex_w / (double) sw;
    const double scale_h = tex_h / (double) sh;
    const double scale = scale_w < scale_h ? scale_w : scale_h;
    int fit_w = (int) (sw * scale + 0.5);
    int fit_h = (int) (sh * scale + 0.5);
    if (fit_w < 1) {
        fit_w = 1;
    }
    if (fit_h < 1) {
        fit_h = 1;
    }
    if (stretch) {
        /* The placeholder always filled the tile (LVGL scaled it to the header size). */
        fit_w = tex_w;
        fit_h = tex_h;
    }

    const SDL_PixelFormat *format = decoded->format;
    SDL_Surface *canvas = SDL_CreateRGBSurface(0, tex_w, tex_h, format->BitsPerPixel,
                                               format->Rmask, format->Gmask, format->Bmask, format->Amask);
    if (canvas == NULL) {
        SDL_FreeSurface(decoded);
        return NULL;
    }
    SDL_FillRect(canvas, NULL, SDL_MapRGBA(format, 0, 0, 0, 255));

    SDL_Rect dst = {
            .x = (tex_w - fit_w) / 2,
            .y = (tex_h - fit_h) / 2,
            .w = fit_w,
            .h = fit_h,
    };
    if (SDL_BlitScaled(decoded, NULL, canvas, &dst) != 0) {
        SDL_FreeSurface(canvas);
        SDL_FreeSurface(decoded);
        return NULL;
    }
    SDL_FreeSurface(decoded);

    /* Exact (unscaled) conversion, so the corners can be given an alpha channel whatever the
     * cover's own format was. */
    SDL_Surface *argb = SDL_ConvertSurfaceFormat(canvas, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(canvas);
    if (argb == NULL) {
        return NULL;
    }
    canvas = argb;
    canvas_round_corners(canvas, radius * tex_w / (disp_w > 0 ? disp_w : 1));

    subimage_info_t *info = SDL_malloc(sizeof(subimage_info_t));
    if (info == NULL) {
        SDL_FreeSurface(canvas);
        return NULL;
    }
    info->w = disp_w;
    info->h = disp_h;
    info->rect = (SDL_Rect) {0, 0, tex_w, tex_h};

    canvas->userdata = info;
    return canvas;
}

static bool coverloader_filecache_get(coverloader_req_t *req) {
#if !DEBUG
    SDL_version ver;
    SDL_GetVersion(&ver);
    if (SDL_VERSIONNUM(ver.major, ver.minor, ver.patch) <= SDL_VERSIONNUM(2, 0, 1)) {
        return false;
    }
#endif
    char path[4096];
    coverloader_cache_item_path(path, req);
    SDL_Surface *decoded = IMG_Load(path);
    if (!decoded) {
        commons_log_warn("CoverLoader", "Failed to load cover from %s: %s", path, IMG_GetError());
        return false;
    }
    if (cover_is_placeholder(decoded)) {
        SDL_FreeSurface(decoded);
        return false;
    }
    // Indexed images needs to be converted to true color before scaling
    if (decoded->format->palette != NULL) {
#if !SDL_VERSION_ATLEAST(2, 30, 0)
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
#define SDL_PIXELFORMAT_RGBX32 SDL_PIXELFORMAT_RGBX8888
#else
#define SDL_PIXELFORMAT_RGBX32 SDL_PIXELFORMAT_XBGR8888
#endif
#endif
        SDL_Surface *true_color = SDL_ConvertSurfaceFormat(decoded, SDL_PIXELFORMAT_RGBX32, 0);
        SDL_FreeSurface(decoded);
        if (true_color == NULL) {
            return false;
        }
        decoded = true_color;
    }
    SDL_Surface *canvas = cover_canvas_new(decoded, req->target_width, req->target_height, req->target_radius,
                                           false);
    if (canvas == NULL) {
        return false;
    }
    req->cached = canvas;
    return true;
}

static bool cover_is_placeholder(const SDL_Surface *surface) {
    return (surface->w == 130 && surface->h == 180) || (surface->w == 628 && surface->h == 888);
}

static void coverloader_filecache_put(coverloader_req_t *req) {
    // This was done in fetch step
    (void) req;
}

static bool coverloader_fetch(coverloader_req_t *req) {
#if !DEBUG
    SDL_version ver;
    SDL_GetVersion(&ver);
    if (SDL_VERSIONNUM(ver.major, ver.minor, ver.patch) <= SDL_VERSIONNUM(2, 0, 1)) {
        return NULL;
    }
#endif
    const pclist_t *node = pcmanager_node(pcmanager, &req->server_id);
    if (!node) {
        return false;
    }
    char path[4096];
    coverloader_cache_item_path(path, req);
    GS_CLIENT client = coverloader_gs_client(req->loader);
    if (gs_download_cover(client, node->server, req->id, path) != GS_OK) {
        return false;
    }
    return coverloader_filecache_get(req);
}

static bool coverloader_run_on_main(img_loader_t *loader, img_loader_run_on_main_fn fn, void *args) {
    (void) loader;
    /* The result matters: false (the app is quitting) tells img_loader not to wait. */
    return app_bus_post(global, (bus_actionfunc) fn, args);
}

static void img_loader_start_cb(coverloader_req_t *req) {
    appitem_viewholder_t *holder = req->target->user_data;
    img_set_cover(req->target, NULL);
    lv_obj_add_flag(holder->title, LV_OBJ_FLAG_HIDDEN);
}

static void img_loader_result_cb(coverloader_req_t *req) {
    req->task = NULL;
    coverloader_t *loader = req->loader;
    if (req->target == NULL) {
        goto done;
    }

    lv_obj_remove_event_cb(req->target, target_deleted_cb);
    appitem_viewholder_t *holder = req->target->user_data;
    img_set_cover(req->target, req->src);
    if (req->src) {
        lv_obj_add_flag(holder->title, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(holder->title, LV_OBJ_FLAG_HIDDEN);
    }
    done:
    loader->reqlist = reqlist_remove(loader->reqlist, req);
    coverloader_req_free(req);
    coverloader_unref(loader);
}

static void img_loader_cancel_cb(coverloader_req_t *req) {
    if (req->target) {
        lv_obj_remove_event_cb(req->target, target_deleted_cb);
    }
    req->task = NULL;
    coverloader_t *loader = req->loader;
    loader->reqlist = reqlist_remove(loader->reqlist, req);
    coverloader_req_free(req);
    coverloader_unref(loader);
}

static void img_set_cover(lv_obj_t *obj, memcache_item_t *src) {
    appitem_viewholder_t *holder = lv_obj_get_user_data(obj);

    /* If the old src is not default cover, assume it's memcache item and unref this obj from it */
    const void *old_src = lv_img_get_src(obj);
    bool skip_add_del_cb = false;
    /* The placeholder is excluded BY ADDRESS: the shape test below was the only guard, and the
     * built placeholder (coverloader_default_cover_build) is also a descriptor pointing at an
     * lv_sdl_img_data_t -- read as a memcache item, it crashed at startup (2026-09-27). */
    if (old_src != NULL && old_src != &holder->styles->defcover_src &&
        lv_img_src_get_type(old_src) == LV_IMG_SRC_VARIABLE) {
        const memcache_item_t *old_img = old_src;
        /* for memcache item, src.data is pointer to data */
        if (old_img->src.data == (const void *) &old_img->data && old_img->src.data_size == sizeof(lv_sdl_img_data_t)) {
            memcache_item_unref_obj((memcache_item_t *) old_img, obj);
            skip_add_del_cb = true;
        }
    }

    if (src != NULL) {
        lv_img_set_src(obj, src);
        memcache_item_ref_obj(src, obj);
        // If the object is deleted, mark the src orphaned. So it will not access obj when recycled.
        if (!skip_add_del_cb) {
            lv_obj_add_event_cb(obj, target_src_unlink_cb, LV_EVENT_DELETE, NULL);
        }
    } else {
        lv_img_set_src(obj, &holder->styles->defcover_src);
        lv_obj_remove_event_cb(obj, target_src_unlink_cb);
    }
}

struct memcache_item_t *memcache_item_new() {
    memcache_item_t *item = calloc(1, sizeof(memcache_item_t));
    _lv_ll_init(&item->objs, sizeof(lv_obj_t *));
    return item;
}

static void memcache_item_free(memcache_item_t *item) {
    lv_disp_drv_t *driver = lv_disp_get_default()->driver;
    lv_draw_sdl_ctx_t *ctx = (lv_draw_sdl_ctx_t *) driver->draw_ctx;

    /* Make sure the obj isn't referencing the src anymore */
    lv_obj_t **i;
    _LV_LL_READ(&item->objs, i) {
        lv_obj_t *obj = *i;
        if (obj == NULL) { continue; }
        appitem_viewholder_t *holder = lv_obj_get_user_data(obj);
        lv_img_set_src(obj, &holder->styles->defcover_src);
        lv_obj_remove_event_cb(obj, target_src_unlink_cb);
        lv_obj_clear_flag(holder->title, LV_OBJ_FLAG_HIDDEN);
    }
    SDL_DestroyTexture(item->data.data.texture);

    // Purge internal texture cache too
    purge_img_cache(ctx, item);
    purge_corners_cache(ctx, item);

    /* unref all objs to this item */
    _lv_ll_clear(&item->objs);
    free(item);
}

static void purge_src_cache(lv_draw_sdl_ctx_t *ctx, const lv_img_dsc_t *src) {
    struct __attribute__ ((__packed__)) {
        lv_draw_sdl_cache_key_head_img_t header;
        const void *src;
    } key_to_remove;
    memset(&key_to_remove, 0, sizeof(key_to_remove));
    key_to_remove.header.magic = LV_GPU_CACHE_KEY_MAGIC_IMG;
    key_to_remove.header.type = LV_IMG_SRC_VARIABLE;
    key_to_remove.header.frame_id = 0;
    key_to_remove.src = src;

    lv_lru_remove(ctx->internals->texture_cache, &key_to_remove, sizeof(key_to_remove));
}

static void purge_img_cache(lv_draw_sdl_ctx_t *ctx, const memcache_item_t *item) {
    purge_src_cache(ctx, &item->src);
}

void coverloader_default_cover_free(lv_img_dsc_t *src, lv_sdl_img_data_t *data) {
    lv_draw_sdl_ctx_t *ctx = (lv_draw_sdl_ctx_t *) lv_disp_get_default()->driver->draw_ctx;
    /* LVGL caches a texture per descriptor ADDRESS, so its entry goes before the descriptor's
     * content changes -- otherwise it keeps drawing the old texture, or a destroyed one. */
    purge_src_cache(ctx, src);
    if (data->type == LV_SDL_IMG_TYPE_TEXTURE && data->data.texture != NULL) {
        SDL_DestroyTexture(data->data.texture);
        data->data.texture = NULL;
    }
}

bool coverloader_default_cover_build(lv_img_dsc_t *src, lv_sdl_img_data_t *data, lv_coord_t width,
                                     lv_coord_t height, lv_coord_t radius) {
    /* Same tile size as the texture was BUILT at (its 2x source rect -- set only by a successful
     * build, unlike the header, which the fallback also writes): nothing to redo. The radius is
     * fixed per fragment. */
    if (data->type == LV_SDL_IMG_TYPE_TEXTURE && data->data.texture != NULL && data->rect.w == width * 2 &&
        data->rect.h == height * 2) {
        return true;
    }
    SDL_RWops *rw = SDL_RWFromConstMem(lv_sdl_img_data_defcover.data.constptr,
                                       (int) lv_sdl_img_data_defcover.data_len);
    SDL_Surface *decoded = rw != NULL ? IMG_Load_RW(rw, 1) : NULL;
    if (decoded == NULL) {
        commons_log_warn("CoverLoader", "Default cover did not decode: %s", IMG_GetError());
        return false;
    }
    SDL_Surface *canvas = cover_canvas_new(decoded, width, height, radius, true);
    if (canvas == NULL) {
        commons_log_warn("CoverLoader", "Default cover canvas failed at %dx%d", width, height);
        return false;
    }
    const lv_draw_sdl_drv_param_t *param = lv_disp_get_default()->driver->user_data;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(param->renderer, canvas);
    const SDL_Rect rect = ((const subimage_info_t *) canvas->userdata)->rect;
    SDL_free(canvas->userdata);
    SDL_FreeSurface(canvas);
    if (texture == NULL) {
        commons_log_warn("CoverLoader", "Default cover texture failed: %s", SDL_GetError());
        return false;
    }
    coverloader_default_cover_free(src, data);
    data->type = LV_SDL_IMG_TYPE_TEXTURE;
    data->data.texture = texture;
    data->rect = rect;
    src->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    src->header.w = width;
    src->header.h = height;
    src->data_size = sizeof(lv_sdl_img_data_t);
    src->data = (const uint8_t *) data;
    return true;
}

static void purge_corners_cache(lv_draw_sdl_ctx_t *ctx, const memcache_item_t *item) {
    struct {
        lv_sdl_cache_key_magic_t magic;
        const SDL_Texture *texture;
        lv_coord_t w, h, radius;
    } key_to_remove;

    memset(&key_to_remove, 0, sizeof(key_to_remove));
    key_to_remove.magic = LV_GPU_CACHE_KEY_MAGIC_IMG_ROUNDED_CORNERS;
    key_to_remove.texture = item->data.data.texture;
    key_to_remove.w = item->target_width;
    key_to_remove.h = item->target_height;
    key_to_remove.radius = LV_MIN3(item->target_radius, item->target_width, item->target_height);

    lv_lru_remove(ctx->internals->texture_cache, &key_to_remove, sizeof(key_to_remove));
}

static void memcache_item_ref_obj(memcache_item_t *item, lv_obj_t *obj) {
    lv_obj_t **node = _lv_ll_ins_tail(&item->objs);
    *node = obj;
}

static void memcache_item_unref_obj(memcache_item_t *item, const lv_obj_t *obj) {
    lv_obj_t **i;
    _LV_LL_READ_BACK(&item->objs, i) {
        if (obj == *i) {
            break;
        }
    }
    if (i == NULL) { return; }
    _lv_ll_remove(&item->objs, i);
    lv_mem_free(i);
}

static inline void coverloader_req_free(coverloader_req_t *req) {
    free(req);
}

static int reqlist_find_by_target(coverloader_req_t *p, const void *v) {
    return p->target != v;
}

static void target_deleted_cb(lv_event_t *e) {
    coverloader_req_t *req = lv_event_get_user_data(e);
    req->target = NULL;
}

static void target_src_unlink_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    memcache_item_unref_obj((memcache_item_t *) lv_img_get_src(obj), obj);
}
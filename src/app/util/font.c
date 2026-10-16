#include "font.h"
#include "ui/config.h"
#include "i18n.h"
#include "res.h"
#include "util/path.h"

#if defined(TARGET_ANDROID)
#include <unistd.h>
#else
#include <fontconfig/fontconfig.h>
#endif
#include <stdlib.h>


#if !defined(TARGET_ANDROID)
static bool fontset_load_fc(app_fontset_t *set, FcPattern *font);
#endif

static bool fontset_load_path(app_fontset_t *set, const char *file);

static bool fontset_load_mem(app_fontset_t *set, const char *name, const void *mem, size_t size);

static void fontset_destroy_fonts(app_fontset_t *fontset);

#ifdef _WIN32
static FcConfig *portable_fontconfig = NULL;

static FcConfig *portable_fontconfig_load(void) {
    if (portable_fontconfig != NULL) {
        return portable_fontconfig;
    }
    char *base = path_assets();
    char *etc = base != NULL ? path_join(base, "etc") : NULL;
    char *fonts = etc != NULL ? path_join(etc, "fonts") : NULL;
    char *config_path = fonts != NULL ? path_join(fonts, "fonts.conf") : NULL;
    free(base);
    free(etc);
    free(fonts);
    if (config_path == NULL) {
        return NULL;
    }

    FcConfig *config = FcConfigCreate();
    const FcBool loaded = config != NULL &&
                          FcConfigParseAndLoad(config, (const FcChar8 *) config_path, FcTrue) &&
                          FcConfigBuildFonts(config);
    free(config_path);
    if (!loaded) {
        if (config != NULL) {
            FcConfigDestroy(config);
        }
        return NULL;
    }
    portable_fontconfig = config;
    return portable_fontconfig;
}
#endif

int app_font_init(app_fonts_t *fonts, int dpi) {
    app_fontset_t fontset = {
            .small_size = _LV_DPX_CALC(dpi, 14),
            .normal_size = _LV_DPX_CALC(dpi, 16),
            .large_size = _LV_DPX_CALC(dpi, 19)
    };
    app_fontset_t iconfonts = fontset;
    if (!fontset_load_mem(&iconfonts, "MaterialIcons", res_mat_iconfont_data, res_mat_iconfont_size)) {
        return -1;
    }
#if defined(TARGET_ANDROID)
    /* No fontconfig on Android: the system's own UI font, and its CJK font as the fallback for host and game
     * names in any script (the desktops' "sans-serif" fallback). Earlier entries are newer Android releases. */
    static const char *const primary[] = {
            "/system/fonts/Roboto-Regular.ttf", "/system/fonts/RobotoStatic-Regular.ttf",
            "/system/fonts/NotoSans-Regular.ttf", "/system/fonts/DroidSans.ttf", NULL,
    };
    static const char *const fallback[] = {
            "/system/fonts/NotoSansCJK-Regular.ttc", "/system/fonts/NotoSansSC-Regular.otf",
            "/system/fonts/DroidSansFallbackFull.ttf", "/system/fonts/DroidSansFallback.ttf", NULL,
    };
    bool loaded = false;
    for (int i = 0; primary[i] != NULL && !loaded; i++) {
        loaded = access(primary[i], R_OK) == 0 && fontset_load_path(&fontset, primary[i]);
    }
    if (!loaded) {
        fontset_destroy_fonts(&iconfonts);
        return -1;
    }
    for (int i = 0; fallback[i] != NULL; i++) {
        if (access(fallback[i], R_OK) != 0) {
            continue;
        }
        app_fontset_t *fb = calloc(1, sizeof(app_fontset_t));
        if (fb == NULL) {
            break;
        }
        fb->small_size = fontset.small_size;
        fb->normal_size = fontset.normal_size;
        fb->large_size = fontset.large_size;
        if (fontset_load_path(fb, fallback[i])) {
            fontset.fallback = fb;
            fontset.normal->fallback = fb->normal;
            fontset.large->fallback = fb->large;
            fontset.small->fallback = fb->small;
            break;
        }
        free(fb);
    }
#else
#ifdef _WIN32
    FcConfig *fc_config = portable_fontconfig_load();
    if (fc_config == NULL) {
        fontset_destroy_fonts(&iconfonts);
        return -1;
    }
#else
    FcConfig *fc_config = NULL;
#endif
    //does not necessarily have to be a specific name.  You could put anything here and Fontconfig WILL find a font for you
    FcPattern *pattern = FcNameParse((const FcChar8 *) FONT_FAMILY);
    if (!pattern) {
        fontset_destroy_fonts(&iconfonts);
        return -1;
    }

    FcConfigSubstitute(fc_config, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    FcResult result;

    FcPattern *font = FcFontMatch(fc_config, pattern, &result);
    if (font == NULL) {
        FcPatternDestroy(pattern);
        fontset_destroy_fonts(&iconfonts);
        return -1;
    }
    if (!fontset_load_fc(&fontset, font)) {
        FcPatternDestroy(font);
        FcPatternDestroy(pattern);
        fontset_destroy_fonts(&iconfonts);
        return -1;
    }
    if (font) {
        FcPatternDestroy(font);
        font = NULL;
    }
    FcPatternDestroy(pattern);
    pattern = NULL;
#ifdef FONT_FAMILY_FALLBACK
    const i18n_entry_t *loc_entry = i18n_entry(i18n_locale());
    pattern = FcNameParse((const FcChar8 *) ((loc_entry && loc_entry->font) ? loc_entry->font : FONT_FAMILY_FALLBACK));
    if (pattern != NULL) {
        FcLangSet *ls = FcLangSetCreate();
        if (loc_entry) {
            FcLangSetAdd(ls, (const FcChar8 *) loc_entry->locale);
            FcPatternAddLangSet(pattern, FC_LANG, ls);
        }

        FcConfigSubstitute(fc_config, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);

        font = FcFontMatch(fc_config, pattern, &result);
        if (font != NULL) {
            fontset.fallback = calloc(1, sizeof(app_fontset_t));
            if (fontset.fallback != NULL) {
                fontset.fallback->small_size = fontset.small_size;
                fontset.fallback->normal_size = fontset.normal_size;
                fontset.fallback->large_size = fontset.large_size;
            }
            if (fontset.fallback != NULL && fontset_load_fc(fontset.fallback, font)) {
                fontset.normal->fallback = fontset.fallback->normal;
                fontset.large->fallback = fontset.fallback->large;
                fontset.small->fallback = fontset.fallback->small;
            } else {
                free(fontset.fallback);
                fontset.fallback = NULL;
            }
            FcPatternDestroy(font);
            font = NULL;
        }
        FcLangSetDestroy(ls);
        FcPatternDestroy(pattern);
        pattern = NULL;
    }
#endif
#endif /* TARGET_ANDROID */
    fonts->fonts = fontset;
    fonts->icons = iconfonts;
    return 0;
}

void app_font_deinit(app_fonts_t *fonts) {
    fontset_destroy_fonts(&fonts->fonts);
    fontset_destroy_fonts(&fonts->icons);
#ifdef _WIN32
    if (portable_fontconfig != NULL) {
        FcConfigDestroy(portable_fontconfig);
        portable_fontconfig = NULL;
    }
#endif
}

#if !defined(TARGET_ANDROID)
static bool fontset_load_fc(app_fontset_t *set, FcPattern *font) {
    //The pointer stored in 'file' is tied to 'font'; therefore, when 'font' is freed, this pointer is freed automatically.
    //If you want to return the filename of the selected font, pass a buffer and copy the file name into that buffer
    FcChar8 *file = NULL;

    if (FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch) {
        return fontset_load_path(set, (const char *) file);
    }
    return false;
}
#endif

/* The three sizes from one font file; all or nothing. */
static bool fontset_load_path(app_fontset_t *set, const char *file) {
    lv_ft_info_t ft_info = {.name=(char *) file, .style = FT_FONT_STYLE_NORMAL, .weight = set->normal_size};
    if (lv_ft_font_init(&ft_info)) {
        set->normal = ft_info.font;
    }
    lv_ft_info_t ft_info_lg = {.name=(char *) file, .style = FT_FONT_STYLE_NORMAL, .weight = set->large_size};
    if (lv_ft_font_init(&ft_info_lg)) {
        set->large = ft_info_lg.font;
    }
    lv_ft_info_t ft_info_sm = {.name=(char *) file, .style = FT_FONT_STYLE_NORMAL, .weight = set->small_size};
    if (lv_ft_font_init(&ft_info_sm)) {
        set->small = ft_info_sm.font;
    }
    if (set->small != NULL && set->normal != NULL && set->large != NULL) {
        return true;
    }
    if (set->small != NULL) {
        lv_ft_font_destroy(set->small);
        set->small = NULL;
    }
    if (set->normal != NULL) {
        lv_ft_font_destroy(set->normal);
        set->normal = NULL;
    }
    if (set->large != NULL) {
        lv_ft_font_destroy(set->large);
        set->large = NULL;
    }
    return false;
}

static bool fontset_load_mem(app_fontset_t *set, const char *name, const void *mem, size_t size) {
    lv_ft_info_t ft_info = {.name = name, .mem=mem, .mem_size = size, .style = FT_FONT_STYLE_NORMAL,
            .weight = set->normal_size};
    if (lv_ft_font_init(&ft_info)) {
        set->normal = ft_info.font;
    }
    lv_ft_info_t ft_info_lg = {.name = name, .mem=mem, .mem_size = size, .style = FT_FONT_STYLE_NORMAL,
            .weight = set->large_size};
    if (lv_ft_font_init(&ft_info_lg)) {
        set->large = ft_info_lg.font;
    }
    lv_ft_info_t ft_info_sm = {.name = name, .mem=mem, .mem_size = size, .style = FT_FONT_STYLE_NORMAL,
            .weight = set->small_size};
    if (lv_ft_font_init(&ft_info_sm)) {
        set->small = ft_info_sm.font;
    }
    if (!set->small || !set->normal || !set->large) {
        if (set->small) {
            lv_ft_font_destroy(set->small);
            set->small = NULL;
        }
        if (set->normal) {
            lv_ft_font_destroy(set->normal);
            set->normal = NULL;
        }
        if (set->large) {
            lv_ft_font_destroy(set->large);
            set->large = NULL;
        }
        return false;
    }
    return true;
}

static void fontset_destroy_fonts(app_fontset_t *fontset) {
    lv_ft_font_destroy(fontset->large);
    lv_ft_font_destroy(fontset->normal);
    lv_ft_font_destroy(fontset->small);
    app_fontset_t *fallback = fontset->fallback;
    if (fallback) {
        lv_ft_font_destroy(fallback->large);
        lv_ft_font_destroy(fallback->normal);
        lv_ft_font_destroy(fallback->small);
        free(fallback);
    }
}

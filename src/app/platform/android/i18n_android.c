/*
 * Translations on Android, which has no gettext runtime.
 *
 * The catalogues are the very files the desktops install -- msgfmt's output for src/i18n/<lang>/messages.po
 * (cmake/MoonlightI18n.cmake) -- packaged as APK assets under locale/<lang>/LC_MESSAGES/moonlight-tv.mo. This
 * reads that format directly: a GNU .mo file holds its original strings sorted, so a lookup is a binary search
 * over the file in memory. The language follows the in-app setting, or else the system's preferred locales,
 * exactly as on webOS (platform/android/app_android.c, app_init_locale).
 */
#include "util/i18n.h"
#include "util/path.h"
#include "android_platform.h"

#include "logging.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MO_MAGIC 0x950412deu
#define MO_MAGIC_SWAPPED 0xde120495u

static unsigned char *catalogue = NULL;
static size_t catalogue_size = 0;
static uint32_t string_count = 0, originals_offset = 0, translations_offset = 0;
static int swapped = 0;
static char current_locale[16] = "";

static uint32_t read_u32(size_t offset) {
    uint32_t v;
    memcpy(&v, catalogue + offset, sizeof(v));
    if (swapped) {
        v = ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) | ((v >> 8) & 0xff00u) | (v >> 24);
    }
    return v;
}

/* A table entry {length, offset} names a NUL-terminated string inside the file, or the entry is unusable. */
static const char *entry_string(uint32_t table, uint32_t index, uint32_t *length_out) {
    const size_t entry = (size_t) table + (size_t) index * 8u;
    if (entry + 8u > catalogue_size) {
        return NULL;
    }
    const uint32_t length = read_u32(entry), offset = read_u32(entry + 4u);
    if ((size_t) offset + length >= catalogue_size || catalogue[offset + length] != '\0') {
        return NULL;
    }
    if (length_out != NULL) {
        *length_out = length;
    }
    return (const char *) catalogue + offset;
}

static void catalogue_unload(void) {
    free(catalogue);
    catalogue = NULL;
    catalogue_size = 0;
    string_count = 0;
}

static bool catalogue_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    bool ok = false;
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 28 && fseek(f, 0, SEEK_SET) == 0) {
        catalogue = malloc((size_t) size);
        if (catalogue != NULL && fread(catalogue, 1, (size_t) size, f) == (size_t) size) {
            catalogue_size = (size_t) size;
            uint32_t magic;
            memcpy(&magic, catalogue, sizeof(magic));
            if (magic == MO_MAGIC || magic == MO_MAGIC_SWAPPED) {
                swapped = magic == MO_MAGIC_SWAPPED;
                string_count = read_u32(8);
                originals_offset = read_u32(12);
                translations_offset = read_u32(16);
                ok = (size_t) originals_offset + (size_t) string_count * 8u <= catalogue_size &&
                     (size_t) translations_offset + (size_t) string_count * 8u <= catalogue_size;
            }
        }
    }
    fclose(f);
    if (!ok) {
        catalogue_unload();
    }
    return ok;
}

const char *locstr(const char *msgid) {
    if (catalogue == NULL || msgid == NULL || msgid[0] == '\0') {
        return msgid;
    }
    uint32_t lo = 0, hi = string_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        const char *original = entry_string(originals_offset, mid, NULL);
        if (original == NULL) {
            return msgid;
        }
        const int cmp = strcmp(msgid, original);
        if (cmp == 0) {
            uint32_t length = 0;
            const char *translated = entry_string(translations_offset, mid, &length);
            return translated != NULL && length > 0 ? translated : msgid;
        }
        if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1u;
        }
    }
    return msgid;
}

bool i18n_is_loaded(void) {
    return catalogue != NULL;
}

const char *i18n_locale() {
    return current_locale;
}

void i18n_setlocale(const char *locale) {
    catalogue_unload();
    current_locale[0] = '\0';
    if (locale == NULL || locale[0] == '\0') {
        return;
    }
    snprintf(current_locale, sizeof(current_locale), "%s", locale);
    /* The catalogue directory uses gettext's spelling: "pt-BR" -> "pt_BR" (cmake/MoonlightI18n.cmake). */
    char dirname[16];
    snprintf(dirname, sizeof(dirname), "%s", locale);
    for (char *p = dirname; *p != '\0'; ++p) {
        if (*p == '-') {
            *p = '_';
        }
    }
    char *assets = path_assets();
    if (assets == NULL) {
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/locale/%s/LC_MESSAGES/moonlight-tv.mo", assets, dirname);
    free(assets);
    if (catalogue_load(path)) {
        commons_log_info("I18N", "Locale '%s' loaded (%u strings)", locale, (unsigned) string_count);
    } else if (strncmp(locale, "en", 2) != 0) {
        commons_log_warn("I18N", "No catalogue for locale '%s' (%s)", locale, path);
    }
}

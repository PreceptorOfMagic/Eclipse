#include "hints.h"
#include "hints_core.h"

#include "app.h"
#include "app_settings.h"
#include "util/i18n.h"
#include "util/path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if TARGET_WEBOS
#define HINT_PLATFORM HINT_PLATFORM_WEBOS
#elif TARGET_ANDROID
#define HINT_PLATFORM HINT_PLATFORM_ANDROID
#elif defined(_WIN32)
#define HINT_PLATFORM HINT_PLATFORM_WINDOWS
#else
#define HINT_PLATFORM HINT_PLATFORM_LINUX
#endif

/* The shown-before list, one tip id per line, beside moonlight.conf. */
#define HINTS_SEEN_FILE "hints_seen"

#define HINTS_COUNT (sizeof(hints_table) / sizeof(hints_table[0]))

static const hint_t hints_table[] = {
#include "hints_table.inc"
};

/* One history for the stream screen and the settings screen, so both follow the same rotation. */
static hints_seen_t seen;
static char *seen_path = NULL;
static bool loaded = false;

static void ensure_loaded(void) {
    if (loaded) {
        return;
    }
    loaded = true;
    if (app_configuration != NULL && app_configuration->conf_dir != NULL) {
        seen_path = path_join(app_configuration->conf_dir, HINTS_SEEN_FILE);
    }
    hints_seen_load(&seen, seen_path);
}

const char *hints_obtain(bool coop, bool hdr) {
    ensure_loaded();
    const int i = hints_choose(hints_table, HINTS_COUNT, HINT_PLATFORM, coop, hdr, &seen, seen_path);
    return i < 0 ? NULL : locstr(hints_table[i].text);
}

static size_t clamp_len(size_t len, size_t cap) {
    return len < cap ? len : cap - 1;   /* snprintf truncated: stop at the terminator */
}

const char *hints_obtain_settings(void) {
    ensure_loaded();
    const int i = hints_choose_settings(hints_table, HINTS_COUNT, HINT_PLATFORM, &seen, seen_path);
    return i < 0 ? NULL : locstr(hints_table[i].text);
}

/* Append "<title>\n1. tip\n2. tip\n..." for one group; nothing if this platform has none of it. */
static void append_group(char *buf, size_t cap, size_t *len, const char *title, hint_when_t when) {
    int idx[HINTS_COUNT];
    const size_t n = hints_ordered(hints_table, HINTS_COUNT, HINT_PLATFORM, when, idx, HINTS_COUNT);
    if (n == 0 || *len >= cap) {
        return;
    }
    int w = snprintf(buf + *len, cap - *len, "%s%s\n", *len > 0 ? "\n" : "", title);
    if (w > 0) {
        *len = clamp_len(*len + (size_t) w, cap);
    }
    for (size_t k = 0; k < n && *len < cap - 1; k++) {
        w = snprintf(buf + *len, cap - *len, "%d. %s\n", (int) k + 1, locstr(hints_table[idx[k]].text));
        if (w > 0) {
            *len = clamp_len(*len + (size_t) w, cap);
        }
    }
}

char *hints_all_text(void) {
    /* Generous: translations run longer than the English. */
    const size_t cap = HINTS_COUNT * 400 + 512;
    char *buf = malloc(cap);
    if (buf == NULL) {
        return NULL;
    }
    size_t len = 0;
    buf[0] = '\0';
    append_group(buf, cap, &len, locstr("Every stream"), HINT_ANY);
    append_group(buf, cap, &len, locstr("Co-op streams"), HINT_COOP);
    append_group(buf, cap, &len, locstr("HDR streams"), HINT_HDR);
    while (len > 0 && buf[len - 1] == '\n') {
        buf[--len] = '\0';
    }
    return buf;
}

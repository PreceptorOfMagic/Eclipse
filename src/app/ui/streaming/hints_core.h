#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * Streaming tips: which one to show next.
 *
 * Every tip has a stable string id and a rank per platform (1 = most important, 0 = not shown there). A stream
 * start shows the most important tip this device has NOT shown before, and remembers it, so a user who only ever
 * sees a handful of tips sees the important ones. Once every tip has been shown the history is cleared and the
 * order starts again from the top. Remembering ids rather than positions means a release can add, drop or
 * reorder tips: a new tip is simply unseen, so it comes up at its rank.
 *
 * No SDL, LVGL or i18n in here, so tests/app/ui links it alone.
 */

typedef enum hint_when_t {
    HINT_ANY = 0,  /* any stream */
    HINT_COOP,     /* co-op streams only; shown ahead of the general tips on a co-op stream */
    HINT_HDR,      /* HDR streams only; shown ahead of the general tips on an HDR stream */
} hint_when_t;

enum {
    HINT_PLATFORM_WEBOS = 0,
    HINT_PLATFORM_WINDOWS,
    HINT_PLATFORM_LINUX,
    HINT_PLATFORM_ANDROID,
    HINT_PLATFORM_COUNT,
};

typedef struct hint_t {
    const char *id;
    const char *text;
    hint_when_t when;
    unsigned char rank[HINT_PLATFORM_COUNT];
    /** Order in the settings bar, which shows every tip (co-op and HDR included) in one sequence. 0 = not there. */
    unsigned char settings_rank[HINT_PLATFORM_COUNT];
} hint_t;

#define HINTS_SEEN_MAX 256
#define HINT_ID_MAX 32

typedef struct hints_seen_t {
    char ids[HINTS_SEEN_MAX][HINT_ID_MAX];
    int count;
} hints_seen_t;

/** Read the shown-before list from @p path. A missing or unreadable file is an empty list. */
void hints_seen_load(hints_seen_t *seen, const char *path);

bool hints_seen_has(const hints_seen_t *seen, const char *id);

/** Add @p id to the list and append it to @p path (NULL path = memory only). */
void hints_seen_add(hints_seen_t *seen, const char *path, const char *id);

/** Forget everything shown, in memory and in @p path. */
void hints_seen_clear(hints_seen_t *seen, const char *path);

/**
 * Index into @p table of the tip to show, or -1 if @p platform has none at all.
 *
 * Order: on a co-op stream the unseen co-op tips first, then on an HDR stream the unseen HDR tips, then the unseen
 * general tips, each by rank. When none of those is left the history is cleared (memory and @p path) and the
 * choice is made again, so the most important tip comes round first. The chosen tip is recorded as seen.
 */
int hints_choose(const hint_t *table, size_t count, int platform, bool coop, bool hdr,
                 hints_seen_t *seen, const char *path);

/**
 * The settings bar's next tip: the unseen tip with the lowest settings_rank, whatever its stream type. Same history
 * and same restart-from-the-top rule as hints_choose (they share @p seen). Index into @p table, or -1.
 */
int hints_choose_settings(const hint_t *table, size_t count, int platform, hints_seen_t *seen, const char *path);

/**
 * The tips of @p when that @p platform shows, most important first: their indices into @p table go into @p out
 * (at most @p max). Returns how many. For the settings screen's "All tips" list; marks nothing seen.
 */
size_t hints_ordered(const hint_t *table, size_t count, int platform, hint_when_t when, int *out, size_t max);

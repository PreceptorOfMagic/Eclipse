#include "hints_core.h"

#include <stdio.h>
#include <string.h>

void hints_seen_load(hints_seen_t *seen, const char *path) {
    seen->count = 0;
    if (path == NULL) {
        return;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    char line[HINT_ID_MAX + 8];
    while (fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || strlen(line) >= HINT_ID_MAX || hints_seen_has(seen, line)) {
            continue;
        }
        if (seen->count >= HINTS_SEEN_MAX) {
            break;
        }
        strcpy(seen->ids[seen->count++], line);
    }
    fclose(f);
}

bool hints_seen_has(const hints_seen_t *seen, const char *id) {
    for (int i = 0; i < seen->count; i++) {
        if (strcmp(seen->ids[i], id) == 0) {
            return true;
        }
    }
    return false;
}

void hints_seen_add(hints_seen_t *seen, const char *path, const char *id) {
    if (strlen(id) >= HINT_ID_MAX || hints_seen_has(seen, id) || seen->count >= HINTS_SEEN_MAX) {
        return;
    }
    strcpy(seen->ids[seen->count++], id);
    if (path == NULL) {
        return;
    }
    FILE *f = fopen(path, "a");
    if (f == NULL) {
        return;   /* read-only conf dir: the order still holds for this run */
    }
    fprintf(f, "%s\n", id);
    fclose(f);
}

void hints_seen_clear(hints_seen_t *seen, const char *path) {
    seen->count = 0;
    if (path == NULL) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (f != NULL) {
        fclose(f);
    }
}

/* Best-ranked unseen tip of @p when for @p platform, or -1. */
static int best_unseen(const hint_t *table, size_t count, int platform, hint_when_t when, const hints_seen_t *seen) {
    int best = -1;
    for (size_t i = 0; i < count; i++) {
        const hint_t *h = &table[i];
        const unsigned char rank = h->rank[platform];
        if (h->when != when || rank == 0 || hints_seen_has(seen, h->id)) {
            continue;
        }
        if (best < 0 || rank < table[best].rank[platform]) {
            best = (int) i;
        }
    }
    return best;
}

static int pick(const hint_t *table, size_t count, int platform, bool coop, bool hdr, const hints_seen_t *seen) {
    int i = -1;
    if (coop) {
        i = best_unseen(table, count, platform, HINT_COOP, seen);
    }
    if (i < 0 && hdr) {
        i = best_unseen(table, count, platform, HINT_HDR, seen);
    }
    if (i < 0) {
        i = best_unseen(table, count, platform, HINT_ANY, seen);
    }
    return i;
}

static int best_unseen_settings(const hint_t *table, size_t count, int platform, const hints_seen_t *seen) {
    int best = -1;
    for (size_t i = 0; i < count; i++) {
        const unsigned char rank = table[i].settings_rank[platform];
        if (rank == 0 || hints_seen_has(seen, table[i].id)) {
            continue;
        }
        if (best < 0 || rank < table[best].settings_rank[platform]) {
            best = (int) i;
        }
    }
    return best;
}

int hints_choose_settings(const hint_t *table, size_t count, int platform, hints_seen_t *seen, const char *path) {
    if (platform < 0 || platform >= HINT_PLATFORM_COUNT) {
        return -1;
    }
    int i = best_unseen_settings(table, count, platform, seen);
    if (i < 0) {
        hints_seen_clear(seen, path);
        i = best_unseen_settings(table, count, platform, seen);
    }
    if (i >= 0) {
        hints_seen_add(seen, path, table[i].id);
    }
    return i;
}

size_t hints_ordered(const hint_t *table, size_t count, int platform, hint_when_t when, int *out, size_t max) {
    if (platform < 0 || platform >= HINT_PLATFORM_COUNT) {
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < count && n < max; i++) {
        const hint_t *h = &table[i];
        if (h->when != when || h->rank[platform] == 0) {
            continue;
        }
        /* insertion sort by rank: the table is ~100 entries */
        size_t j = n++;
        while (j > 0 && table[out[j - 1]].rank[platform] > h->rank[platform]) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = (int) i;
    }
    return n;
}

int hints_choose(const hint_t *table, size_t count, int platform, bool coop, bool hdr,
                 hints_seen_t *seen, const char *path) {
    if (platform < 0 || platform >= HINT_PLATFORM_COUNT) {
        return -1;
    }
    int i = pick(table, count, platform, coop, hdr, seen);
    if (i < 0) {
        /* Everything this stream could show has been shown: start the order again. */
        hints_seen_clear(seen, path);
        i = pick(table, count, platform, coop, hdr, seen);
    }
    if (i >= 0) {
        hints_seen_add(seen, path, table[i].id);
    }
    return i;
}

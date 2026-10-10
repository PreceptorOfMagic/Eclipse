#include "coop_assign.h"

#include <string.h>

/** Bounded copy that always terminates. snprintf would drag in stdio for nothing. */
static void copy_str(char *dst, size_t cap, const char *src) {
    if (cap == 0) {
        return;
    }
    size_t i = 0;
    for (; src != NULL && src[i] != '\0' && i + 1 < cap; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/** Append " <n>" for a small positive n. Silent no-op if it will not fit, which
 *  keeps the label valid rather than truncating it into another pad's name. */
static void append_ordinal(char *dst, size_t cap, unsigned n) {
    char digits[8];
    size_t d = 0;
    if (n == 0) {
        return;
    }
    while (n > 0 && d < sizeof(digits)) {
        digits[d++] = (char) ('0' + (n % 10u));
        n /= 10u;
    }
    size_t len = strlen(dst);
    if (len + 1 + d + 1 > cap) {
        return;
    }
    dst[len++] = ' ';
    while (d > 0) {
        dst[len++] = digits[--d];
    }
    dst[len] = '\0';
}

/**
 * Label every pad: the FIRST of a name keeps it bare, and each later one sharing
 * that name becomes " 2", " 3" ... (user's rule, 2026-08-05).
 *
 * Numbering from 2 rather than 1 is what makes a label STABLE: it depends only on
 * what came before, so plugging in a second "Xbox Wireless Controller" cannot
 * rename the first one out from under a setting that already refers to it. That
 * also means one pass -- counting only earlier pads -- instead of the two passes a
 * "number every collision from 1" rule would need.
 */
static void label_pads(const coop_assign_pad_t *pads, size_t count,
                       coop_assign_result_t *out) {
    for (size_t i = 0; i < count; ++i) {
        copy_str(out[i].label, sizeof(out[i].label), pads[i].name);
        unsigned seen_before = 0;
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(pads[i].name, pads[j].name) == 0) {
                seen_before++;
            }
        }
        if (seen_before > 0) {
            append_ordinal(out[i].label, sizeof(out[i].label), seen_before + 1u);
        }
    }
}

size_t coop_assign_label(const coop_assign_pad_t *pads, size_t count,
                         coop_assign_result_t *out) {
    if (pads == NULL || out == NULL) {
        return 0;
    }
    if (count > COOP_ASSIGN_MAX_PADS) {
        count = COOP_ASSIGN_MAX_PADS;
    }
    label_pads(pads, count, out);
    for (size_t i = 0; i < count; ++i) {
        out[i].route = COOP_ASSIGN_PRIMARY;
    }
    return count;
}

/** A route expressed as load: BOTH really does occupy a slot on each host. */
static void add_load(int route, int *load, int host_count) {
    if (route == COOP_ASSIGN_BOTH) {
        for (int h = 0; h < host_count; ++h) {
            load[h]++;
        }
    } else if (route >= 0 && route < host_count) {
        load[route]++;
    }
}

static bool route_is_valid(int route, int host_count) {
    if (route == COOP_ASSIGN_BOTH) {
        return host_count > 1;            /* BOTH is meaningless with one host */
    }
    return route >= 0 && route < host_count;
}

size_t coop_assign_compute(const coop_assign_pad_t *pads, size_t count,
                           int host_count, coop_assign_result_t *out) {
    if (pads == NULL || out == NULL) {
        return 0;
    }
    if (count > COOP_ASSIGN_MAX_PADS) {
        count = COOP_ASSIGN_MAX_PADS;
    }
    if (host_count < 1) {
        host_count = 1;
    }
    if (host_count > COOP_ASSIGN_MAX_PADS) {
        host_count = COOP_ASSIGN_MAX_PADS;
    }

    label_pads(pads, count, out);

    /* NOTHING TO SPREAD ACROSS. One host means the old behaviour verbatim: the
     * settings value, or PRIMARY when it is unset. Distribution must not change
     * what an ordinary single-host stream has always done. */
    if (host_count < 2) {
        for (size_t i = 0; i < count; ++i) {
            const int d = pads[i].settings_default;
            out[i].route = route_is_valid(d, host_count) ? d : COOP_ASSIGN_PRIMARY;
        }
        return count;
    }

    int load[COOP_ASSIGN_MAX_PADS];
    memset(load, 0, sizeof(load));
    bool placed[COOP_ASSIGN_MAX_PADS];
    memset(placed, 0, sizeof(placed));

    /* Pass 1 -- the immovable and the explicitly chosen, in that order. Both are
     * placed BEFORE anything is spread so that their load is visible to the
     * spread; that is the whole reason the totals come out even. */
    for (size_t i = 0; i < count; ++i) {
        if (pads[i].source == COOP_ASSIGN_SRC_HOST &&
            route_is_valid(pads[i].pinned_host, host_count)) {
            out[i].route = pads[i].pinned_host;
            placed[i] = true;
            add_load(out[i].route, load, host_count);
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (!placed[i] && route_is_valid(pads[i].choice, host_count)) {
            out[i].route = pads[i].choice;
            placed[i] = true;
            add_load(out[i].route, load, host_count);
        }
    }

    /* Pass 2 -- deal the rest to the least-loaded host, ties to the lower index.
     * Deterministic: same pads in the same order give the same map every time. */
    for (size_t i = 0; i < count; ++i) {
        if (placed[i]) {
            continue;
        }
        int best = 0;
        for (int h = 1; h < host_count; ++h) {
            if (load[h] < load[best]) {
                best = h;
            }
        }
        out[i].route = best;
        placed[i] = true;
        add_load(best, load, host_count);
    }
    return count;
}

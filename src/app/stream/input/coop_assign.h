/*
 * Controller assignment for a co-op stream: who drives which host.
 *
 * WHY THIS IS ITS OWN MODULE. The rule the user wants is a pure function --
 * controllers in, host map out -- and it has to run in two places eventually: here
 * in Eclipse, and inside the Umbra host executable once the host-side pieces are compiled
 * in (FORK-PREP defers that compile to last). So this file deliberately depends on
 * NOTHING: no app_settings, no SDL, no platform headers, C99 only, extern "C" for
 * Apollo's C++ build. Everything it needs is passed in. That is what lets the same
 * source drop into the fork unchanged instead of being rewritten there.
 *
 * WHAT REPLACED WHAT. The old model was app_configuration.coop_pad_pane[4]: four
 * fixed slots, each statically PRIMARY/SECONDARY/BOTH, with no idea which pads are
 * actually plugged in and no distribution at all. This adds the two things that
 * were missing -- it only considers controllers REALLY CONNECTED at stream start,
 * and it spreads them across the hosts taking part.
 *
 * PRECEDENCE, highest first (user's rule, 2026-08-05):
 *   1. PINNED    -- a pad physically plugged into one host cannot be moved, so it
 *                   is placed there and simply counts as load on that host.
 *   2. CHOICE    -- an explicit per-controller pick is always honoured.
 *   3. SPREAD    -- everything left is dealt out evenly.
 *   4. DEFAULT   -- with fewer than two hosts there is nothing to spread across,
 *                   so the existing settings value is used and today's single-host
 *                   behaviour is unchanged.
 *
 * "EVENLY" MEANS EVEN TOTALS, so each remaining pad goes to the host with the
 * fewest so far rather than blind alternation. Blind round-robin ignores steps 1
 * and 2, so one pinned pad and one chosen pad on the same host leaves that host
 * with twice the pads while the count still "alternated". Ties break toward the
 * lower host index, which makes the whole result deterministic: the same
 * controllers in the same order always produce the same map, so reconnecting one
 * pad cannot reshuffle everyone else's.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COOP_ASSIGN_MAX_PADS 16
#define COOP_ASSIGN_NAME_MAX 64
/** Room for the raw name plus the " 12" a collision can add, and the terminator. */
#define COOP_ASSIGN_LABEL_MAX (COOP_ASSIGN_NAME_MAX + 8)

/** Route values MUST match app_settings.h COOP_PAD_* -- existing conf files carry
 *  these numbers, and this module is what now writes them. */
enum {
    COOP_ASSIGN_PRIMARY = 0,
    COOP_ASSIGN_SECONDARY = 1,
    COOP_ASSIGN_BOTH = 2,
};

/** No explicit pick / no pin. Distinct from every valid route value. */
#define COOP_ASSIGN_UNSET (-1)

/** Where a controller physically lives, which decides how it is actuated once
 *  assigned -- TV pads route over coop_input, Pi pads are forwarded by the hub,
 *  and a pad already plugged into a host is simply left alone. */
typedef enum {
    COOP_ASSIGN_SRC_TV = 0,
    COOP_ASSIGN_SRC_PI = 1,
    COOP_ASSIGN_SRC_HOST = 2,
} coop_assign_src_t;

typedef struct {
    /** Device name as reported, e.g. "Xbox Wireless Controller". Not unique. */
    char name[COOP_ASSIGN_NAME_MAX];
    coop_assign_src_t source;
    /** Host index this pad is physically bound to, or COOP_ASSIGN_UNSET if free.
     *  Only meaningful for COOP_ASSIGN_SRC_HOST. */
    int pinned_host;
    /** Explicit per-controller pick, or COOP_ASSIGN_UNSET to let it be spread. */
    int choice;
    /** Existing settings value used when there is nothing to spread across. */
    int settings_default;
} coop_assign_pad_t;

typedef struct {
    /** Display name. The FIRST controller of a given name keeps that name as-is;
     *  each later one sharing it becomes " 2", " 3" ... So a label depends only on
     *  what came before it and never changes because another pad appeared later --
     *  a setting that names a controller keeps pointing at the same one. This is
     *  what both the settings list and the picker show. */
    char label[COOP_ASSIGN_LABEL_MAX];
    /** COOP_ASSIGN_PRIMARY / _SECONDARY / _BOTH. */
    int route;
} coop_assign_result_t;

/**
 * Work out the label and route for every connected controller.
 *
 * @param pads       controllers ACTUALLY CONNECTED right now, in a stable order.
 * @param count      how many, clamped to COOP_ASSIGN_MAX_PADS.
 * @param host_count how many hosts take part (1 = ordinary single-host stream).
 * @param out        filled with @p count results; must hold at least @p count.
 * @return number of entries written.
 */
size_t coop_assign_compute(const coop_assign_pad_t *pads, size_t count,
                           int host_count, coop_assign_result_t *out);

/**
 * De-duplicate display names on their own, for a settings screen that lists
 * controllers without assigning them. Same rule as coop_assign_compute().
 */
size_t coop_assign_label(const coop_assign_pad_t *pads, size_t count,
                         coop_assign_result_t *out);

#ifdef __cplusplus
}
#endif

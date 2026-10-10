/*
 * The co-op controller assignment rule, checked against the behaviour the user
 * specified on 2026-08-05: real controller names (numbered only where they
 * collide), explicit picks honoured first, and everything else spread evenly
 * across the hosts taking part.
 *
 * Standalone like the rest of tests/coop -- no fixtures, no host, no hardware:
 *   gcc -std=c99 -Wall -Wextra -Werror -o /tmp/test_coop_assign \
 *       tests/coop/test_coop_assign.c src/app/stream/input/coop_assign.c && /tmp/test_coop_assign
 *
 * NOTE ON WHAT IS ASSERTED. Where a split cannot come out level -- 3 pads over 2
 * hosts -- the test asserts the INVARIANT (totals within one) rather than one
 * particular arrangement. Which host wins a tie is the documented lower-index
 * rule, an implementation detail, and pinning it here would make the test fail
 * for a change that is still correct.
 */
#include "../../src/app/stream/input/coop_assign.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
static void ck(const char *what, int ok) {
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

static coop_assign_pad_t mk(const char *n, coop_assign_src_t s, int pin, int choice, int def) {
    coop_assign_pad_t p;
    memset(&p, 0, sizeof(p));
    strncpy(p.name, n, COOP_ASSIGN_NAME_MAX - 1);
    p.source = s; p.pinned_host = pin; p.choice = choice; p.settings_default = def;
    return p;
}

int main(void) {
    coop_assign_result_t r[COOP_ASSIGN_MAX_PADS];

    printf("1. explicit choice honoured, remainder even (the agreed rule)\n");
    {
        coop_assign_pad_t p[4] = {
            mk("Pad A", COOP_ASSIGN_SRC_TV, -1, COOP_ASSIGN_UNSET, COOP_ASSIGN_UNSET),
            mk("Pad B", COOP_ASSIGN_SRC_TV, -1, COOP_ASSIGN_UNSET, COOP_ASSIGN_UNSET),
            mk("Pad C", COOP_ASSIGN_SRC_TV, -1, COOP_ASSIGN_SECONDARY, COOP_ASSIGN_UNSET),
            mk("Pad D", COOP_ASSIGN_SRC_TV, -1, COOP_ASSIGN_UNSET, COOP_ASSIGN_UNSET),
        };
        coop_assign_compute(p, 4, 2, r);
        for (int i = 0; i < 4; i++) printf("     %s -> host %d\n", r[i].label, r[i].route);
        ck("chosen pad kept its pick", r[2].route == COOP_ASSIGN_SECONDARY);
        int h0 = 0, h1 = 0;
        for (int i = 0; i < 4; i++) { if (r[i].route == 0) h0++; else h1++; }
        ck("totals even 2/2", h0 == 2 && h1 == 2);
    }

    printf("2. names: first keeps its name, the next is 2\n");
    {
        coop_assign_pad_t p[4] = {
            mk("Xbox Wireless Controller", COOP_ASSIGN_SRC_TV, -1, -1, -1),
            mk("Steam Controller", COOP_ASSIGN_SRC_PI, -1, -1, -1),
            mk("Xbox Wireless Controller", COOP_ASSIGN_SRC_PI, -1, -1, -1),
            mk("Xbox Wireless Controller", COOP_ASSIGN_SRC_TV, -1, -1, -1),
        };
        coop_assign_label(p, 4, r);
        for (int i = 0; i < 4; i++) printf("     [%s]\n", r[i].label);
        ck("first keeps its bare name", strcmp(r[0].label, "Xbox Wireless Controller") == 0);
        ck("second becomes 2", strcmp(r[2].label, "Xbox Wireless Controller 2") == 0);
        ck("third becomes 3", strcmp(r[3].label, "Xbox Wireless Controller 3") == 0);
        ck("a different name is untouched", strcmp(r[1].label, "Steam Controller") == 0);

        /* THE POINT OF NUMBERING FROM 2: a label must depend only on what came
         * before it, so plugging in a second pad of the same name cannot rename
         * the first one out from under a setting that already refers to it. */
        coop_assign_result_t alone[1];
        coop_assign_label(p, 1, alone);
        ck("first label identical when it is the only one",
           strcmp(alone[0].label, r[0].label) == 0);
    }

    printf("3. single host keeps today's behaviour (settings default)\n");
    {
        coop_assign_pad_t p[2] = {
            mk("Pad A", COOP_ASSIGN_SRC_TV, -1, COOP_ASSIGN_UNSET, COOP_ASSIGN_PRIMARY),
            mk("Pad B", COOP_ASSIGN_SRC_TV, -1, COOP_ASSIGN_UNSET, COOP_ASSIGN_UNSET),
        };
        coop_assign_compute(p, 2, 1, r);
        ck("default honoured", r[0].route == COOP_ASSIGN_PRIMARY);
        ck("unset falls back to primary", r[1].route == COOP_ASSIGN_PRIMARY);
    }

    printf("4. a pad plugged into a host cannot be moved, and counts as load\n");
    {
        coop_assign_pad_t p[3] = {
            mk("Puck", COOP_ASSIGN_SRC_HOST, 1, COOP_ASSIGN_UNSET, COOP_ASSIGN_UNSET),
            mk("Pad A", COOP_ASSIGN_SRC_TV, -1, COOP_ASSIGN_UNSET, COOP_ASSIGN_UNSET),
            mk("Pad B", COOP_ASSIGN_SRC_TV, -1, COOP_ASSIGN_UNSET, COOP_ASSIGN_UNSET),
        };
        coop_assign_compute(p, 3, 2, r);
        for (int i = 0; i < 3; i++) printf("     %s -> host %d\n", r[i].label, r[i].route);
        ck("pinned pad stayed on its host", r[0].route == 1);
        /* 3 pads over 2 hosts can only ever be 2/1. Assert the INVARIANT (totals
         * within one) rather than one particular arrangement -- which host wins a
         * tie is the documented lower-index rule, not a requirement. */
        int h0 = 0, h1 = 0;
        for (int i = 0; i < 3; i++) { if (r[i].route == 0) h0++; else h1++; }
        printf("     split %d/%d\n", h0, h1);
        ck("totals within one of each other", (h0 - h1 == 1) || (h1 - h0 == 1));
        ck("host 0 got a share despite the pin", h0 >= 1);
    }

    printf("5. BOTH loads every host, and is refused when there is only one\n");
    {
        coop_assign_pad_t p[3] = {
            mk("Pad A", COOP_ASSIGN_SRC_TV, -1, COOP_ASSIGN_BOTH, COOP_ASSIGN_UNSET),
            mk("Pad B", COOP_ASSIGN_SRC_TV, -1, COOP_ASSIGN_UNSET, COOP_ASSIGN_UNSET),
            mk("Pad C", COOP_ASSIGN_SRC_TV, -1, COOP_ASSIGN_UNSET, COOP_ASSIGN_UNSET),
        };
        coop_assign_compute(p, 3, 2, r);
        ck("BOTH kept", r[0].route == COOP_ASSIGN_BOTH);
        ck("remainder still even", r[1].route != r[2].route);
        coop_assign_pad_t q[1] = { mk("Pad A", COOP_ASSIGN_SRC_TV, -1, COOP_ASSIGN_BOTH, COOP_ASSIGN_UNSET) };
        coop_assign_compute(q, 1, 1, r);
        ck("BOTH rejected on a single host", r[0].route == COOP_ASSIGN_PRIMARY);
    }

    printf("6. deterministic: same input, same map\n");
    {
        coop_assign_pad_t p[5] = {
            mk("A", COOP_ASSIGN_SRC_TV, -1, -1, -1), mk("B", COOP_ASSIGN_SRC_TV, -1, -1, -1),
            mk("C", COOP_ASSIGN_SRC_TV, -1, -1, -1), mk("D", COOP_ASSIGN_SRC_TV, -1, -1, -1),
            mk("E", COOP_ASSIGN_SRC_TV, -1, -1, -1),
        };
        coop_assign_result_t a[5], b[5];
        coop_assign_compute(p, 5, 2, a);
        coop_assign_compute(p, 5, 2, b);
        int same = 1;
        for (int i = 0; i < 5; i++) if (a[i].route != b[i].route) same = 0;
        ck("stable across runs", same);
        int h0 = 0, h1 = 0;
        for (int i = 0; i < 5; i++) { if (a[i].route == 0) h0++; else h1++; }
        printf("     odd count split %d/%d\n", h0, h1);
        ck("odd count differs by at most one", (h0 - h1 == 1) || (h1 - h0 == 1));
    }

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails != 0;
}

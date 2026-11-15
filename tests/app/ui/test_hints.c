/* Streaming tips order (hints_core.c): most important unseen first, remembered across runs, context tips first,
 * restart from the top once everything has been shown. */
#include "unity.h"
#include "ui/streaming/hints_core.h"

#include <stdio.h>
#include <string.h>

#define W HINT_PLATFORM_WEBOS
#define D HINT_PLATFORM_WINDOWS

/* rank columns: webOS, Windows, Linux, Android */
/* rank = stream order (per stream type), settings = the settings bar's single order */
static const hint_t table[] = {
        {"third", "Third", HINT_ANY, {3, 2, 0, 0}, {4, 2, 0, 0}},
        {"first", "First", HINT_ANY, {1, 0, 0, 0}, {1, 0, 0, 0}},
        {"second", "Second", HINT_ANY, {2, 1, 0, 0}, {2, 1, 0, 0}},
        {"coop_b", "Coop B", HINT_COOP, {2, 2, 0, 0}, {6, 4, 0, 0}},
        {"coop_a", "Coop A", HINT_COOP, {1, 1, 0, 0}, {3, 3, 0, 0}},
        {"hdr_a", "HDR A", HINT_HDR, {1, 0, 0, 0}, {5, 0, 0, 0}},
};
#define N (sizeof(table) / sizeof(table[0]))

static char path[256];
static hints_seen_t seen;

void setUp(void) {
    snprintf(path, sizeof(path), "test-hints-seen-%d.txt", (int) sizeof(table));
    remove(path);
    hints_seen_load(&seen, path);
}

void tearDown(void) {
    remove(path);
}

static const char *next(int platform, bool coop, bool hdr) {
    const int i = hints_choose(table, N, platform, coop, hdr, &seen, path);
    return i < 0 ? "(none)" : table[i].id;
}

static void test_rank_order_then_restart(void) {
    TEST_ASSERT_EQUAL_STRING("first", next(W, false, false));
    TEST_ASSERT_EQUAL_STRING("second", next(W, false, false));
    TEST_ASSERT_EQUAL_STRING("third", next(W, false, false));
    TEST_ASSERT_EQUAL_STRING("first", next(W, false, false));   /* all shown: from the top again */
}

static void test_platform_filter(void) {
    TEST_ASSERT_EQUAL_STRING("second", next(D, false, false));   /* "first" is webOS only */
    TEST_ASSERT_EQUAL_STRING("third", next(D, false, false));
    TEST_ASSERT_EQUAL_STRING("second", next(D, false, false));
    TEST_ASSERT_EQUAL_STRING("(none)", next(HINT_PLATFORM_LINUX, false, false));
}

static void test_context_tips_first_and_only_in_context(void) {
    TEST_ASSERT_EQUAL_STRING("coop_a", next(W, true, false));
    TEST_ASSERT_EQUAL_STRING("first", next(W, false, false));   /* not a co-op stream: no co-op tips */
    TEST_ASSERT_EQUAL_STRING("coop_b", next(W, true, false));
    TEST_ASSERT_EQUAL_STRING("second", next(W, true, false));   /* co-op tips used up: general ones */
    TEST_ASSERT_EQUAL_STRING("hdr_a", next(W, false, true));
}

static void test_remembered_across_runs(void) {
    TEST_ASSERT_EQUAL_STRING("first", next(W, false, false));
    TEST_ASSERT_EQUAL_STRING("second", next(W, false, false));
    hints_seen_t reloaded;
    hints_seen_load(&reloaded, path);
    TEST_ASSERT_EQUAL_INT(2, reloaded.count);
    const int i = hints_choose(table, N, W, false, false, &reloaded, path);
    TEST_ASSERT_EQUAL_STRING("third", table[i].id);
}

static void test_unknown_ids_in_file_are_harmless(void) {
    /* A tip dropped in a later release stays in the file and is simply never matched. */
    FILE *f = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("removed_tip\r\nfirst\n\n", f);
    fclose(f);
    hints_seen_load(&seen, path);
    TEST_ASSERT_EQUAL_INT(2, seen.count);
    TEST_ASSERT_EQUAL_STRING("second", next(W, false, false));
}

static void test_no_file_path_still_orders(void) {
    TEST_ASSERT_EQUAL_INT(1, table[hints_choose(table, N, W, false, false, &seen, NULL)].rank[W]);
    TEST_ASSERT_EQUAL_INT(2, table[hints_choose(table, N, W, false, false, &seen, NULL)].rank[W]);
}

static void test_ordered_list(void) {
    int out[8];
    size_t n = hints_ordered(table, N, W, HINT_ANY, out, 8);
    TEST_ASSERT_EQUAL_INT(3, (int) n);
    TEST_ASSERT_EQUAL_STRING("first", table[out[0]].id);
    TEST_ASSERT_EQUAL_STRING("second", table[out[1]].id);
    TEST_ASSERT_EQUAL_STRING("third", table[out[2]].id);
    n = hints_ordered(table, N, D, HINT_COOP, out, 8);
    TEST_ASSERT_EQUAL_INT(2, (int) n);
    TEST_ASSERT_EQUAL_STRING("coop_a", table[out[0]].id);
    TEST_ASSERT_EQUAL_INT(0, (int) hints_ordered(table, N, D, HINT_HDR, out, 8));   /* webOS-only HDR tip */
    TEST_ASSERT_EQUAL_INT(1, (int) hints_ordered(table, N, W, HINT_ANY, out, 1));   /* respects max */
    /* listing does not mark anything seen */
    TEST_ASSERT_EQUAL_STRING("first", next(W, false, false));
}

static const char *next_settings(int platform) {
    const int i = hints_choose_settings(table, N, platform, &seen, path);
    return i < 0 ? "(none)" : table[i].id;
}

static void test_settings_shows_every_type_in_its_order(void) {
    TEST_ASSERT_EQUAL_STRING("first", next_settings(W));
    TEST_ASSERT_EQUAL_STRING("second", next_settings(W));
    TEST_ASSERT_EQUAL_STRING("coop_a", next_settings(W));   /* co-op tip, outside a co-op stream */
    TEST_ASSERT_EQUAL_STRING("third", next_settings(W));
    TEST_ASSERT_EQUAL_STRING("hdr_a", next_settings(W));
    TEST_ASSERT_EQUAL_STRING("coop_b", next_settings(W));
    TEST_ASSERT_EQUAL_STRING("first", next_settings(W));    /* all shown: from the top again */
}

static void test_settings_and_stream_share_history(void) {
    TEST_ASSERT_EQUAL_STRING("first", next(W, false, false));      /* a stream showed it */
    TEST_ASSERT_EQUAL_STRING("second", next_settings(W));          /* so settings moves on */
    TEST_ASSERT_EQUAL_STRING("coop_a", next(W, true, false));      /* a co-op stream shows its first tip */
    TEST_ASSERT_EQUAL_STRING("third", next_settings(W));           /* settings skips it: coop_a was rank 3 there */
    TEST_ASSERT_EQUAL_STRING("coop_b", next(W, true, false));      /* the co-op stream continues its own order */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rank_order_then_restart);
    RUN_TEST(test_platform_filter);
    RUN_TEST(test_context_tips_first_and_only_in_context);
    RUN_TEST(test_remembered_across_runs);
    RUN_TEST(test_unknown_ids_in_file_are_harmless);
    RUN_TEST(test_no_file_path_still_orders);
    RUN_TEST(test_ordered_list);
    RUN_TEST(test_settings_shows_every_type_in_its_order);
    RUN_TEST(test_settings_and_stream_share_history);
    return UNITY_END();
}

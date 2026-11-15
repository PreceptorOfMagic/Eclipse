/* backend/update_manifest.c and util/json_mini.c: which release the update notice shows, and when. */
#include "unity.h"

#include <string.h>

#include "backend/update_manifest.h"
#include "util/json_mini.h"

static const char *MANIFEST =
        "{\"schema\": 1,\n"
        " \"eclipse\": {\n"
        "   \"download_url\": \"https://example.invalid/latest\",\n"
        "   \"how_to_update\": {\"windows\": \"Run the installer.\", \"webos\": \"Install the .ipk.\"},\n"
        "   \"releases\": [\n"
        "     {\"version\": \"1.3.7\", \"date\": \"2026-11-20\", \"highlights\": [\"Update notice\", \"Signed \\u00e9\"]},\n"
        "     {\"version\": \"1.3.8\", \"notify\": false, \"highlights\": [\"Quiet fix\"]},\n"
        "     {\"version\": \"1.3.9\", \"platforms\": [\"windows\"], \"highlights\": [\"Windows only\"]},\n"
        "     {\"version\": \"1.3.6\", \"highlights\": [\"Old\"]},\n"
        "     {\"version\": \"1.4.0-beta.1\", \"notify\": false}\n"
        "   ]},\n"
        " \"umbra\": {\"releases\": []},\n"
        " \"future_key\": [1, 2.5e3, null, true]}\n";

static update_info_t info;

void setUp(void) {
    memset(&info, 0, sizeof(info));
}

void tearDown(void) {
    update_info_clear(&info);
}

static void eval(const char *json, const char *platform, const char *installed) {
    update_info_clear(&info);
    TEST_ASSERT_TRUE(update_manifest_evaluate(json, strlen(json), "eclipse", platform, installed, &info));
}

void test_version_compare(void) {
    TEST_ASSERT_TRUE(update_version_compare("1.10.0", "1.9.2") > 0);
    TEST_ASSERT_TRUE(update_version_compare("1.3.6", "1.3.6") == 0);
    TEST_ASSERT_TRUE(update_version_compare("1.3", "1.3.0") == 0);
    TEST_ASSERT_TRUE(update_version_compare("v1.3.7", "1.3.6") > 0);
    TEST_ASSERT_TRUE(update_version_compare("1.4.0-beta.1", "1.4.0") < 0);
    TEST_ASSERT_TRUE(update_version_compare("1.4.0-beta.1", "1.3.9") > 0);
    TEST_ASSERT_TRUE(update_version_compare("1.4.0-beta.2", "1.4.0-beta.1") > 0);
}

/* Linux: 1.3.9 is Windows-only and 1.3.8 is silent, so 1.3.7 is what notifies. */
void test_newest_notifying_release_for_platform(void) {
    eval(MANIFEST, "linux", "1.3.6");
    TEST_ASSERT_TRUE(info.available);
    TEST_ASSERT_EQUAL_STRING("1.3.7", info.latest_version);
    TEST_ASSERT_EQUAL_UINT(1, info.releases_count);
    TEST_ASSERT_EQUAL_STRING("Signed \xc3\xa9", info.releases[0].highlights[1]);
    TEST_ASSERT_NULL(info.how_to_update);
    TEST_ASSERT_EQUAL_STRING("https://example.invalid/latest", info.download_url);
}

/* Windows: 1.3.9 notifies, and the silent 1.3.8 between is listed with it, newest first. */
void test_silent_release_listed_under_a_later_one(void) {
    eval(MANIFEST, "windows", "1.3.6");
    TEST_ASSERT_EQUAL_STRING("1.3.9", info.latest_version);
    TEST_ASSERT_EQUAL_UINT(3, info.releases_count);
    TEST_ASSERT_EQUAL_STRING("1.3.9", info.releases[0].version);
    TEST_ASSERT_EQUAL_STRING("1.3.8", info.releases[1].version);
    TEST_ASSERT_EQUAL_STRING("1.3.7", info.releases[2].version);
    TEST_ASSERT_EQUAL_STRING("Run the installer.", info.how_to_update);
}

/* Only a silent release above the installed one: nothing to show. */
void test_silent_release_alone_does_not_notify(void) {
    eval(MANIFEST, "linux", "1.3.7");
    TEST_ASSERT_FALSE(info.available);
    TEST_ASSERT_NULL(info.latest_version);
}

void test_up_to_date_and_newer_than_manifest(void) {
    eval(MANIFEST, "windows", "1.3.9");
    TEST_ASSERT_FALSE(info.available);
    eval(MANIFEST, "windows", "2.0.0");
    TEST_ASSERT_FALSE(info.available);
}

void test_unusable_documents_are_refused(void) {
    const char *bad[] = {
            "",
            "{",
            "{\"schema\": 2, \"eclipse\": {\"releases\": []}}",
            "{\"eclipse\": {\"releases\": []}}",
            "{\"schema\": 1}",
            "{\"schema\": 1, \"eclipse\": {\"releases\": {}}}",
            "{\"schema\": 1, \"eclipse\": {\"releases\": []}} trailing",
            "[1, 2]",
            "{\"schema\": 1, \"eclipse\": {\"releases\": [\"unterminated]}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        update_info_t out;
        TEST_ASSERT_FALSE_MESSAGE(update_manifest_evaluate(bad[i], strlen(bad[i]), "eclipse", "linux", "1.0.0",
                                                           &out), bad[i]);
        TEST_ASSERT_FALSE(out.available);
    }
}

void test_json_mini_edges(void) {
    const char *deep = "[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]";
    TEST_ASSERT_NULL(json_mini_parse(deep, strlen(deep)));
    const char *pair = "\"\\ud83d\\ude00\"";
    json_value_t *v = json_mini_parse(pair, strlen(pair));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_STRING("\xf0\x9f\x98\x80", v->string);
    json_mini_free(v);
    const char *lone = "\"\\ude00\"";
    TEST_ASSERT_NULL(json_mini_parse(lone, strlen(lone)));
    const char *ctrl = "\"a\nb\"";
    TEST_ASSERT_NULL(json_mini_parse(ctrl, strlen(ctrl)));
    const char *num = "-0.5e2";
    v = json_mini_parse(num, strlen(num));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(v->number == -50.0);
    json_mini_free(v);
    TEST_ASSERT_NULL(json_mini_parse("01", 2));
    TEST_ASSERT_NULL(json_mini_parse("tru", 3));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_version_compare);
    RUN_TEST(test_newest_notifying_release_for_platform);
    RUN_TEST(test_silent_release_listed_under_a_later_one);
    RUN_TEST(test_silent_release_alone_does_not_notify);
    RUN_TEST(test_up_to_date_and_newer_than_manifest);
    RUN_TEST(test_unusable_documents_are_refused);
    RUN_TEST(test_json_mini_edges);
    return UNITY_END();
}

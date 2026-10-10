#include "unity.h"
#include "backend/pcmanager/known_hosts.h"
#include "hostport.h"
#include "util/path.h"

#include <stdio.h>
#include <stdlib.h>

void setUp(void) {

}

void tearDown(void) {

}

void test_parse() {
    known_host_t *hosts = known_hosts_parse(FIXTURES_PATH_PREFIX "hosts_read.ini");
    TEST_ASSERT_EQUAL_STRING("192.168.1.100", hostport_get_hostname(hosts->address));
    TEST_ASSERT_EQUAL(0, hostport_get_port(hosts->address));

    known_host_t *next = hosts->next;
    TEST_ASSERT_EQUAL_STRING("192.168.1.101", hostport_get_hostname(next->address));
    TEST_ASSERT_EQUAL(47985, hostport_get_port(next->address));

    known_hosts_free(hosts, known_hosts_node_free);
}

void test_parse_utf8_path() {
    const char *directory = "known-hosts-\xE9\x9B\xAA-\xF0\x9F\x8C\x99";
    TEST_ASSERT_EQUAL_INT(0, path_dir_ensure(directory));
    char *path = path_join(directory, "hosts.ini");
    TEST_ASSERT_NOT_NULL(path);
    FILE *file = path_fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fputs("[00112233-4455-6677-8899-aabbccddeeff]\n"
                           "hostname = Unicode profile\n"
                           "mac = 00:11:22:33:44:55\n"
                           "address = 127.0.0.1:47989\n", file) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));

    known_host_t *hosts = known_hosts_parse(path);
    TEST_ASSERT_NOT_NULL(hosts);
    TEST_ASSERT_EQUAL_STRING("Unicode profile", hosts->hostname);
    TEST_ASSERT_EQUAL_STRING("127.0.0.1", hostport_get_hostname(hosts->address));
    known_hosts_free(hosts, known_hosts_node_free);
    TEST_ASSERT_EQUAL_INT(0, path_file_remove(path));
    free(path);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse);
    RUN_TEST(test_parse_utf8_path);
    return UNITY_END();
}

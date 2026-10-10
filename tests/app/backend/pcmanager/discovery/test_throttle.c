#include "unity.h"
#include "backend/pcmanager/discovery/throttle.h"

#include <SDL2/SDL_timer.h>

static discovery_throttle_t throttle;
static int counter = 0;

void callback(const sockaddr_t *addr, void *user_data) {
    (void) addr;
    (void) user_data;
    counter++;
}

void setUp(void) {
    counter = 0;
    discovery_throttle_init(&throttle, callback, NULL);
}

void tearDown(void) {
    discovery_throttle_deinit(&throttle);
}

void test_discovery_throttle(void) {
    sockaddr_t *addr = sockaddr_new();
    sockaddr_set_ip_str(addr, AF_INET, "192.168.1.110");
    sockaddr_set_port(addr, 47989);
    discovery_throttle_on_discovered(&throttle, addr, 10);
    discovery_throttle_on_discovered(&throttle, addr, 10);
    discovery_throttle_on_discovered(&throttle, addr, 10);
    TEST_ASSERT_EQUAL(1, counter);
    SDL_Delay(20);
    discovery_throttle_on_discovered(&throttle, addr, 10);
    TEST_ASSERT_EQUAL(2, counter);
    sockaddr_free(addr);
}

/*
 * Two hosts with staggered expiry. The single-host test above can only ever reach the
 * "everything in the list has expired" branch of throttle_hosts_evict(); this one reaches
 * the branch where part of the list is expired and part is still live, which is where the
 * list head used to be left pointing at the expired prefix -- dropping every live node out
 * of the list, so the next announcement from a still-throttled host counted as brand new.
 */
void test_discovery_throttle_partial_expiry(void) {
    sockaddr_t *a = sockaddr_new();
    sockaddr_set_ip_str(a, AF_INET, "192.168.1.110");
    sockaddr_set_port(a, 47989);
    sockaddr_t *b = sockaddr_new();
    sockaddr_set_ip_str(b, AF_INET, "192.168.1.111");
    sockaddr_set_port(b, 47989);

    discovery_throttle_on_discovered(&throttle, a, 200);
    TEST_ASSERT_EQUAL(1, counter);

    SDL_Delay(150);
    discovery_throttle_on_discovered(&throttle, b, 200);
    TEST_ASSERT_EQUAL(2, counter);

    /* a has expired, b has not. */
    SDL_Delay(100);
    discovery_throttle_on_discovered(&throttle, b, 200);
    TEST_ASSERT_EQUAL_MESSAGE(2, counter, "a live host must stay throttled when another expires");

    /* a really was evicted, so it is allowed through again. */
    discovery_throttle_on_discovered(&throttle, a, 200);
    TEST_ASSERT_EQUAL_MESSAGE(3, counter, "an expired host must be let through again");

    sockaddr_free(a);
    sockaddr_free(b);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_discovery_throttle);
    RUN_TEST(test_discovery_throttle_partial_expiry);
    return UNITY_END();
}
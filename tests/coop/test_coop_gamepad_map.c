/* Stable per-host controller slot and active-mask packing.
 *
 * Build + run:
 *   gcc -std=c99 -Wall -Wextra -Werror -Isrc/app/stream/input \
 *       tests/coop/test_coop_gamepad_map.c \
 *       src/app/stream/input/coop_gamepad_map.c \
 *       -o /tmp/test_coop_gamepad_map && /tmp/test_coop_gamepad_map
 */
#include "coop_gamepad_map.h"

#include <stdio.h>

static int failures = 0;

static void check(int condition, const char *what) {
    printf("  [%s] %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) {
        failures++;
    }
}

int main(void) {
    check(coop_gamepad_compact_slot(0xFFFFu, 0) == 0 &&
          coop_gamepad_compact_slot(0xFFFFu, 15) == 15,
          "single-host routing preserves controller ids");
    check(coop_gamepad_compact_mask(0xFFFFu, 0x8421u) == 0x8421u,
          "single-host routing preserves the active mask");

    const uint16_t primary_routes = (1u << 1) | (1u << 3);
    const uint16_t peer_routes = (1u << 0) | (1u << 2);
    check(coop_gamepad_compact_slot(primary_routes, 1) == 0 &&
          coop_gamepad_compact_slot(primary_routes, 3) == 1,
          "primary ids are compacted around peer-only pads");
    check(coop_gamepad_source_slot(primary_routes, 0) == 1 &&
          coop_gamepad_source_slot(primary_routes, 1) == 3,
          "primary feedback ids reverse-map to the physical pads");
    check(coop_gamepad_compact_slot(peer_routes, 0) == 0 &&
          coop_gamepad_compact_slot(peer_routes, 2) == 1,
          "peer ids are compacted around primary-only pads");
    check(coop_gamepad_source_slot(peer_routes, 0) == 0 &&
          coop_gamepad_source_slot(peer_routes, 1) == 2,
          "peer feedback ids keep their existing physical-pad mapping");
    check(coop_gamepad_compact_slot(primary_routes, 0) == -1 &&
          coop_gamepad_compact_slot(peer_routes, 3) == -1,
          "pads not routed to a host are rejected");

    /* Source pads 0 and 3 are connected. Pad 1 is configured for primary but
     * disconnected, so primary slot 0 must NOT appear active. Pad 3 retains
     * stable primary slot 1 rather than changing identity mid-session. */
    const uint16_t active = (1u << 0) | (1u << 3);
    check(coop_gamepad_compact_mask(primary_routes, active) == (1u << 1),
          "primary mask excludes configured-but-disconnected pads");
    check(coop_gamepad_compact_mask(peer_routes, active) == (1u << 0),
          "peer mask includes only active connected pads");

    const uint16_t both_source = (1u << 2);
    check(coop_gamepad_compact_mask(primary_routes | both_source, both_source) == (1u << 1) &&
          coop_gamepad_compact_mask(peer_routes, both_source) == (1u << 1),
          "a BOTH pad is active in each host's own compact slot space");
    check(coop_gamepad_compact_slot(primary_routes, -1) == -1 &&
          coop_gamepad_compact_slot(primary_routes, 16) == -1 &&
          coop_gamepad_source_slot(primary_routes, 2) == -1,
          "invalid source ids are rejected");

    printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

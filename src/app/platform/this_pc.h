#pragma once

#include <stdbool.h>

/**
 * Is @p address one of THIS machine's own addresses? On Windows the client PC can also be a co-op
 * host (its own Apollo), and it must then be offered ONLY as a co-op host -- never in the main PC
 * list, the settings' per-PC list, or as the launcher's selected PC (issue #39). One exception
 * (D15): while UNPAIRED it is listed in the main PC list so a fresh install can pair it; pairing it
 * still does not select it. Desktop Linux too since 2026-11-14 (platform/posix/this_pc_posix.c), for
 * host-and-client co-op there. Elsewhere (TV, Android) the client is never a host, so the answer is always no.
 */
#if defined(_WIN32) || (defined(OS_LINUX) && !defined(TARGET_WEBOS) && !defined(TARGET_ANDROID))
bool local_host_is_this_pc(const char *address);
#else
static inline bool local_host_is_this_pc(const char *address) {
    (void) address;
    return false;
}
#endif

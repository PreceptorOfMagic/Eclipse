/*
 * The Pi's controllers, as seen from the TV.
 *
 * WHY ECLIPSE CANNOT JUST ENUMERATE THEM. A pad attached to the Pi never reaches SDL
 * here: the Pi opens the evdev node itself and forwards XLINK_T_PAD_STATE straight to
 * a PC over UDP. So SDL_NumJoysticks() on the TV will never see it, and the co-op
 * picker -- which is built from SDL gamepads -- silently omitted a controller the user
 * was holding. Asking the Pi is the only way to know.
 *
 * ROUTING IS THE WHOLE POINT. The Pi decides which host it forwards to, and its
 * discovery cannot express "send this one to PC2" (it prefers whoever advertises
 * session=on, and PC2 does not run the discovery agent at all). The co-op picker is
 * where that choice is made, so it has to be able to tell the Pi.
 *
 * BROADCAST, NOT A CONFIGURED ADDRESS, because the Pi moves on DHCP like everything
 * else here and a pinned address is the exact failure this project keeps hitting. The
 * reply tells us where it came from, and that address is reused for the route command.
 *
 * SENT TWICE, TO 255.255.255.255 AND TO THE DIRECTED SUBNET BROADCAST. This TV has two
 * NICs (wired UE300 plus wlan0), and a global broadcast leaves by whichever one the
 * routing table prefers -- which may not be the one the Pi is on. A directed broadcast
 * derived from an address known to be reachable (a paired PC) is routed to THAT
 * subnet, so it goes out the interface that can actually reach it. The same class of
 * mistake as the WSL broadcast trap already recorded in this project.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COOP_PI_NAME_MAX 64
#define COOP_PI_ADDR_MAX 64

typedef struct {
    /** Device name as the Pi reports it, e.g. "Xbox Wireless Controller". */
    char name[COOP_PI_NAME_MAX];
    /** Where the Pi answered from -- used to send the routing command back. */
    char pi_addr[COOP_PI_ADDR_MAX];
} coop_pi_pad_t;

/**
 * A running discovery sweep.
 *
 * SPLIT INTO SEND AND DRAIN, AND NON-BLOCKING THROUGHOUT, because the caller polls it
 * from the UI thread. A blocking query is fine once, at open, but repeating one on a
 * timer would stall the interface for its timeout on every single tick -- and the whole
 * reason for polling is to notice a controller switched on later. So nothing here ever
 * waits: the send returns immediately and replies are collected on subsequent ticks.
 */
typedef struct {
    intptr_t fd;            /**< < 0 when the sweep could not start */
    coop_pi_pad_t pads[16];
    size_t count;
} coop_pi_scan_t;

/** Open the socket and send the first probe. Cheap, and never blocks. */
bool coop_pi_scan_begin(coop_pi_scan_t *scan, const char *subnet_hint);

/** Re-probe. Call every couple of seconds so pads that appear later are found. */
void coop_pi_scan_probe(coop_pi_scan_t *scan, const char *subnet_hint);

/**
 * Collect whatever has arrived since the last call, without waiting.
 * @return true when the pad set CHANGED, i.e. the caller should redraw.
 */
bool coop_pi_scan_poll(coop_pi_scan_t *scan);

void coop_pi_scan_end(coop_pi_scan_t *scan);

/**
 * Tell the Pi to forward @p pad_name to @p host_ip (a dotted quad); pass NULL for
 * @p host_ip to clear the pin and let the Pi discover a host for itself, or NULL for
 * @p pad_name to set the shared default for every pad.
 *
 * PER-PAD IS THE POINT: two controllers on one Pi must be able to drive two different
 * PCs, which a single shared setting cannot express.
 *
 * Waits for at most @p timeout_ms milliseconds. Unrelated UDP packets, acknowledgements
 * from another endpoint, and acknowledgements for a different applied route are ignored.
 * A non-positive timeout performs only an immediate poll.
 *
 * @return true only if the selected Pi acknowledged the exact route it applied.
 */
bool coop_pi_pads_route(const char *pi_addr, const char *pad_name,
                        const char *host_ip, int timeout_ms);

#ifdef __cplusplus
}
#endif

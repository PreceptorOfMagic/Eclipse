#pragma once
/*
 * This device's WIRED network card, for the host-side wired pin (Umbra, launch parameters auroraWiredMac /
 * auroraOwnMacs / auroraOwnIps; see the `wired_pin_mode` setting). Used on webOS: a TV with a USB-Ethernet
 * adapter AND Wi-Fi on the same subnet answers ARP from either card, and the host could send the whole
 * stream over Wi-Fi (measured 2026-08-22: 157 MB on wlan0, 9 KB on the adapter).
 *
 * THE WIRED CARD: not wireless (no /sys/class/net/<if>/wireless), named eth* or en*, up, with an IPv4
 * address; the fastest if there are several. Not "on USB": the TV's own Wi-Fi chip sits on USB too.
 */
#include <stdbool.h>

typedef struct wired_nic_t {
    char name[32];
    char mac[24];
    char own_macs[160];   /* every card's MAC, comma-separated */
    char own_ips[160];    /* every non-loopback IPv4 address, comma-separated */
} wired_nic_t;

/** Fill @p out; true when a wired card is up. Linux (webOS) only -- false elsewhere. */
bool wired_nic_discover(wired_nic_t *out);

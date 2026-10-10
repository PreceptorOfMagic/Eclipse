#include "wired_nic.h"

#include <string.h>

#if defined(__linux__)

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static bool read_line(const char *iface, const char *leaf, char *dest, size_t len) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/%s", iface, leaf);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    bool ok = fgets(dest, (int) len, f) != NULL;
    fclose(f);
    if (ok) {
        dest[strcspn(dest, "\r\n")] = '\0';
    }
    return ok;
}

static bool is_wireless(const char *iface) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", iface);
    if (access(path, F_OK) == 0) {
        return true;
    }
    snprintf(path, sizeof(path), "/sys/class/net/%s/phy80211", iface);
    return access(path, F_OK) == 0;
}

static void append_csv(char *list, size_t len, const char *item) {
    if (item[0] == '\0' || strstr(list, item) != NULL) {
        return;
    }
    const size_t used = strlen(list);
    snprintf(list + used, len - used, "%s%s", used > 0 ? "," : "", item);
}

bool wired_nic_discover(wired_nic_t *out) {
    memset(out, 0, sizeof(*out));
    struct ifaddrs *all = NULL;
    if (getifaddrs(&all) != 0) {
        return false;
    }
    long best_speed = -2;
    for (struct ifaddrs *a = all; a != NULL; a = a->ifa_next) {
        if (a->ifa_name == NULL || (a->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        char mac[24] = "";
        if (read_line(a->ifa_name, "address", mac, sizeof(mac)) && strcmp(mac, "00:00:00:00:00:00") != 0 &&
            strlen(mac) == 17) {
            append_csv(out->own_macs, sizeof(out->own_macs), mac);
        }
        if (a->ifa_addr == NULL || a->ifa_addr->sa_family != AF_INET || (a->ifa_flags & IFF_UP) == 0) {
            continue;
        }
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &((struct sockaddr_in *) a->ifa_addr)->sin_addr, ip, sizeof(ip)) == NULL) {
            continue;
        }
        append_csv(out->own_ips, sizeof(out->own_ips), ip);
        if (is_wireless(a->ifa_name) || (strncmp(a->ifa_name, "eth", 3) != 0 && strncmp(a->ifa_name, "en", 2) != 0)) {
            continue;
        }
        char oper[16] = "";
        if (!read_line(a->ifa_name, "operstate", oper, sizeof(oper)) || strcmp(oper, "up") != 0 ||
            strlen(mac) != 17) {
            continue;
        }
        char speed_text[16] = "";
        const long speed = read_line(a->ifa_name, "speed", speed_text, sizeof(speed_text)) ? atol(speed_text) : -1;
        if (speed > best_speed) {
            best_speed = speed;
            snprintf(out->name, sizeof(out->name), "%s", a->ifa_name);
            snprintf(out->mac, sizeof(out->mac), "%s", mac);
        }
    }
    freeifaddrs(all);
    return out->mac[0] != '\0';
}

#else

bool wired_nic_discover(wired_nic_t *out) {
    memset(out, 0, sizeof(*out));
    return false;
}

#endif

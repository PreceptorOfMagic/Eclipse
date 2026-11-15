/*
 * "Is this host address THIS machine?" for desktop Linux -- the same test as Windows
 * (platform/windows/local_host_win.c): binding a socket to an address succeeds only when an interface here owns
 * it, so no interface enumeration, and it follows DHCP moves. Loopback counts. Inside the Flatpak the network
 * namespace is the host's (--share=network), so the answer is the PC's own. See platform/this_pc.h.
 */
#include "platform/this_pc.h"

#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

bool local_host_is_this_pc(const char *address) {
    if (address == NULL || address[0] == '\0') {
        return false;
    }
    bool local = false;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST;
    struct addrinfo *res = NULL;
    if (getaddrinfo(address, NULL, &hints, &res) != 0) {
        return false;
    }
    for (struct addrinfo *ai = res; ai != NULL && !local; ai = ai->ai_next) {
        const int s = socket(ai->ai_family, SOCK_DGRAM, 0);
        if (s < 0) {
            continue;
        }
        local = bind(s, ai->ai_addr, ai->ai_addrlen) == 0;
        close(s);
    }
    freeaddrinfo(res);
    return local;
}

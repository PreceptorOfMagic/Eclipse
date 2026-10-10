#include "aurora_host_caps.h"

#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>

#include "app.h"
#include "pcmanager.h"
#include "platform/this_pc.h"
#include "types.h"

/** The fields only Umbra emits. Kept in ONE place: a second copy of this test would be a
 *  second definition of what "our host build" means, and they would drift. */
static bool server_is_aurora_apollo(const SERVER_DATA *server) {
    /* UmbraCaps is sent by every Umbra build from 2026-09-29, even as 0; EncoderVendor/CoopProfileCaps
     * came from a patch no deployed host carried, so without it this answered "no" for every real host. */
    return server != NULL && (server->umbraCapsKnown || server->encoderVendorKnown || server->coopProfileCaps != 0);
}

const char *umbra_wired_pin_host_name(void) {
    if (pcmanager == NULL) {
        return NULL;
    }
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        if (cur->server != NULL && (cur->server->umbraCaps & 1) != 0) {
            return cur->server->hostname;
        }
    }
    return NULL;
}

bool umbra_wired_pin_on_host(const uuidstr_t *uuid) {
    if (uuid == NULL || pcmanager == NULL) {
        return false;
    }
    const pclist_t *node = pcmanager_node(pcmanager, uuid);
    return node != NULL && node->server != NULL && (node->server->umbraCaps & 1) != 0;
}

bool umbra_monitor_off_on_host(const uuidstr_t *uuid) {
    if (uuid == NULL || pcmanager == NULL) {
        return false;
    }
    const pclist_t *node = pcmanager_node(pcmanager, uuid);
    return node != NULL && node->server != NULL && (node->server->umbraCaps & UMBRA_CAP_MONITOR_OFF) != 0 &&
           !local_host_is_this_pc(node->server->serverInfo.address);
}

bool aurora_apollo_on_host(const uuidstr_t *uuid) {
    if (uuid == NULL || pcmanager == NULL) {
        return false;
    }
    const pclist_t *node = pcmanager_node(pcmanager, uuid);
    return node != NULL && server_is_aurora_apollo(node->server);
}

bool aurora_apollo_anywhere(void) {
    return aurora_apollo_host_name() != NULL;
}

const char *aurora_apollo_host_name(void) {
    if (pcmanager == NULL) {
        return NULL;
    }
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        if (server_is_aurora_apollo(cur->server)) {
            /* The hostname is owned by SERVER_DATA, which is replaced when a host updates --
             * so this is only valid for the caller's immediate use (drawing a label). Said out
             * loud because backend/types.h warns about holding references to that struct. */
            return cur->server->hostname;
        }
    }
    return NULL;
}


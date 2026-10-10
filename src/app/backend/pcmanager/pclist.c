#include "util/diag_logs.h"
#include "util/diag_report.h"
#include "pclist.h"
#include "priv.h"

#include <assert.h>
#include <util/bus.h>

#include "listeners.h"
#include "logging.h"

#define LINKEDLIST_IMPL
#define LINKEDLIST_MODIFIER static
#define LINKEDLIST_TYPE pclist_t
#define LINKEDLIST_PREFIX pclist_ll
#define LINKEDLIST_DOUBLE 1

#include "linked_list.h"

#undef LINKEDLIST_DOUBLE
#undef LINKEDLIST_TYPE
#undef LINKEDLIST_PREFIX

#define LINKEDLIST_IMPL
#define LINKEDLIST_MODIFIER static
#define LINKEDLIST_TYPE appid_list_t
#define LINKEDLIST_PREFIX appid_list_ll
#define LINKEDLIST_DOUBLE 1

#include "linked_list.h"
#include "uuidstr.h"

#undef LINKEDLIST_DOUBLE
#undef LINKEDLIST_TYPE
#undef LINKEDLIST_PREFIX

#define LINKEDLIST_IMPL
#define LINKEDLIST_MODIFIER static
#define LINKEDLIST_TYPE padname_list_t
#define LINKEDLIST_PREFIX padname_ll
#define LINKEDLIST_DOUBLE 1

#include "linked_list.h"

#undef LINKEDLIST_DOUBLE
#undef LINKEDLIST_TYPE
#undef LINKEDLIST_PREFIX

static void upsert_perform(pclist_update_context_t *context);

static void upsert_async_perform(pclist_update_context_t *context);

static void remove_perform(pclist_update_context_t *context);

typedef struct {
    pcmanager_t *manager;
    sockaddr_t *addr;
} pclist_offline_context_t;

static void mark_offline_async_perform(pclist_offline_context_t *context);

static void pclist_ll_nodefree(pclist_t *node);

static int pclist_ll_compare_ip(pclist_t *other, const void *v);

static int pclist_ll_compare_address(pclist_t *other, const void *v);

static int pclist_ll_compare_uuid(pclist_t *other, const void *v);

static int appid_list_find_id(appid_list_t *other, const void *v);

pclist_t *pclist_insert_known(pcmanager_t *manager, const uuidstr_t *id, SERVER_DATA *server) {
    pclist_t *node = pclist_ll_new();
    node->id = *id;
    node->state.code = SERVER_STATE_NONE;
    node->server = server;
    node->known = true;
    manager->servers = pclist_ll_append(manager->servers, node);
    return node;
}

void pclist_upsert(pcmanager_t *manager, const uuidstr_t *uuid, const SERVER_STATE *state, SERVER_DATA *server) {
    assert(manager);
    assert(uuid);
    pclist_update_context_t args = {
            .manager = manager,
            .uuid = *uuid,
            .server = server,
            .state = *state
    };
    if (SDL_ThreadID() == manager->thread_id) {
        upsert_perform(&args);
    } else {
        app_bus_post_sync(manager->app, (bus_actionfunc) upsert_perform, &args);
    }
}

void pclist_upsert_async(pcmanager_t *manager, const uuidstr_t *uuid,
                         const SERVER_STATE *state, SERVER_DATA *server) {
    assert(manager);
    assert(uuid);
    pclist_update_context_t *context = SDL_malloc(sizeof(*context));
    if (context == NULL) {
        serverdata_free(server);
        return;
    }
    *context = (pclist_update_context_t) {
            .manager = manager,
            .uuid = *uuid,
            .server = server,
            .state = *state,
    };
    if (SDL_ThreadID() == manager->thread_id) {
        upsert_async_perform(context);
    } else if (!app_bus_post(manager->app, (bus_actionfunc) upsert_async_perform, context)) {
        serverdata_free(context->server);
        SDL_free(context);
    }
}

void pclist_mark_offline_async(pcmanager_t *manager, const sockaddr_t *addr) {
    assert(manager);
    assert(addr);
    pclist_offline_context_t *context = SDL_calloc(1, sizeof(*context));
    if (context == NULL) {
        return;
    }
    context->manager = manager;
    context->addr = sockaddr_clone(addr);
    if (context->addr == NULL) {
        SDL_free(context);
        return;
    }
    if (SDL_ThreadID() == manager->thread_id) {
        mark_offline_async_perform(context);
    } else if (!app_bus_post(manager->app, (bus_actionfunc) mark_offline_async_perform, context)) {
        sockaddr_free(context->addr);
        SDL_free(context);
    }
}

/**
 * Thread safe.
 * @param manager
 * @param uuid
 */
void pclist_remove(pcmanager_t *manager, const uuidstr_t *uuid) {
    assert(manager);
    assert(uuid);
    pclist_update_context_t context = {
            .uuid = *uuid,
            .manager = manager,
    };
    if (SDL_ThreadID() == manager->thread_id) {
        remove_perform(&context);
    } else {
        app_bus_post_sync(manager->app, (bus_actionfunc) remove_perform, &context);
    }
}

void pclist_free(pcmanager_t *manager) {
    pclist_ll_free(manager->servers, pclist_ll_nodefree);
}


bool pcmanager_node_is_app_favorite(const pclist_t *node, int appid) {
    return appid_list_ll_find_by(node->favs, &appid, appid_list_find_id) != NULL;
}

bool pcmanager_node_is_app_hidden(const pclist_t *node, int appid) {
    return appid_list_ll_find_by(node->hidden, &appid, appid_list_find_id) != NULL;
}

bool pclist_node_set_app_favorite(pclist_t *node, int appid, bool favorite) {
    appid_list_t *existing = appid_list_ll_find_by(node->favs, &appid, appid_list_find_id);
    if (favorite) {
        if (existing) { return false; }
        appid_list_t *item = appid_list_ll_new();
        item->id = appid;
        node->favs = appid_list_ll_append(node->favs, item);
    } else if (existing) {
        node->favs = appid_list_ll_remove(node->favs, existing);
    }
    return true;
}

bool pclist_node_set_app_hidden(pclist_t *node, int appid, bool hidden) {
    appid_list_t *existing = appid_list_ll_find_by(node->hidden, &appid, appid_list_find_id);
    if (hidden) {
        if (existing) { return false; }
        appid_list_t *item = appid_list_ll_new();
        item->id = appid;
        node->hidden = appid_list_ll_append(node->hidden, item);
    } else if (existing) {
        node->hidden = appid_list_ll_remove(node->hidden, existing);
    }
    return true;
}

/** A hypervisor's virtual adapter (Hyper-V/WSL, VMware, VirtualBox, Parallels, QEMU), by the MAC's vendor
 *  prefix: an address on one is reachable from that PC only, and wake-on-LAN cannot use its MAC. */
static bool mac_is_virtual_adapter(const char *mac) {
    unsigned int a = 0, b = 0, c = 0;
    if (mac == NULL || (SDL_sscanf(mac, "%2x:%2x:%2x", &a, &b, &c) != 3 &&
                        SDL_sscanf(mac, "%2x-%2x-%2x", &a, &b, &c) != 3)) {
        return false;
    }
    static const unsigned int virtual_oui[] = {
            0x00155D,                               /* Hyper-V (the WSL / Default Switch adapters) */
            0x005056, 0x000C29, 0x000569, 0x001C14, /* VMware */
            0x080027, 0x0A0027,                     /* VirtualBox */
            0x001C42,                               /* Parallels */
            0x525400,                               /* QEMU/KVM */
    };
    const unsigned int oui = (a << 16) | (b << 8) | c;
    for (size_t i = 0; i < SDL_arraysize(virtual_oui); i++) {
        if (virtual_oui[i] == oui) {
            return true;
        }
    }
    return false;
}

void pclist_node_apply(pclist_t *node, const SERVER_STATE *state, SERVER_DATA *server) {
    if (state != NULL && state->code != SERVER_STATE_NONE) {
        node->state = *state;
    }
    if (server != NULL) {
        if (node->server != server) {
            if (node->server) {
                /* #16: a PC with a Hyper-V switch (WSL, Docker) answers discovery on that virtual
                 * adapter too, and the reply carries THAT adapter's address and MAC -- SKYNET's
                 * 172.21.80.1 / 00:15:5d:.. -- so hosts.ini flipped to whichever answer came last.
                 * The reply's other data is the same PC's; only keep the real adapter's address and
                 * MAC over a virtual one's. A real adapter's answer still replaces anything. */
                /* "Real" also means NOT all zeros: Apollo answers every UNPAIRED (HTTP) request with
                 * 00:00:00:00:00:00, and a host whose only MAC is virtual (a VM) must not have its
                 * first unpaired address pinned for good (diff-sentinel 2026-09-28). */
                if (server->mac != NULL && node->server->mac != NULL &&
                    node->server->serverInfo.address != NULL &&
                    mac_is_virtual_adapter(server->mac) && !mac_is_virtual_adapter(node->server->mac) &&
                    SDL_strcmp(node->server->mac, "00:00:00:00:00:00") != 0) {
                    commons_log_info("PCManager", "%s answered on a virtual adapter (%s, %s): keeping %s, %s",
                                     server->hostname != NULL ? server->hostname : "host",
                                     server->serverInfo.address != NULL ? server->serverInfo.address : "?",
                                     server->mac, node->server->serverInfo.address, node->server->mac);
                    const char *mac = server->mac, *address = server->serverInfo.address;
                    server->mac = node->server->mac;
                    server->serverInfo.address = node->server->serverInfo.address;
                    node->server->mac = mac;
                    node->server->serverInfo.address = address;
                }
                serverdata_free(node->server);
            }
            uuidstr_fromstr(&node->id, server->uuid);
            node->server = server;
        }
        node->known |= server->paired;
    }
}

void pclist_ll_nodefree(pclist_t *node) {
    if (node->server) {
        serverdata_free((PSERVER_DATA) node->server);
    }
    if (node->favs) {
        appid_list_ll_free(node->favs, (appid_list_ll_nodefree_fn) free);
    }
    if (node->hidden) {
        appid_list_ll_free(node->hidden, (appid_list_ll_nodefree_fn) free);
    }
    if (node->pin_code) {
        SDL_free(node->pin_code);
    }
    if (node->pads) {
        padname_ll_free(node->pads, (padname_ll_nodefree_fn) free);
    }
    free(node);
}

bool pclist_node_has_pad(const pclist_t *node, const char *name) {
    if (node == NULL || name == NULL) {
        return false;
    }
    for (const padname_list_t *cur = node->pads; cur != NULL; cur = cur->next) {
        if (SDL_strcmp(cur->name, name) == 0) {
            return true;
        }
    }
    return false;
}

void pclist_node_set_pad(pclist_t *node, const char *name, bool assigned) {
    if (node == NULL || name == NULL || name[0] == '\0') {
        return;
    }
    for (padname_list_t *cur = node->pads; cur != NULL; cur = cur->next) {
        if (SDL_strcmp(cur->name, name) != 0) {
            continue;
        }
        if (!assigned) {
            node->pads = padname_ll_remove(node->pads, cur);
            free(cur);
        }
        return;                     /* already in the state that was asked for */
    }
    if (!assigned) {
        return;
    }
    padname_list_t *item = padname_ll_new();
    SDL_strlcpy(item->name, name, sizeof(item->name));
    node->pads = padname_ll_append(node->pads, item);
}


pclist_t *pclist_find_by_ip(pcmanager_t *manager, const char *ip) {
    SDL_assert_release(ip != NULL);
    pcmanager_lock(manager);
    pclist_t *result = pclist_ll_find_by(manager->servers, ip, pclist_ll_compare_ip);
    pcmanager_unlock(manager);
    return result;
}

pclist_t *pclist_find_by_addr(pcmanager_t *manager, const sockaddr_t *addr) {
    SDL_assert_release(addr != NULL);
    pcmanager_lock(manager);
    pclist_t *result = pclist_ll_find_by(manager->servers, addr, pclist_ll_compare_address);
    pcmanager_unlock(manager);
    return result;
}

pclist_t *pclist_find_by_uuid(pcmanager_t *manager, const uuidstr_t *uuid) {
    SDL_assert_release(uuid != NULL);
    pcmanager_lock(manager);
    pclist_t *result = pclist_ll_find_by(manager->servers, uuid, pclist_ll_compare_uuid);
    pcmanager_unlock(manager);
    return result;
}

static int pclist_ll_compare_ip(pclist_t *other, const void *v) {
    SDL_assert_release(v);
    SDL_assert_release(other);
    SDL_assert_release(other->server);
    SDL_assert_release(other->server->serverInfo.address);
    return SDL_strcmp(other->server->serverInfo.address, (const char *) v);
}

static int pclist_ll_compare_address(pclist_t *other, const void *v) {
    SDL_assert_release(v);
    SDL_assert_release(other);
    SDL_assert_release(other->server);
    SDL_assert_release(other->server->serverInfo.address);
    const sockaddr_t *addr = (const sockaddr_t *) v;
    char ip[64];
    if (sockaddr_get_ip_str(addr, ip, sizeof(ip)) != 0) {
        return 1;
    }
    return SDL_strcmp(other->server->serverInfo.address, ip);
}

static int pclist_ll_compare_uuid(pclist_t *other, const void *v) {
    SDL_assert_release(v);
    SDL_assert_release(other);
    SDL_assert_release(other->server);
    return !uuidstr_t_equals_t(&other->id, (const uuidstr_t *) v);
}

static int appid_list_find_id(appid_list_t *other, const void *v) {
    return other->id - *((const int *) v);
}

static void upsert_perform(pclist_update_context_t *context) {
    pcmanager_t *manager = context->manager;
    pcmanager_lock(manager);
    pclist_t *node = pclist_ll_find_by(manager->servers, &context->uuid, pclist_ll_compare_uuid);
    bool updated = node != NULL;
    if (!node) {
        node = pclist_ll_new();
        manager->servers = pclist_ll_append(manager->servers, node);
    }
    pclist_node_apply(node, &context->state, context->server);
    if (diag_logs_active()) {
        diag_report_log_host(node);   /* what this computer reports, whenever that changes */
    }
    pcmanager_unlock(manager);
    pcmanager_listeners_notify(manager, &context->uuid, updated ? PCMANAGER_NOTIFY_UPDATED : PCMANAGER_NOTIFY_ADDED);
}

static void upsert_async_perform(pclist_update_context_t *context) {
    upsert_perform(context);
    SDL_free(context);
}

static void mark_offline_async_perform(pclist_offline_context_t *context) {
    pcmanager_t *manager = context->manager;
    uuidstr_t uuid = {0};
    bool found = false;
    pcmanager_lock(manager);
    pclist_t *node = pclist_ll_find_by(manager->servers, context->addr,
                                      pclist_ll_compare_address);
    if (node != NULL) {
        node->state.code = SERVER_STATE_OFFLINE;
        uuid = node->id;
        found = true;
        if (diag_logs_active()) {
            diag_report_log_host(node);
        }
    }
    pcmanager_unlock(manager);
    if (found) {
        pcmanager_listeners_notify(manager, &uuid, PCMANAGER_NOTIFY_UPDATED);
    }
    sockaddr_free(context->addr);
    SDL_free(context);
}

static void remove_perform(pclist_update_context_t *context) {
    pcmanager_t *manager = context->manager;
    pcmanager_lock(manager);
    pclist_t *node = pclist_ll_find_by(manager->servers, &context->uuid, pclist_ll_compare_uuid);
    if (!node) {
        pcmanager_unlock(manager);
        return;
    }
    manager->servers = pclist_ll_remove(manager->servers, node);
    pcmanager_unlock(manager);
    pcmanager_listeners_notify(manager, &context->uuid, PCMANAGER_NOTIFY_REMOVED);
    pclist_ll_nodefree(node);
}

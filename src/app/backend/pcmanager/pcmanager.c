#include "backend/pcmanager.h"
#include "discovery/discovery.h"
#include "priv.h"

#include "pclist.h"
#include "app.h"
#include "backend/pcmanager/worker/worker.h"
#include "logging.h"
#include "util/bus.h"

pcmanager_t *pcmanager_new(app_t *app, executor_t *executor) {
    pcmanager_t *manager = SDL_calloc(1, sizeof(pcmanager_t));
    manager->app = app;
    manager->executor = executor;
    manager->thread_id = SDL_ThreadID();
    manager->lock = SDL_CreateMutex();
    discovery_init(&manager->discovery, (discovery_callback) pcmanager_lan_host_discovered, manager);
    pcmanager_load_known_hosts(manager);
    return manager;
}

void pcmanager_destroy(pcmanager_t *manager) {
    pcmanager_auto_discovery_stop(manager);
    /* A discovery result may have been queued just before stop joined its
     * worker. Consume that manager-owned action while the manager/list still
     * exist; otherwise a later bus drain would dereference freed state. */
    SDL_assert_release(SDL_ThreadID() == manager->thread_id);
    app_bus_drain();
    pcmanager_save_known_hosts(manager);
    pclist_free(manager);
    discovery_deinit(&manager->discovery);
    SDL_DestroyMutex(manager->lock);
    SDL_free(manager);
}


void pcmanager_auto_discovery_start(pcmanager_t *manager) {
    discovery_start(&manager->discovery);
}

void pcmanager_auto_discovery_stop(pcmanager_t *manager) {
    discovery_stop(&manager->discovery);
}

void pcmanager_lock(pcmanager_t *manager) {
    SDL_LockMutex(manager->lock);
}

void pcmanager_unlock(pcmanager_t *manager) {
    SDL_UnlockMutex(manager->lock);
}

bool pcmanager_quitapp(pcmanager_t *manager, const uuidstr_t *uuid, pcmanager_callback_t callback, void *userdata) {
    if (pcmanager_server_current_app(pcmanager, uuid) == 0) {
        return false;
    }
    worker_context_t *ctx = worker_context_new(manager, uuid, callback, userdata);
    pcmanager_worker_queue(manager, worker_quit_app, ctx);
    return true;
}

void pcmanager_request_update(pcmanager_t *manager, const uuidstr_t *uuid, pcmanager_callback_t callback,
                              void *userdata) {
    commons_log_info("PcManager", "Requesting update for %s", (const char *) uuid);
    worker_context_t *ctx = worker_context_new(manager, uuid, callback, userdata);
    pcmanager_worker_queue(manager, worker_host_update, ctx);
}

void pcmanager_favorite_app(pcmanager_t *manager, const uuidstr_t *uuid, int appid, bool favorite) {
    pcmanager_lock(manager);
    pclist_t *node = pclist_find_by_uuid(manager, uuid);
    if (!node) {
        goto unlock;
    }
    pclist_node_set_app_favorite(node, appid, favorite);
    unlock:
    pcmanager_unlock(manager);
}

void pcmanager_set_app_hidden(pcmanager_t *manager, const uuidstr_t *uuid, int appid, bool hidden) {
    pcmanager_lock(manager);
    pclist_t *node = pclist_find_by_uuid(manager, uuid);
    if (!node) {
        goto unlock;
    }
    pclist_node_set_app_hidden(node, appid, hidden);
    unlock:
    pcmanager_unlock(manager);
}

void pcmanager_set_host_pin(pcmanager_t *manager, const uuidstr_t *uuid, const char *pin,
                            bool is_password) {
    pcmanager_lock(manager);
    pclist_t *node = pclist_find_by_uuid(manager, uuid);
    if (node) {
        if (node->pin_code) {
            SDL_free(node->pin_code);
            node->pin_code = NULL;
        }
        if (pin != NULL && pin[0] != '\0') {
            node->pin_code = SDL_strdup(pin);
        }
        /* Cleared with the credential: a kind left behind on an empty slot would
         * decide how the NEXT one is typed. */
        node->pin_is_password = node->pin_code != NULL && is_password;
    }
    pcmanager_unlock(manager);
    /* Persist immediately: a provisioned PIN must survive a crash/kill, unlike
     * favorites which are flushed only on clean shutdown. */
    if (node) {
        pcmanager_save_known_hosts(manager);
    }
}

bool pcmanager_node_has_pad(const pclist_t *node, const char *name) {
    return pclist_node_has_pad(node, name);
}

void pcmanager_set_host_pad(pcmanager_t *manager, const uuidstr_t *uuid, const char *name, bool assigned) {
    pcmanager_lock(manager);
    pclist_t *node = pclist_find_by_uuid(manager, uuid);
    if (node != NULL) {
        pclist_node_set_pad(node, name, assigned);
    }
    pcmanager_unlock(manager);
    if (node != NULL) {
        pcmanager_save_known_hosts(manager);
    }
}

void pcmanager_set_host_gpu(pcmanager_t *manager, const uuidstr_t *uuid, int vendor) {
    pcmanager_lock(manager);
    pclist_t *node = pclist_find_by_uuid(manager, uuid);
    if (node != NULL) {
        node->gpu_vendor = vendor;
    }
    pcmanager_unlock(manager);
    if (node != NULL) {
        /* Saved IMMEDIATELY, like a pad assignment: the answer was given in a dialog the user
         * is about to leave, and a co-op launch that crashes must not lose it and ask again. */
        pcmanager_save_known_hosts(manager);
    }
}

bool pcmanager_select(pcmanager_t *manager, const uuidstr_t *uuid) {
    pcmanager_lock(manager);
    const pclist_t *node = pcmanager_node(manager, uuid);
    if (node == NULL) {
        pcmanager_unlock(manager);
        return false;
    }
    for (pclist_t *cur = pcmanager->servers; cur; cur = cur->next) {
        cur->selected = node == cur;
    }
    pcmanager_unlock(manager);
    return true;
}

bool pcmanager_forget(pcmanager_t *manager, const uuidstr_t *uuid) {
    const pclist_t *node = pcmanager_node(manager, uuid);
    if (node == NULL) {
        return false;
    }
    pclist_remove(manager, uuid);
    return true;
}

const pclist_t *pcmanager_node(pcmanager_t *manager, const uuidstr_t *uuid) {
    return pclist_find_by_uuid(manager, uuid);
}

const SERVER_STATE *pcmanager_state(pcmanager_t *manager, const uuidstr_t *uuid) {
    const pclist_t *node = pcmanager_node(manager, uuid);
    if (!node) {
        return NULL;
    }
    return &node->state;
}

int pcmanager_server_current_app(pcmanager_t *manager, const uuidstr_t *uuid) {
    const pclist_t *node = pcmanager_node(manager, uuid);
    if (!node) {
        return 0;
    }
    return pcmanager_node_current_app(node);
}

int pcmanager_node_current_app(const pclist_t *node) {
    SDL_assert_release(node != NULL);
    return node->server->currentGame;
}

bool pcmanager_send_wol(pcmanager_t *manager, const uuidstr_t *uuid, pcmanager_callback_t callback,
                        void *userdata) {
    worker_context_t *ctx = worker_context_new(manager, uuid, callback, userdata);
    pcmanager_worker_queue(manager, worker_wol, ctx);
    return true;
}

const pclist_t *pcmanager_servers(pcmanager_t *manager) {
    return manager->servers;
}

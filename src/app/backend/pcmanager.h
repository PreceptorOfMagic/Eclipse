#pragma once

#include <stdbool.h>

#include "backend/types.h"

#include "libgamestream/client.h"
#include "uuidstr.h"
#include "sockaddr.h"
#include "hostport.h"

typedef struct app_t app_t;
typedef struct pcmanager_t pcmanager_t;
typedef struct worker_context_t worker_context_t;
typedef struct executor_t executor_t;

typedef void (*pcmanager_callback_t)(int result, const char *error, const uuidstr_t *uuid, void *userdata);

typedef void(*pcmanager_listener_fn)(const uuidstr_t *uuid, void *userdata);

typedef struct pcmanager_listener_t {
    pcmanager_listener_fn added;
    pcmanager_listener_fn updated;
    pcmanager_listener_fn removed;
} pcmanager_listener_t;

/**
 * @brief Initialize computer manager context
 * 
 */
pcmanager_t *pcmanager_new(app_t *app, executor_t *executor);

/**
 * @brief Free all allocated memories, such as computer_list.
 * 
 */
void pcmanager_destroy(pcmanager_t *manager);

void pcmanager_auto_discovery_start(pcmanager_t *manager);

void pcmanager_auto_discovery_stop(pcmanager_t *manager);

/** Write the known hosts now (pairings, addresses, per-PC choices). Normally done on clean shutdown. */
void pcmanager_save_known_hosts(pcmanager_t *manager);

const pclist_t *pcmanager_servers(pcmanager_t *manager);

const pclist_t *pcmanager_node(pcmanager_t *manager, const uuidstr_t *uuid);

const SERVER_STATE *pcmanager_state(pcmanager_t *manager, const uuidstr_t *uuid);

bool pcmanager_node_is_app_favorite(const pclist_t *node, int appid);

bool pcmanager_node_is_app_hidden(const pclist_t *node, int appid);

int pcmanager_node_current_app(const pclist_t *node);

int pcmanager_server_current_app(pcmanager_t *manager, const uuidstr_t *uuid);

void pcmanager_favorite_app(pcmanager_t *manager, const uuidstr_t *uuid, int appid, bool favorite);

void pcmanager_set_app_hidden(pcmanager_t *manager, const uuidstr_t *uuid, int appid, bool hidden);

/**
 * Set (or clear) the per-host Windows login PIN used for Wake-on-LAN auto-entry.
 * Pass NULL or "" to clear. Persists to hosts.ini immediately.
 */
/**
 * Set (or clear, with NULL/empty) the host's Wake-on-LAN login credential.
 * @param is_password false = a PIN (digits, typed on a number pad and usually
 *        auto-submitted by Windows Hello); true = a password (full character set,
 *        always needs Enter).
 */
void pcmanager_set_host_pin(pcmanager_t *manager, const uuidstr_t *uuid, const char *pin,
                            bool is_password);

/**
 * CO-OP DEFAULT CONTROLLERS, per host. A host may claim any number of them, and a
 * controller may default to more than one host -- one pad driving both machines is a
 * real case (menus during setup, or one person on both panes). Persists to hosts.ini
 * immediately, like the PIN: a controller layout is set up once and must survive a kill.
 */
bool pcmanager_node_has_pad(const pclist_t *node, const char *name);

void pcmanager_set_host_pad(pcmanager_t *manager, const uuidstr_t *uuid, const char *name, bool assigned);

/** Record what the user said this host's GPU is (coop_gpu_vendor_t). Persisted to hosts.ini
 *  at once. A host that reports its own encoder overrules this -- see types.h. */
void pcmanager_set_host_gpu(pcmanager_t *manager, const uuidstr_t *uuid, int vendor);

bool pcmanager_select(pcmanager_t *manager, const uuidstr_t *uuid);

bool pcmanager_forget(pcmanager_t *manager, const uuidstr_t *uuid);

void pcmanager_register_listener(pcmanager_t *manager, const pcmanager_listener_t *listener, void *userdata);

void pcmanager_unregister_listener(pcmanager_t *manager, const pcmanager_listener_t *listener);

/* ------------------------------------------------------------------------------ */
/*                               Async functions                                  */
/* ------------------------------------------------------------------------------ */

/**
 * Add a host by its IP address
 * @param manager
 * @param address
 * @param callback
 * @param userdata
 * @return
 */
bool pcmanager_manual_add(pcmanager_t *manager, hostport_t *host, pcmanager_callback_t callback, void *userdata);

/**
 * @brief Generates a PIN code, and start pairing process.
 * Generated PIN code will be written into `pin` pointer.
 * 
 * @param p 
 * @param pin 
 */
bool pcmanager_pair(pcmanager_t *manager, const uuidstr_t *uuid, char *pin, pcmanager_callback_t callback,
                    void *userdata);

/**
 * Quit application running on host
 * @param manager
 * @param uuid
 * @param callback
 * @param userdata
 * @return
 */
bool pcmanager_quitapp(pcmanager_t *manager, const uuidstr_t *uuid, pcmanager_callback_t callback, void *userdata);

/**
 * Fetch host information
 * @param manager
 * @param uuid
 * @param callback
 * @param userdata
 */
void pcmanager_request_update(pcmanager_t *manager, const uuidstr_t *uuid, pcmanager_callback_t callback,
                              void *userdata);

/**
 * Send Wake-on-LAN packet, and request host info update
 * @param manager
 * @param uuid
 * @param callback
 * @param userdata
 * @return
 */
bool pcmanager_send_wol(pcmanager_t *manager, const uuidstr_t *uuid, pcmanager_callback_t callback,
                        void *userdata);

/* ------------------------------------------------------------------------------ */
/*                             IO-thread functions                                */
/* ------------------------------------------------------------------------------ */

/**
 *
 * @param manager pcmanager instance
 * @param ip
 * @param force If false, update will be omitted if the server is already online
 * @param callback
 * @param userdata
 * @return
 */
int pcmanager_update_by_host(worker_context_t *context, const char *ip, uint16_t port, bool force);


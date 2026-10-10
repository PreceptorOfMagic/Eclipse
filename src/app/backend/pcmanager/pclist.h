/**
 * @file pclist.h
 *
 * Thread safe list management
 */
#pragma once

#include "../pcmanager.h"

pclist_t *pclist_insert_known(pcmanager_t *manager, const uuidstr_t *uuid, SERVER_DATA *server);

/**
 * Update item in server list if exists. Otherwise insert.
 * @param manager
 * @param server
 */
void pclist_upsert(pcmanager_t *manager, const uuidstr_t *uuid, const SERVER_STATE *state, SERVER_DATA *server);

/**
 * Queue an upsert on the UI thread without waiting for it. Ownership of
 * `server` is consumed on both success and failure. This is used by discovery:
 * launcher teardown runs on the UI thread and must be able to join the
 * discovery worker without that worker waiting synchronously for the UI.
 */
void pclist_upsert_async(pcmanager_t *manager, const uuidstr_t *uuid,
                         const SERVER_STATE *state, SERVER_DATA *server);

/** Queue an address-matched OFFLINE transition on the UI thread. */
void pclist_mark_offline_async(pcmanager_t *manager, const sockaddr_t *addr);

void pclist_node_apply(pclist_t *node, const SERVER_STATE *state, SERVER_DATA *server);

bool pclist_node_set_app_favorite(pclist_t *node, int appid, bool favorite);

bool pclist_node_set_app_hidden(pclist_t *node, int appid, bool hidden);

bool pclist_node_has_pad(const pclist_t *node, const char *name);

void pclist_node_set_pad(pclist_t *node, const char *name, bool assigned);

void pclist_remove(pcmanager_t *manager, const uuidstr_t *uuid);

void pclist_free(pcmanager_t *manager);

pclist_t *pclist_find_by_uuid(pcmanager_t *manager, const uuidstr_t *uuid);

pclist_t *pclist_find_by_ip(pcmanager_t *manager, const char *ip);

pclist_t *pclist_find_by_addr(pcmanager_t *manager, const sockaddr_t *addr);

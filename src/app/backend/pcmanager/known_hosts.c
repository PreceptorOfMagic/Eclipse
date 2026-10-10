#include "known_hosts.h"
#include "stream/video/coop_stream_mode.h"
#include "priv.h"
#include "pclist.h"
#include "app.h"

#include <ini.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "ini_writer.h"
#include "util/ini_ext.h"
#include "util/path.h"
#include "app_settings.h"

#define LINKEDLIST_IMPL
#define LINKEDLIST_MODIFIER static
#define LINKEDLIST_TYPE appid_list_t
#define LINKEDLIST_PREFIX appid_list
#define LINKEDLIST_DOUBLE 1

#include "linked_list.h"
#include "logging.h"

#undef LINKEDLIST_DOUBLE
#undef LINKEDLIST_TYPE
#undef LINKEDLIST_PREFIX

#define LINKEDLIST_IMPL
#define LINKEDLIST_MODIFIER static
#define LINKEDLIST_TYPE padname_list_t
#define LINKEDLIST_PREFIX padname_list
#define LINKEDLIST_DOUBLE 1

#include "linked_list.h"

#undef LINKEDLIST_DOUBLE
#undef LINKEDLIST_TYPE
#undef LINKEDLIST_PREFIX

static int known_hosts_handle(known_host_t **list, const char *section, const char *name, const char *value);

static int known_hosts_find_uuid(known_host_t *node, void *v);

static known_host_t *known_hosts_parse_impl(const char *conf_file, bool *save_allowed);

void pcmanager_load_known_hosts(pcmanager_t *manager) {
    commons_log_info("PCManager", "Load unknown hosts");
    char *conf_file = path_join(manager->app->settings.conf_dir, CONF_NAME_HOSTS);
    known_host_t *hosts = known_hosts_parse_impl(conf_file, &manager->known_hosts_save_allowed);

    bool selected_set = false;
    for (known_host_t *cur = hosts; cur; cur = cur->next) {
        const char *mac = cur->mac, *hostname = cur->hostname;
        const hostport_t *address = cur->address;
        char address_buf[256] = {0};
        hostport_to_string(address, address_buf, sizeof(address_buf));
        if (!mac || !hostname || !address) {
            commons_log_warn("PCManager", "Unknown host entry: mac=%s, hostname=%s, address=%s", mac, hostname,
                             address_buf);
            continue;
        }

        PSERVER_DATA server = serverdata_new();
        server->uuid = uuidstr_tostr(&cur->uuid);
        server->mac = mac;
        server->hostname = hostname;
        server->serverInfo.address = strdup(hostport_get_hostname(address));
        server->extPort = hostport_get_port(address);

        pclist_t *node = pclist_insert_known(manager, &cur->uuid, server);

        node->favs = cur->favs;
        cur->favs = NULL;
        node->hidden = cur->hidden;
        cur->hidden = NULL;
        node->pin_code = cur->pin_code;
        cur->pin_code = NULL;
        node->pin_is_password = cur->pin_is_password;
        node->pads = cur->pads;
        cur->pads = NULL;
        node->gpu_vendor = cur->gpu_vendor;

        if (!selected_set && cur->selected) {
            commons_log_info("PCManager", "Known host %s was selected", cur->hostname);
            node->selected = true;
            selected_set = true;
        }
    }
    known_hosts_free(hosts, known_hosts_node_free);
    free(conf_file);
}

void pcmanager_save_known_hosts(pcmanager_t *manager) {
    if (!manager->known_hosts_save_allowed) {
        commons_log_error("PCManager", "Not replacing hosts.ini after an earlier read/parse failure");
        return;
    }
    char *conf_file = path_join(manager->app->settings.conf_dir, CONF_NAME_HOSTS);
    if (conf_file == NULL) {
        return;
    }
    const size_t staging_capacity = strlen(conf_file) + 40;
    char *staging_file = malloc(staging_capacity);
    if (staging_file == NULL) {
        free(conf_file);
        return;
    }
#if defined(_WIN32)
    const unsigned long process_id = GetCurrentProcessId();
#else
    const unsigned long process_id = (unsigned long) getpid();
#endif
    snprintf(staging_file, staging_capacity, "%s.tmp.%lu", conf_file, process_id);
    FILE *fp = path_fopen(staging_file, "wb");
    if (!fp) {
        free(staging_file);
        free(conf_file);
        return;
    }

    bool selected_set = false;
    for (pclist_t *cur = manager->servers; cur != NULL; cur = cur->next) {
        if (!cur->server || !cur->known) {
            continue;
        }
        const SERVER_DATA *server = cur->server;
        hostport_t *address = hostport_new(server->serverInfo.address, server->extPort);
        if (address == NULL) {
            continue;
        }
        ini_write_section(fp, server->uuid);

        ini_write_string(fp, "mac", server->mac);
        ini_write_string(fp, "hostname", server->hostname);
        if (cur->pin_code != NULL && cur->pin_code[0] != '\0') {
            ini_write_string(fp, "pin_code", cur->pin_code);
            /* Only alongside a credential: a bare kind with no code says nothing. */
            ini_write_bool(fp, "pin_is_password", cur->pin_is_password);
        }

        char address_buf[260] = {0};
        hostport_to_string(address, address_buf, sizeof(address_buf));
        ini_write_string(fp, "address", address_buf);
        hostport_free(address);

        if (!selected_set && cur->selected) {
            ini_write_bool(fp, "selected", true);
            selected_set = true;
        }
        if (cur->favs) {
            ini_write_comment(fp, " favorites list");
            for (appid_list_t *idcur = cur->favs; idcur; idcur = idcur->next) {
                ini_write_int(fp, "favorite", idcur->id);
            }
        }
        if (cur->hidden) {
            ini_write_comment(fp, " hidden apps list");
            for (appid_list_t *idcur = cur->hidden; idcur; idcur = idcur->next) {
                ini_write_int(fp, "hidden", idcur->id);
            }
        }
        /* Written as a WORD, not the enum's number: hosts.ini is a file people edit, and a
         * bare `gpu = 2` says nothing to the next reader. Absent means never answered. */
        if (cur->gpu_vendor != COOP_GPU_UNKNOWN) {
            ini_write_string(fp, "gpu", coop_gpu_vendor_token(cur->gpu_vendor));
        }
        if (cur->pads) {
            ini_write_comment(fp, " controllers that default to this host in co-op");
            for (padname_list_t *padcur = cur->pads; padcur; padcur = padcur->next) {
                ini_write_string(fp, "pad", padcur->name);
            }
        }
    }
    bool success = ferror(fp) == 0;
    if (success && fflush(fp) != 0) {
        success = false;
    }
#if defined(_WIN32)
    if (success && _commit(_fileno(fp)) != 0) {
        success = false;
    }
#else
    if (success && fsync(fileno(fp)) != 0) {
        success = false;
    }
#endif
    if (fclose(fp) != 0) {
        success = false;
    }
    if (success) {
        success = path_file_replace(staging_file, conf_file);
    }
    if (!success) {
        (void) path_file_remove(staging_file);
        commons_log_error("PCManager", "Failed to save hosts.ini atomically: %s", strerror(errno));
    }
    free(staging_file);
    free(conf_file);
}

static int known_hosts_handle(known_host_t **list, const char *section, const char *name, const char *value) {
    if (!section) { return 1; }
    known_host_t *host = known_hosts_find_by(*list, section, (known_hosts_find_fn) known_hosts_find_uuid);
    if (!host) {
        host = known_hosts_new();
        uuidstr_fromstr(&host->uuid, section);
        *list = known_hosts_append(*list, host);
    }
    if (INI_NAME_MATCH("mac")) {
        host->mac = SDL_strdup(value);
    } else if (INI_NAME_MATCH("hostname")) {
        host->hostname = SDL_strdup(value);
    } else if (INI_NAME_MATCH("address")) {
        host->address = hostport_parse(value);
    } else if (INI_NAME_MATCH("pin_code")) {
        host->pin_code = SDL_strdup(value);
    } else if (INI_NAME_MATCH("pin_is_password")) {
        host->pin_is_password = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("selected")) {
        host->selected = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("favorite")) {
        appid_list_t *id_item = appid_list_new();
        id_item->id = SDL_atoi(value);
        host->favs = appid_list_append(host->favs, id_item);
    } else if (INI_NAME_MATCH("hidden")) {
        appid_list_t *id_item = appid_list_new();
        id_item->id = SDL_atoi(value);
        host->hidden = appid_list_append(host->hidden, id_item);
    } else if (INI_NAME_MATCH("gpu")) {
        host->gpu_vendor = coop_gpu_vendor_parse(value);
    } else if (INI_NAME_MATCH("pad")) {
        padname_list_t *pad_item = padname_list_new();
        SDL_strlcpy(pad_item->name, value, sizeof(pad_item->name));
        host->pads = padname_list_append(host->pads, pad_item);
    }
    return 1;
}

static int known_hosts_find_uuid(known_host_t *node, void *v) {
    return !uuidstr_t_equals_s(&node->uuid, v);
}

void known_hosts_node_free(known_host_t *node) {
    if (node->address) {
        free(node->address);
    }
    if (node->favs) {
        appid_list_free(node->favs, (appid_list_nodefree_fn) free);
    }
    if (node->hidden) {
        appid_list_free(node->hidden, (appid_list_nodefree_fn) free);
    }
    if (node->pin_code) {
        SDL_free(node->pin_code);
    }
    if (node->pads) {
        padname_list_free(node->pads, (padname_list_nodefree_fn) free);
    }
    free(node);
}

static known_host_t *known_hosts_parse_impl(const char *conf_file, bool *save_allowed) {
    known_host_t *hosts = NULL;
    FILE *fp = path_fopen(conf_file, "rb");
    if (fp == NULL) {
        if (save_allowed != NULL) {
            *save_allowed = errno == ENOENT;
        }
        return NULL;
    }
    const int parse_result = ini_parse_file(fp, (ini_handler) known_hosts_handle, &hosts);
    const int close_result = fclose(fp);
    if (save_allowed != NULL) {
        *save_allowed = parse_result == 0 && close_result == 0;
    }
    if (parse_result != 0 || close_result != 0) {
        known_hosts_free(hosts, known_hosts_node_free);
        return NULL;
    }
    return hosts;
}

known_host_t *known_hosts_parse(const char *conf_file) {
    return known_hosts_parse_impl(conf_file, NULL);
}

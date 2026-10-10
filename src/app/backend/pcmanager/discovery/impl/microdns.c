/*
 * Copyright (c) 2024 Mariotaku <https://github.com/mariotaku>.
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include "impl.h"

#include <microdns/microdns.h>
#include "sockaddr.h"
#include "logging.h"

#include <string.h>

/* The service we query for, and the only one whose SRV port may be believed. */
#define DISCOVERY_SERVICE_NAME "_nvstream._tcp.local"
/* Standard GameStream HTTP port, used when an answer carries an address but no SRV of
 * ours to take a port from. */
#define DISCOVERY_DEFAULT_PORT 47989

static void discovery_callback(discovery_task_t *task, int status, const struct rr_entry *entries);

static bool discovery_is_stopped(discovery_task_t *task);

/* True if `name` is an instance of `service`, i.e. "<whatever>._nvstream._tcp.local",
 * with or without the trailing root dot that some responders emit. */
static bool rr_name_is_instance_of(const char *name, const char *service) {
    if (name == NULL) {
        return false;
    }
    size_t name_len = strlen(name), service_len = strlen(service);
    if (name_len > 0 && name[name_len - 1] == '.') {
        name_len--;
    }
    return name_len > service_len && name[name_len - service_len - 1] == '.' &&
           memcmp(name + name_len - service_len, service, service_len) == 0;
}

int discovery_worker(discovery_task_t *task) {
    int r;
    char err[128];
    static const char *const service_name[] = {DISCOVERY_SERVICE_NAME};

    struct mdns_ctx *ctx = NULL;
    if ((r = mdns_init(&ctx, NULL, MDNS_PORT)) < 0) {
        goto err;
    }
    commons_log_info("Discovery", "Start mDNS discovery");
    if ((r = mdns_listen(ctx, service_name, 1, RR_PTR, 10, (mdns_stop_func) discovery_is_stopped,
                         (mdns_listen_callback) discovery_callback, task)) < 0) {
        goto err;
    }
    err:
    if (r < 0) {
        mdns_strerror(r, err, sizeof(err));
        commons_log_error("Discovery", "fatal: %s", err);
    }
    if (ctx != NULL) {
        mdns_destroy(ctx);
    }
    commons_log_info("Discovery", "mDNS discovery stopped");
    return r;
}

void discovery_worker_stop(discovery_task_t *task) {
    SDL_LockMutex(task->lock);
    task->stop = true;
    SDL_UnlockMutex(task->lock);
}

void discovery_callback(discovery_task_t *task, int status, const struct rr_entry *entries) {
    char err[128];

    if (status < 0) {
        mdns_strerror(status, err, sizeof(err));
        commons_log_error("Discovery", "error: %s", err);
        return;
    }
    if (discovery_is_stopped(task)) { return; }
    sockaddr_t *addr = sockaddr_new();
    bool have_port = false;
    for (const struct rr_entry *cur = entries; cur != NULL; cur = cur->next) {
        switch (cur->type) {
            case RR_A: {
                if (addr->sa_family != AF_UNSPEC) { continue; }
                sockaddr_set_ip(addr, AF_INET, &cur->data.A.addr);
                break;
            }
            case RR_AAAA: {
                if (addr->sa_family != AF_UNSPEC) { continue; }
                // Ignore any link-local addresses
                if (IN6_IS_ADDR_LINKLOCAL(&cur->data.AAAA.addr)) { continue; }
//                sockaddr_set_ip(addr, AF_INET6, &cur->data.AAAA.addr);
                break;
            }
            case RR_SRV: {
                /* ONLY our service's SRV. This used to take the port from whichever SRV
                 * came last in the message, and a host may answer with other services'
                 * records alongside ours: on Windows that was Delivery Optimization's
                 * 7680, so from the first poll the client asked
                 * http://<host>:7680/serverinfo, got nothing, and presented a healthy
                 * Apollo as "Offline" with a Wake-on-LAN prompt -- with the real port
                 * 47989 answering 200 the whole time. Measured 2026-09-22. */
                if (!rr_name_is_instance_of(cur->name, DISCOVERY_SERVICE_NAME)) {
                    continue;
                }
                sockaddr_set_port(addr, cur->data.SRV.port);
                have_port = true;
                break;
            }
        }
    }
    if (addr->sa_family != AF_UNSPEC) {
        if (!have_port) {
            /* An answer that named an address but carried no SRV of ours. Reporting it
             * with port 0, which is what happened before the filter above existed, is
             * simply a host that can never be reached. */
            sockaddr_set_port(addr, DISCOVERY_DEFAULT_PORT);
        }
        discovery_discovered(task->discovery, addr);
    }
    sockaddr_free(addr);
}

bool discovery_is_stopped(discovery_task_t *task) {
    SDL_LockMutex(task->lock);
    bool stop = task->stop;
    SDL_UnlockMutex(task->lock);
    return stop;
}

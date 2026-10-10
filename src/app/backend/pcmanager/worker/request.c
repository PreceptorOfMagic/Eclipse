#include "worker.h"
#include "backend/pcmanager/priv.h"
#include "logging.h"
#include "hostport.h"
#include "util/bus.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static void worker_callback(worker_context_t *ctx);

worker_context_t *worker_context_new(pcmanager_t *manager, const uuidstr_t *uuid, pcmanager_callback_t callback,
                                     void *userdata) {
    worker_context_t *context = calloc(1, sizeof(worker_context_t));
    context->app = manager->app;
    context->manager = manager;
    if (uuid != NULL) {
        context->uuid = *uuid;
    }
    context->callback = callback;
    context->userdata = userdata;
    return context;
}

void worker_context_finalize(worker_context_t *context, int result) {
    context->result = result;
    /* A host refusing what the user asked for was shown in a dialog and logged nowhere: the host's reason (e.g.
     * "Bad status 599: ...") lived only in the worker (support logs, 2026-09-30: "Unable to quit game" on PC2). */
    if (context->name != NULL && result != 0 && result != ECANCELED) {
        char target[160];
        if (context->arg1 != NULL && context->uuid.data[0] == '\0') {   /* adding by address: arg1 is the hostport */
            const hostport_t *host = context->arg1;                   /* (pairing's arg1 is the PIN: never logged) */
            const uint16_t port = hostport_get_port(host);   /* 0 = none typed: the default port */
            if (port != 0) {
                snprintf(target, sizeof(target), "address %s port %u", hostport_get_hostname(host), (unsigned) port);
            } else {
                snprintf(target, sizeof(target), "address %s", hostport_get_hostname(host));
            }
        } else {
            snprintf(target, sizeof(target), "computer %.*s", (int) sizeof(context->uuid.data), context->uuid.data);
        }
        commons_log_warn("PCManager", "%s failed on %s: %s (code %d)", context->name, target,
                         context->error != NULL ? context->error : "no reason given", result);
    }
    if (result != ECANCELED) {
        app_bus_post_sync(context->app, (bus_actionfunc) worker_callback, context);
    }
    if (context->arg1 != NULL) {
        free(context->arg1);
    }
    if (context->error != NULL) {
        free(context->error);
    }
    free(context);
}

static const char *worker_name(worker_action action) {
    if (action == worker_pairing) {
        return "Pairing";
    }
    if (action == worker_quit_app) {
        return "Quitting the running app";
    }
    if (action == worker_wol) {
        return "Wake-on-LAN";
    }
    if (action == worker_add_by_host) {
        return "Adding a computer by address";
    }
    return NULL;
}

void pcmanager_worker_queue(pcmanager_t *manager, worker_action action, worker_context_t *context) {
    context->name = worker_name(action);
    executor_submit(manager->executor, (executor_action_cb) action,
                    (executor_cleanup_cb) worker_context_finalize, context);
}

static void worker_callback(worker_context_t *ctx) {
    if (ctx->callback) {
        ctx->callback(ctx->result, ctx->error, &ctx->uuid, ctx->userdata);
    }
}
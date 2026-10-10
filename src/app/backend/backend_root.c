#include "backend_root.h"

#include "pcmanager.h"
#include <SDL2/SDL_cpuinfo.h>
#include <SDL2/SDL_stdinc.h>

#include "util/bus.h"
#include "executor.h"

pcmanager_t *pcmanager;


void backend_init(app_backend_t *backend, app_t *app) {
    backend->app = app;
    backend->executor = executor_create("moonlight-io", 2 * SDL_min(3, SDL_GetCPUCount()));
    backend->gs_client_mutex = SDL_CreateMutex();
    pcmanager = pcmanager_new(app, backend->executor);
}

void backend_destroy(app_backend_t *backend) {
    /* Discovery can queue host work; stop it and drain its queued callbacks
     * before shutting down the executor. Active requests still own references
     * to the manager, its servers and the GameStream mutex until their join. */
    pcmanager_auto_discovery_stop(pcmanager);
    app_bus_drain();
    executor_destroy(backend->executor);
    pcmanager_destroy(pcmanager);
    SDL_DestroyMutex(backend->gs_client_mutex);
    backend->executor = NULL;
    backend->gs_client_mutex = NULL;
    pcmanager = NULL;
}

bool backend_dispatch_userevent(app_backend_t *backend, int which, void *data1, void *data2) {
    (void) backend;
    (void) which;
    (void) data1;
    (void) data2;
    return false;
}

#include <util/nullable.h>
#include "priv.h"


PSERVER_DATA serverdata_new() {
    PSERVER_DATA server = malloc(sizeof(SERVER_DATA));
    SDL_memset(server, 0, sizeof(SERVER_DATA));
    return server;
}

static DISPLAY_MODE *display_mode_clone(const DISPLAY_MODE *mode) {
    if (mode == NULL) return NULL;
    DISPLAY_MODE *result = SDL_malloc(sizeof(DISPLAY_MODE));
    SDL_memcpy(result, mode, sizeof(DISPLAY_MODE));
    result->next = display_mode_clone(mode->next);
    return result;
}

PSERVER_DATA serverdata_clone(const SERVER_DATA *src) {
    PSERVER_DATA server = serverdata_new();
    server->uuid = strdup_nullable(src->uuid);
    server->mac = strdup_nullable(src->mac);
    server->hostname = strdup_nullable(src->hostname);
    server->gpuType = strdup_nullable(src->gpuType);
    /* Cloned WITH its known-flag and caps, because the pair only means anything
     * together: an encoderVendor that survived the copy while encoderVendorKnown did not
     * would read as "this host answered, and said nothing". */
    server->encoderVendor = strdup_nullable(src->encoderVendor);
    server->encoderVendorKnown = src->encoderVendorKnown;
    server->coopProfileCaps = src->coopProfileCaps;
    server->extPort = src->extPort;
    server->httpsPort = src->httpsPort;
    server->paired = src->paired;
    server->supports4K = src->supports4K;
    server->supportsHdr = src->supportsHdr;
    server->unsupported = src->unsupported;
    server->currentGame = src->currentGame;
    /* The rest of what the host reported travels with it too: a session's copy that dropped these read as
     * "not reported" (umbraCaps 0, lock screen unknown) to anything that looked at the session's copy. */
    server->codecModeSupport = src->codecModeSupport;
    server->umbraCaps = src->umbraCaps;
    server->umbraCapsKnown = src->umbraCapsKnown;
    server->sessionLocked = src->sessionLocked;
    server->sessionLockedKnown = src->sessionLockedKnown;
    server->serverMajorVersion = src->serverMajorVersion;
    server->gsVersion = strdup_nullable(src->gsVersion);
    server->modes = display_mode_clone(src->modes);
    server->serverInfo.address = strdup_nullable(src->serverInfo.address);
    server->serverInfo.rtspSessionUrl = strdup_nullable(src->serverInfo.rtspSessionUrl);
    server->serverInfo.serverInfoAppVersion = strdup_nullable(src->serverInfo.serverInfoAppVersion);
    server->serverInfo.serverInfoGfeVersion = strdup_nullable(src->serverInfo.serverInfoGfeVersion);
    return server;
}

void serverdata_free(PSERVER_DATA data) {
    PDISPLAY_MODE mode = data->modes;
    while (mode) {
        PDISPLAY_MODE tmp = mode;
        mode = mode->next;
        SDL_free(tmp);
    }
    free_nullable((void *) data->uuid);
    free_nullable((void *) data->mac);
    free_nullable((void *) data->hostname);
    free_nullable((void *) data->gpuType);
    free_nullable((void *) data->encoderVendor);
    free_nullable((void *) data->gsVersion);
    free_nullable((void *) data->serverInfo.serverInfoAppVersion);
    free_nullable((void *) data->serverInfo.serverInfoGfeVersion);
    free_nullable((void *) data->serverInfo.address);
    free_nullable((void *) data->serverInfo.rtspSessionUrl);
    free(data);
}


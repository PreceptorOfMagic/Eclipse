#include "update_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>
#include <curl/curl.h>

#include "app.h"
#include "executor.h"
#include "logging.h"
#include "util/bus.h"

/* The Project Hub (GitHub Pages). ECLIPSE_UPDATE_URL overrides it, for testing a manifest before it is
 * published. */
#ifndef UPDATE_MANIFEST_URL
#define UPDATE_MANIFEST_URL "https://preceptorofmagic.github.io/Eclipse-Umbra-Project-Hub/updates.json"
#endif
#define UPDATE_PRODUCT "eclipse"
#define CHECK_INTERVAL_SEC (24 * 60 * 60)
#define RETRY_INTERVAL_MS (60 * 60 * 1000)   /* after a failed download, within one run */
#define MAX_BODY (64 * 1024)
#define CACHE_FILE "updates-cache.json"
#define STATE_FILE "update-state.ini"

typedef struct fetch_ctx_t {
    CURL *curl;
    char *body;
    size_t len;
    bool too_big;
    long status;
    CURLcode result;
    unsigned generation;
    char *ca_path;   /* a copy: app_input_deinit frees the original while a download may still run */
} fetch_ctx_t;

static struct {
    bool initialized;
    app_t *app;
    executor_t *executor;
    const executor_task_t *task;
    SDL_atomic_t closing;
    unsigned generation;
    update_info_t info;
    long long last_check;   /* unix seconds of the last SUCCESSFUL download */
    char seen[64];          /* the version whose "What's new" was opened */
    Uint32 last_failure_ticks;
    bool failed_once;
    update_check_listener listener;
    void *listener_ctx;
} uc;

const char *update_check_platform(void) {
#if TARGET_WEBOS
    return "webos";
#elif TARGET_ANDROID
    return "android";
#elif defined(OS_WINDOWS) || defined(_WIN32)
    return "windows";
#else
    return "linux";
#endif
}

static char *conf_path(const char *name) {
    const char *dir = uc.app->settings.conf_dir;
    if (dir == NULL) {
        return NULL;
    }
    size_t n = strlen(dir) + strlen(name) + 2;
    char *p = malloc(n);
    if (p != NULL) {
        snprintf(p, n, "%s/%s", dir, name);
    }
    return p;
}

static char *read_file(const char *path, size_t *len) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    char *buf = malloc(MAX_BODY + 1);
    size_t n = buf != NULL ? fread(buf, 1, MAX_BODY + 1, fp) : 0;
    fclose(fp);
    if (buf == NULL || n == 0 || n > MAX_BODY) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

/* Written beside, then renamed over: a crash mid-write never leaves half a file. */
static void write_file(const char *path, const char *data, size_t len) {
    size_t n = strlen(path) + 5;
    char *tmp = malloc(n);
    if (tmp == NULL) {
        return;
    }
    snprintf(tmp, n, "%s.tmp", path);
    FILE *fp = fopen(tmp, "wb");
    bool ok = fp != NULL && fwrite(data, 1, len, fp) == len;
    if (fp != NULL && fclose(fp) != 0) {
        ok = false;
    }
    if (ok) {
        remove(path);   /* Windows rename does not replace */
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
        commons_log_warn("Update", "Could not save %s", path);
    }
    free(tmp);
}

static void load_state(void) {
    char *path = conf_path(STATE_FILE);
    size_t len = 0;
    char *text = path != NULL ? read_file(path, &len) : NULL;
    free(path);
    if (text == NULL) {
        return;
    }
    for (char *line = strtok(text, "\r\n"); line != NULL; line = strtok(NULL, "\r\n")) {
        if (strncmp(line, "last_check=", 11) == 0) {
            uc.last_check = strtoll(line + 11, NULL, 10);
        } else if (strncmp(line, "seen=", 5) == 0) {
            snprintf(uc.seen, sizeof(uc.seen), "%s", line + 5);
        }
    }
    free(text);
}

static void save_state(void) {
    char *path = conf_path(STATE_FILE);
    if (path == NULL) {
        return;
    }
    char text[160];
    int n = snprintf(text, sizeof(text), "last_check=%lld\nseen=%s\n", uc.last_check, uc.seen);
    if (n > 0 && (size_t) n < sizeof(text)) {
        write_file(path, text, (size_t) n);
    }
    free(path);
}

/* Replaces the current result if the document is usable; false leaves it as it was. */
static bool evaluate(const char *json, size_t len) {
    update_info_t next;
    if (!update_manifest_evaluate(json, len, UPDATE_PRODUCT, update_check_platform(), APP_VERSION, &next)) {
        return false;
    }
    update_info_clear(&uc.info);
    uc.info = next;
    return true;
}

static void notify_listener(void) {
    if (uc.listener != NULL) {
        uc.listener(uc.listener_ctx);
    }
}

void update_check_init(app_t *app) {
    if (uc.initialized) {
        return;
    }
    memset(&uc, 0, sizeof(uc));
    uc.app = app;
    uc.executor = app->backend.executor;
    uc.initialized = true;
    load_state();
    char *path = conf_path(CACHE_FILE);
    size_t len = 0;
    char *cached = path != NULL ? read_file(path, &len) : NULL;
    free(path);
    if (cached != NULL) {
        evaluate(cached, len);
        free(cached);
    }
    if (uc.info.available) {
        commons_log_info("Update", "Eclipse %s is available (saved check)", uc.info.latest_version);
    }
}

static size_t body_cb(void *data, size_t size, size_t nmemb, void *userp) {
    fetch_ctx_t *ctx = userp;
    size_t n = size * nmemb;
    if (ctx->len + n > MAX_BODY) {
        ctx->too_big = true;
        return 0;   /* aborts the transfer */
    }
    char *grown = realloc(ctx->body, ctx->len + n + 1);
    if (grown == NULL) {
        return 0;
    }
    ctx->body = grown;
    memcpy(ctx->body + ctx->len, data, n);
    ctx->len += n;
    ctx->body[ctx->len] = '\0';
    return n;
}

/* Lets app shutdown abort a download instead of waiting for its timeout. */
static int progress_cb(void *p, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    (void) p, (void) dt, (void) dn, (void) ut, (void) un;
    return SDL_AtomicGet(&uc.closing) ? 1 : 0;
}

static int fetch_task(void *arg) {
    fetch_ctx_t *ctx = arg;
    const char *url = SDL_getenv("ECLIPSE_UPDATE_URL");
    if (url == NULL || url[0] == '\0') {
        url = UPDATE_MANIFEST_URL;
    }
    CURL *curl = ctx->curl;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Eclipse/" APP_VERSION);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    if (ctx->ca_path != NULL) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ctx->ca_path);
    }
    ctx->result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &ctx->status);
    return 0;
}

static void fetch_ctx_free(fetch_ctx_t *ctx) {
    if (ctx->curl != NULL) {
        curl_easy_cleanup(ctx->curl);
    }
    free(ctx->body);
    free(ctx->ca_path);
    free(ctx);
}

/* UI thread. */
static void fetch_apply(void *arg) {
    fetch_ctx_t *ctx = arg;
    if (!uc.initialized || ctx->generation != uc.generation) {
        fetch_ctx_free(ctx);
        return;
    }
    uc.task = NULL;
    bool ok = ctx->result == CURLE_OK && ctx->status == 200 && !ctx->too_big && ctx->body != NULL &&
              evaluate(ctx->body, ctx->len);
    if (ok) {
        char *path = conf_path(CACHE_FILE);
        if (path != NULL) {
            write_file(path, ctx->body, ctx->len);
            free(path);
        }
        uc.last_check = (long long) time(NULL);
        uc.failed_once = false;
        save_state();
        if (uc.info.available) {
            commons_log_info("Update", "Eclipse %s is available", uc.info.latest_version);
        } else {
            commons_log_info("Update", "Up to date (%s)", APP_VERSION);
        }
        notify_listener();
    } else {
        uc.failed_once = true;
        uc.last_failure_ticks = SDL_GetTicks();
        commons_log_info("Update", "Check failed (curl %d, HTTP %ld%s); will retry later", (int) ctx->result,
                         ctx->status, ctx->too_big ? ", too large" : "");
    }
    fetch_ctx_free(ctx);
}

/* Worker thread. */
static void fetch_finalizer(void *arg, int result) {
    (void) result;
    fetch_ctx_t *ctx = arg;
    if (SDL_AtomicGet(&uc.closing) || !app_bus_post(uc.app, fetch_apply, ctx)) {
        fetch_ctx_free(ctx);
    }
}

void update_check_maybe_start(void) {
    if (!uc.initialized || uc.executor == NULL || !uc.app->settings.check_updates || uc.task != NULL) {
        return;
    }
    long long now = (long long) time(NULL);
    if (uc.last_check > 0 && uc.last_check <= now && now - uc.last_check < CHECK_INTERVAL_SEC) {
        return;
    }
    if (uc.failed_once && SDL_GetTicks() - uc.last_failure_ticks < RETRY_INTERVAL_MS) {
        return;
    }
    fetch_ctx_t *ctx = calloc(1, sizeof(fetch_ctx_t));
    if (ctx == NULL) {
        return;
    }
    ctx->curl = curl_easy_init();
    ctx->generation = ++uc.generation;
    if (ctx->curl == NULL) {
        free(ctx);
        return;
    }
    /* The CA bundle the controller-database download uses (NULL on webOS: curl's default). */
    const char *ca = uc.app->input.gcdb_ca_path;
    if (ca != NULL) {
        ctx->ca_path = strdup(ca);
    }
    uc.task = executor_submit(uc.executor, fetch_task, fetch_finalizer, ctx);
    if (uc.task == NULL) {
        fetch_ctx_free(ctx);
    }
}

void update_check_deinit(void) {
    if (!uc.initialized) {
        return;
    }
    SDL_AtomicSet(&uc.closing, 1);
    if (uc.task != NULL) {
        executor_cancel(uc.executor, uc.task);
        uc.task = NULL;
    }
    update_info_clear(&uc.info);
    uc.listener = NULL;
    uc.initialized = false;
    /* uc.closing stays set: a download still finishing frees its own context in the finalizer. */
}

const update_info_t *update_check_info(void) {
    return &uc.info;
}

bool update_check_dot_visible(void) {
    /* "Check for updates" off hides a notice already found, not just the next download. */
    return uc.initialized && uc.app->settings.check_updates && uc.info.available &&
           uc.info.latest_version != NULL && strcmp(uc.seen, uc.info.latest_version) != 0;
}

void update_check_mark_seen(void) {
    if (!update_check_dot_visible()) {
        return;
    }
    snprintf(uc.seen, sizeof(uc.seen), "%s", uc.info.latest_version);
    save_state();
    notify_listener();
}

void update_check_set_listener(update_check_listener listener, void *ctx) {
    uc.listener = listener;
    uc.listener_ctx = ctx;
}

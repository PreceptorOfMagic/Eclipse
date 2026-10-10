/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "http.h"
#include "errors.h"
#include "set_error.h"
#include "logging.h"
#include "file_compat.h"

#include <string.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <stdbool.h>

struct HTTP_T {
    CURL *curl;
    pthread_mutex_t mutex;
};

static bool http_set_credentials(CURL *curl, const char *keydir) {
    char *certificate_path = gs_path_join(keydir, CERTIFICATE_FILE_NAME);
    char *key_path = gs_path_join(keydir, KEY_FILE_NAME);
    if (certificate_path == NULL || key_path == NULL) {
        free(certificate_path);
        free(key_path);
        gs_set_error(GS_IO_ERROR, "Failed to allocate credential paths");
        return false;
    }

#if LIBCURL_VERSION_NUM >= 0x074700
    unsigned char *certificate_data = NULL;
    unsigned char *key_data = NULL;
    size_t certificate_size = 0;
    size_t key_size = 0;
    if (gs_read_file_utf8(certificate_path, &certificate_data, &certificate_size) != 0 ||
        gs_read_private_file_utf8(key_path, &key_data, &key_size) != 0) {
        free(certificate_data);
        free(key_data);
        free(certificate_path);
        free(key_path);
        gs_set_error(GS_IO_ERROR, "Failed to read TLS credentials: %s", strerror(errno));
        return false;
    }
    struct curl_blob certificate_blob = {
            .data = certificate_data,
            .len = certificate_size,
            .flags = CURL_BLOB_COPY,
    };
    struct curl_blob key_blob = {
            .data = key_data,
            .len = key_size,
            .flags = CURL_BLOB_COPY,
    };
    const CURLcode cert_result = curl_easy_setopt(curl, CURLOPT_SSLCERT_BLOB, &certificate_blob);
    const CURLcode key_result = curl_easy_setopt(curl, CURLOPT_SSLKEY_BLOB, &key_blob);
    free(certificate_data);
    free(key_data);
    free(certificate_path);
    free(key_path);
    if (cert_result != CURLE_OK || key_result != CURLE_OK) {
        gs_set_error(GS_IO_ERROR, "cURL TLS backend rejected in-memory credentials");
        return false;
    }
#else
#if defined(_WIN32)
    /* The blob options are what make non-ACP profile paths portable on Windows.
     * Supported Windows packages are newer than 7.71; fail closed rather than
     * silently making pairing work only for some user names. */
    free(certificate_path);
    free(key_path);
    gs_set_error(GS_IO_ERROR, "libcurl 7.71 or newer is required on Windows");
    return false;
#else
    const CURLcode cert_result = curl_easy_setopt(curl, CURLOPT_SSLCERT, certificate_path);
    const CURLcode key_result = curl_easy_setopt(curl, CURLOPT_SSLKEY, key_path);
    free(certificate_path);
    free(key_path);
    if (cert_result != CURLE_OK || key_result != CURLE_OK) {
        gs_set_error(GS_IO_ERROR, "cURL TLS backend rejected credential paths");
        return false;
    }
#endif
#endif
    return true;
}

static size_t write_fn(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    HTTP_DATA *mem = (HTTP_DATA *) userp;

    void *allocated = realloc(mem->memory, mem->size + realsize + 1);
    assert(allocated != NULL);
    mem->memory = allocated;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

HTTP *http_create(const char *keydir) {
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        gs_set_error(GS_ERROR, "Failed to create cURL instance");
        return NULL;
    }

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_SSLENGINE_DEFAULT, 1L);
    curl_easy_setopt(curl, CURLOPT_SSLCERTTYPE, "PEM");
    curl_easy_setopt(curl, CURLOPT_SSLKEYTYPE, "PEM");
    if (!http_set_credentials(curl, keydir)) {
        curl_easy_cleanup(curl);
        return NULL;
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_fn);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_SESSIONID_CACHE, 0L);
    // Fix for https://github.com/mariotaku/moonlight-tv/issues/452
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);

    struct HTTP_T *http = malloc(sizeof(struct HTTP_T));
    assert(http != NULL);
    http->curl = curl;
    pthread_mutex_init(&http->mutex, NULL);
    return http;
}

int http_request(HTTP *http, char *url, HTTP_DATA *data) {
    assert(http != NULL);
    assert(data != NULL);
    if (data->size > 0) {
        void *allocated = realloc(data->memory, 1);
        assert(allocated != NULL);
        data->memory = allocated;
        data->memory[0] = 0;
        data->size = 0;
    }
    pthread_mutex_lock(&http->mutex);
    CURL *curl = http->curl;
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, data);
    curl_easy_setopt(curl, CURLOPT_URL, url);

    commons_log_debug("GameStream", "Request %p %s", data, url);

    int ret = GS_FAILED;
    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_HTTP_RETURNED_ERROR) {
        /* CURLINFO_RESPONSE_CODE stores a LONG. An int here is a 4-byte slot for an 8-byte write on
         * 64-bit Linux -- the stack protector aborted the first Linux desktop build in exactly this
         * function (2026-09-29). Windows (LLP64) and the TV (32-bit ARM) only fit by luck. */
        long http_status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
        commons_log_warn("GameStream", "Request %p error HTTP %ld", data, http_status);
        goto finish;
    } else if (res != CURLE_OK) {
        const char *errmsg = curl_easy_strerror(res);
        ret = gs_set_error(GS_IO_ERROR, "cURL error: %s", errmsg);
        commons_log_debug("GameStream", "Request %p error %d: %s", data, ret, errmsg);
        goto finish;
    }
    assert (data->memory != NULL);
    long http_code = 0;   /* a long, for the reason above */
    curl_easy_getinfo(curl, CURLINFO_HTTP_CODE, &http_code);
    commons_log_debug("GameStream", "Request %p response %ld", data, http_code);
    commons_log_hexdump(COMMONS_LOG_LEVEL_VERBOSE, "GameStream", data->memory, data->size);

    ret = GS_OK;
    finish:
    pthread_mutex_unlock(&http->mutex);
    return ret;
}

void http_destroy(HTTP *http) {
    assert(http != NULL);
    pthread_mutex_lock(&http->mutex);
    curl_easy_cleanup(http->curl);
    pthread_mutex_unlock(&http->mutex);
    pthread_mutex_destroy(&http->mutex);
    free((void *) http);
}

void http_set_timeout(HTTP *http, int timeout) {
    assert(http != NULL);
    pthread_mutex_lock(&http->mutex);
    curl_easy_setopt(http->curl, CURLOPT_CONNECTTIMEOUT, timeout);
    /* CONNECTTIMEOUT BOUNDS THE CONNECT ONLY. Once the socket is up, curl waits
     * FOREVER for a reply, and that is how a stream got stuck "Disconnecting..."
     * with no way out but powering the TV off: gs_quit_app() POSTs /cancel to a host
     * that is already tearing the session down, the connection succeeds, no response
     * ever comes, and session_worker blocks in it before LiStopConnection() with the
     * session state left at STREAMING_DISCONNECTING. Measured 2026-08-07 in
     * /tmp/aurora-diag.log: "[QUIT@70288] primary quit ->" with no matching "rc=" --
     * the call never returned, and the next line in the log is the next app run.
     *
     * LOW_SPEED rather than CURLOPT_TIMEOUT, deliberately: this handle also fetches
     * box art and app lists, and a hard total cap would abort a large download that
     * is merely slow. This aborts only a transfer that has STALLED -- under one byte
     * a second for the whole window -- which is the actual failure and never a slow
     * but progressing one. */
    curl_easy_setopt(http->curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(http->curl, CURLOPT_LOW_SPEED_TIME, (long) timeout);
    pthread_mutex_unlock(&http->mutex);
}

HTTP_DATA *http_data_alloc() {
    HTTP_DATA *data = malloc(sizeof(HTTP_DATA));
    assert(data != NULL);

    data->memory = malloc(1);
    assert(data->memory != NULL);
    data->size = 0;

    return data;
}

void http_data_free(HTTP_DATA *data) {
    if (data == NULL) {
        return;
    }
    if (data->memory != NULL) {
        free(data->memory);
    }

    free(data);
}

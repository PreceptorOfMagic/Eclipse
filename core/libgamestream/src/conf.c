#include "conf.h"
#include "client.h"
#include "errors.h"
#include "set_error.h"
#include "priv.h"
#include "mkcert.h"
#include "http.h"
#include "logging.h"
#include "file_compat.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <mbedtls/version.h>
#include <mbedtls/pk.h>
#include <mbedtls/error.h>

#if MBEDTLS_VERSION_NUMBER >= 0x03020100

#include <mbedtls/ctr_drbg.h>

#endif

static int mbed_parse_key(mbedtls_pk_context *ctx, const unsigned char *data, size_t length);

static int load_unique_id(struct GS_CLIENT_T *hnd, const char *keydir);

static int init_unique_id(const char *keydir);

static int load_cert(struct GS_CLIENT_T *hnd, const char *keydir);

static int init_cert(const char *keydir);

int gs_conf_load(GS_CLIENT hnd, const char *keydir) {
    int ret = GS_OK;
    if ((ret = load_unique_id(hnd, keydir)) != GS_OK) {
        return ret;
    }

    if ((ret = load_cert(hnd, keydir)) != GS_OK) {
        return ret;
    }
    return ret;
}

int gs_conf_init(const char *keydir) {
    commons_log_info("GameStream", "Initializing configuration");
    if (gs_mkdirs_utf8(keydir) != 0) {
        return gs_set_error(GS_IO_ERROR, "Failed to create config directory %s: %s", keydir, strerror(errno));
    }
    int ret;
    /* The unique ID is written last: it marks a finished set-up. A first start stopped while the key was being
     * made (it can take seconds on a TV) then leaves no unique ID, and the next start makes everything again. */
    if ((ret = init_cert(keydir)) != GS_OK) {
        return ret;
    }
    if ((ret = init_unique_id(keydir)) != GS_OK) {
        return ret;
    }
    commons_log_info("GameStream", "Configuration initialized");
    return ret;
}

static int mbed_parse_key(mbedtls_pk_context *ctx, const unsigned char *data, size_t length) {
#if MBEDTLS_VERSION_NUMBER >= 0x03020100
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ctr_drbg_init(&ctr_drbg);
    int ret = mbedtls_pk_parse_key(ctx, data, length, NULL, 0,
                                   mbedtls_ctr_drbg_random, &ctr_drbg);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    return ret;
#else
    return mbedtls_pk_parse_key(ctx, data, length, NULL, 0);
#endif
}

int load_unique_id(struct GS_CLIENT_T *hnd, const char *keydir) {
    char *id_path = gs_path_join(keydir, UNIQUE_FILE_NAME);
    if (id_path == NULL) {
        return gs_set_error(GS_IO_ERROR, "Failed to allocate unique ID path");
    }

    FILE *fd = gs_fopen_utf8(id_path, "rb");
    if (fd == NULL) {
        int ret = gs_set_error(GS_BAD_CONF, "Failed to open unique ID file %s: %s", id_path, strerror(errno));
        free(id_path);
        return ret;
    }
    free(id_path);
    if (fread(hnd->unique_id, 1, UNIQUEID_CHARS, fd) != UNIQUEID_CHARS) {
        fclose(fd);
        return gs_set_error(GS_BAD_CONF, "Bad unique ID file");
    }
    fclose(fd);
    hnd->unique_id[UNIQUEID_CHARS] = 0;
    return GS_OK;
}

int init_unique_id(const char *keydir) {
    commons_log_info("GameStream", "Generating device unique ID");
    char *id_path = gs_path_join(keydir, UNIQUE_FILE_NAME);
    if (id_path == NULL) {
        return gs_set_error(GS_IO_ERROR, "Failed to allocate unique ID path");
    }
    FILE *fd = gs_fopen_utf8(id_path, "wb");
    if (fd == NULL) {
        int ret = gs_set_error(GS_BAD_CONF, "Failed to create unique ID file %s: %s", id_path, strerror(errno));
        free(id_path);
        return ret;
    }
    free(id_path);
    if (fwrite(UNIQUE_ID_DEFAULT, 1, UNIQUEID_CHARS, fd) != UNIQUEID_CHARS) {
        int ret = gs_set_error(GS_BAD_CONF, "Failed to write unique ID file: %s", strerror(errno));
        fclose(fd);
        return ret;
    }
    fclose(fd);
    return GS_OK;
}

int load_cert(struct GS_CLIENT_T *hnd, const char *keydir) {
    char *cert_path = gs_path_join(keydir, CERTIFICATE_FILE_NAME);
    char *key_path = gs_path_join(keydir, KEY_FILE_NAME);
    unsigned char *cert_data = NULL;
    unsigned char *key_data = NULL;
    size_t cert_size = 0;
    size_t key_size = 0;
    if (cert_path == NULL || key_path == NULL) {
        free(cert_path);
        free(key_path);
        return gs_set_error(GS_IO_ERROR, "Failed to allocate certificate paths");
    }
    if (gs_read_file_utf8(cert_path, &cert_data, &cert_size) != 0) {
        /* Missing (a set-up that never finished, before the unique ID was written last): not set up, so it is made
         * again. A certificate that exists but cannot be read is never replaced. */
        const int missing = errno == ENOENT;
        int error = gs_set_error(missing ? GS_BAD_CONF : GS_IO_ERROR, "Failed to open certFile %s for reading: %s",
                                 cert_path, strerror(errno));
        free(cert_path);
        free(key_path);
        return error;
    }
    if (gs_read_private_file_utf8(key_path, &key_data, &key_size) != 0) {
        const int missing = errno == ENOENT;
        int error = gs_set_error(missing ? GS_BAD_CONF : GS_IO_ERROR,
                                 "Cannot safely read private key %s: %s (requires an owned regular file, no links)",
                                 key_path, strerror(errno));
        free(cert_data);
        free(cert_path);
        free(key_path);
        return error;
    }
    free(cert_path);
    free(key_path);

    int ret;
    mbedtls_x509_crt_init(&hnd->cert);
    if ((ret = mbedtls_x509_crt_parse(&hnd->cert, cert_data, cert_size + 1)) != 0) {
        char buf[512];
        mbedtls_strerror(ret, buf, 512);
        mbedtls_x509_crt_free(&hnd->cert);
        free(cert_data);
        free(key_data);
        return gs_set_error(GS_FAILED, "Failed to parse certificate: %s", buf);
    }
    if (cert_size > (sizeof(hnd->cert_hex) - 1) / 2) {
        mbedtls_x509_crt_free(&hnd->cert);
        free(cert_data);
        free(key_data);
        return gs_set_error(GS_BAD_CONF, "Certificate file is too large");
    }
    for (size_t i = 0; i < cert_size; ++i) {
        snprintf(&hnd->cert_hex[i * 2], 3, "%02x", cert_data[i]);
    }
    hnd->cert_hex[cert_size * 2] = 0;
    free(cert_data);

    mbedtls_pk_init(&hnd->pk);
    ret = mbed_parse_key(&hnd->pk, key_data, key_size + 1);
    free(key_data);
    if (ret != 0) {
        char buf[512];
        mbedtls_strerror(ret, buf, 512);
        mbedtls_x509_crt_free(&hnd->cert);
        mbedtls_pk_free(&hnd->pk);
        return gs_set_error(GS_FAILED, "Error loading key into memory: %s", buf);
    }

    return GS_OK;
}

static int init_cert(const char *keydir) {
    commons_log_info("GameStream", "Generating device cert/key pair");
    char *cert_path = gs_path_join(keydir, CERTIFICATE_FILE_NAME);
    char *key_path = gs_path_join(keydir, KEY_FILE_NAME);
    if (cert_path == NULL || key_path == NULL) {
        free(cert_path);
        free(key_path);
        return gs_set_error(GS_IO_ERROR, "Failed to allocate certificate paths");
    }
    const int result = mkcert_generate(cert_path, key_path);
    free(cert_path);
    free(key_path);
    return result;
}

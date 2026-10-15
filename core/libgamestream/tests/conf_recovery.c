/* The client's identity (uniqueid.dat, client.pem, key.pem) when the first start that made it did not finish.
 *
 * On the SHIELD TV the first start was stopped while it generated the key (its entropy read was blocking): the key
 * folder kept uniqueid.dat but no certificate, and every later start ended in "Failed to initialize client ...
 * reinstall the app", because only a missing unique ID made the app generate the identity again (2026-10-15).
 *   - A folder with the unique ID but no certificate or key reads as not set up (GS_BAD_CONF), so it is made again.
 *   - Setting up writes the unique ID last: a set-up that fails (or is stopped) before the certificate is written
 *     leaves no unique ID behind. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "client.h"
#include "errors.h"
#include "http.h"

static void path_in(char *out, size_t size, const char *dir, const char *name) {
    snprintf(out, size, "%s/%s", dir, name);
}

int main(void) {
    char base[] = "/tmp/gs-conf-recovery-XXXXXX";
    assert(mkdtemp(base) != NULL);
    char dir[512], path[600];

    /* 1. Unique ID only, as an interrupted first start leaves it. */
    snprintf(dir, sizeof(dir), "%s/interrupted", base);
    assert(mkdir(dir, 0700) == 0);
    path_in(path, sizeof(path), dir, "uniqueid.dat");
    FILE *f = fopen(path, "wb");
    assert(f != NULL && fwrite("0123456789ABCDEF", 1, 16, f) == 16 && fclose(f) == 0);
    GS_CLIENT client = gs_new(dir);
    assert(client == NULL);
    const char *message = NULL;
    const int error = gs_get_error(&message);
    printf("unique ID without a certificate: error %d (%s)\n", error, message != NULL ? message : "");
    assert(error == GS_BAD_CONF);
    assert(gs_conf_init(dir) == GS_OK);
    client = gs_new(dir);
    assert(client != NULL);
    gs_destroy(client);

    /* 2. A set-up that cannot write its certificate leaves no unique ID. */
    snprintf(dir, sizeof(dir), "%s/unwritable", base);
    assert(mkdir(dir, 0700) == 0);
    path_in(path, sizeof(path), dir, CERTIFICATE_FILE_NAME);
    assert(mkdir(path, 0700) == 0);   /* a directory where the certificate goes */
    assert(gs_conf_init(dir) != GS_OK);
    path_in(path, sizeof(path), dir, "uniqueid.dat");
    assert(access(path, F_OK) != 0);

    /* 3. A certificate that exists but cannot be used is still refused, never replaced. */
    snprintf(dir, sizeof(dir), "%s/corrupt", base);
    assert(mkdir(dir, 0700) == 0);
    assert(gs_conf_init(dir) == GS_OK);
    path_in(path, sizeof(path), dir, CERTIFICATE_FILE_NAME);
    f = fopen(path, "wb");
    assert(f != NULL && fputs("not a certificate\n", f) >= 0 && fclose(f) == 0);
    assert(gs_new(dir) == NULL);
    assert(gs_get_error(NULL) != GS_BAD_CONF);

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
    assert(system(cmd) == 0);
    printf("PASS\n");
    return 0;
}

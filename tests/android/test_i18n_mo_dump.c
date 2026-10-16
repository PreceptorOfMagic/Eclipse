/*
 * Host driver for the Android catalogue reader (src/app/platform/android/i18n_android.c): for each hex-encoded
 * msgid on stdin, prints the hex-encoded translation. test_i18n_mo.py compares every string of every catalogue
 * against Python's gettext, so the Android client shows exactly what the desktop builds show.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *locstr(const char *msgid);
void i18n_setlocale(const char *locale);

/* The reader's only platform dependency, as path_android.c would answer it. */
char *path_assets(void) {
    const char *a = getenv("TEST_ASSETS");
    return a != NULL ? strdup(a) : NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return 2;
    }
    i18n_setlocale(argv[1]);
    static char line[16384], id[8192];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        size_t n = 0;
        for (size_t i = 0; line[i] != '\0' && line[i + 1] != '\0' && n + 1 < sizeof(id); i += 2) {
            unsigned v;
            sscanf(line + i, "%2x", &v);
            id[n++] = (char) v;
        }
        id[n] = '\0';
        for (const unsigned char *p = (const unsigned char *) locstr(id); *p != '\0'; ++p) {
            printf("%02x", *p);
        }
        printf("\n");
    }
    return 0;
}

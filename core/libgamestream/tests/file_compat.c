#include "file_compat.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    /* Escapes keep the source encoding irrelevant while forcing the Windows
     * path boundary to handle characters outside common ANSI code pages. */
    const char *directory = "aurora-file-\xE9\x9B\xAA-\xF0\x9F\x8C\x99";
    assert(gs_mkdirs_utf8(directory) == 0);

    char *path = gs_path_join(directory, "credential.pem");
    assert(path != NULL);
    FILE *file = gs_fopen_utf8(path, "wb");
    assert(file != NULL);
    static const unsigned char expected[] = "portable-credential\n";
    assert(fwrite(expected, 1, sizeof(expected) - 1, file) == sizeof(expected) - 1);
    assert(fclose(file) == 0);

    unsigned char *actual = NULL;
    size_t actual_size = 0;
    assert(gs_read_file_utf8(path, &actual, &actual_size) == 0);
    assert(actual_size == sizeof(expected) - 1);
    assert(memcmp(actual, expected, actual_size) == 0);
    assert(actual[actual_size] == '\0');

    free(actual);
    free(path);
    return 0;
}

/* commons' Android UUIDs (util/uuidstr/uuidstr_android.c): version 4, RFC 4122 variant, lowercase, unique. */
#include "uuidstr.h"

#include <regex.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    regex_t re;
    regcomp(&re, "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$", REG_EXTENDED | REG_NOSUB);
    static char seen[2000][UUIDSTR_CAPACITY];
    int bad = 0;
    for (int i = 0; i < 2000; i++) {
        uuidstr_t u;
        if (!uuidstr_random(&u) || u.zero != 0 || regexec(&re, u.data, 0, NULL, 0) != 0) {
            printf("bad uuid %.*s\n", UUIDSTR_LENGTH, u.data);
            bad++;
        }
        memcpy(seen[i], u.data, UUIDSTR_CAPACITY);
        for (int j = 0; j < i; j++) {
            if (strcmp(seen[i], seen[j]) == 0) {
                bad++;
            }
        }
    }
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad != 0;
}

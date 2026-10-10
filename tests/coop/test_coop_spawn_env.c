/* Verify that secondary launch arguments are a per-spawn snapshot, not stale parent state. */
#include "coop_coord.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RESULT_PATH "/tmp/test_coop_spawn_env.result"

static int failures = 0;

static void check(int condition, const char *description) {
    printf("  [%s] %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) { failures++; }
}

static int record_child_environment(const char *path) {
    /* Make the parent enter reap before this orderly exit.  A reap path that sends SIGTERM
     * immediately kills the helper before it can record anything; the cooperative grace wins. */
    usleep(100000);
    FILE *out = fopen(path, "w");
    if (out == NULL) { return 3; }
    const char *host = getenv("COOP_HOST_UUID");
    const char *app = getenv("COOP_APP_ID");
    const char *width = getenv("COOP_STREAM_WIDTH");
    const char *height = getenv("COOP_STREAM_HEIGHT");
    fprintf(out, "%s|%s|%s|%s\n", host != NULL ? host : "<unset>",
            app != NULL ? app : "<unset>", width != NULL ? width : "<unset>",
            height != NULL ? height : "<unset>");
    fclose(out);
    return 0;
}

static bool read_result(char *out, size_t cap) {
    FILE *input = fopen(RESULT_PATH, "r");
    if (input == NULL) { return false; }
    const bool ok = fgets(out, (int) cap, input) != NULL;
    fclose(input);
    return ok;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--coop-secondary") == 0) {
        return record_child_environment(argv[2]);
    }

    char self[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1u);
    if (n <= 0 || (size_t) n >= sizeof(self) - 1u) {
        fprintf(stderr, "cannot resolve test executable\n");
        return 2;
    }
    self[n] = '\0';

    unlink(RESULT_PATH);
    setenv("COOP_HOST_UUID", "stale-host", 1);
    setenv("COOP_APP_ID", "999", 1);
    setenv("COOP_STREAM_WIDTH", "640", 1);
    setenv("COOP_STREAM_HEIGHT", "480", 1);
    check(coop_coord_spawn_secondary_for(self, RESULT_PATH, NULL, 0, 2560, 1440) > 0,
          "spawn with deliberately missing host/app");
    check(getenv("COOP_HOST_UUID") == NULL && getenv("COOP_APP_ID") == NULL &&
          getenv("COOP_STREAM_WIDTH") == NULL && getenv("COOP_STREAM_HEIGHT") == NULL,
          "launch-only variables are cleared from the parent after spawn");
    coop_coord_reap_secondary();
    char result[128] = {0};
    check(read_result(result, sizeof(result)), "missing-config child records its environment");
    check(strcmp(result, "<unset>|<unset>|2560|1440\n") == 0,
          "child receives current geometry without stale host/app values");

    unlink(RESULT_PATH);
    check(coop_coord_spawn_secondary_for(self, RESULT_PATH, "fresh-host", 42, 3440, 1440) > 0,
          "spawn with a fresh host/app snapshot");
    check(getenv("COOP_HOST_UUID") == NULL && getenv("COOP_APP_ID") == NULL &&
          getenv("COOP_STREAM_WIDTH") == NULL && getenv("COOP_STREAM_HEIGHT") == NULL,
          "fresh launch variables are also cleared from the parent");
    coop_coord_reap_secondary();
    memset(result, 0, sizeof(result));
    check(read_result(result, sizeof(result)), "configured child records its environment");
    check(strcmp(result, "fresh-host|42|3440|1440\n") == 0,
          "configured child receives exactly the current launch snapshot");

    unlink(RESULT_PATH);
    printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

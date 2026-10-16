/*
 * Android's paths (platform/android/path_android.c) and launch arguments (app_android.c), on the host. The paths
 * come from the environment EclipseActivity publishes, so the co-op peer resolves the same ones; the launch
 * arguments are the webOS launch payload's three fields, which `adb shell am start ... --es host_uuid` uses.
 */
#include "android_platform.h"
#include "app.h"
#include "app_launch.h"
#include "util/path.h"
#include "util/i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- what app_android.c reaches outside itself ---------------------------------------------------------- */
PCONFIGURATION app_configuration;
static bool coop_requested_set, coop_requested_value;
static const char *secondary_socket;

void app_launch_set_coop_requested(bool requested) {
    coop_requested_set = true;
    coop_requested_value = requested;
}
const char *coop_secondary_mode(void) { return secondary_socket; }
const char *android_device_name(void) { return "Test TV"; }
int android_sdk_int(void) { return 30; }
void i18n_setlocale(const char *locale) { (void) locale; }
const i18n_entry_t *i18n_entry(const char *locale) { (void) locale; return NULL; }
bool i18n_is_loaded(void) { return false; }

/* --- the tests ---------------------------------------------------------------------------------------------- */
static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static bool is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void test_paths(const char *root) {
    char files[512], cache[512], assets[512], native[512], expect[600];
    snprintf(files, sizeof(files), "%s/files", root);
    snprintf(cache, sizeof(cache), "%s/cache", root);
    snprintf(assets, sizeof(assets), "%s/files/assets-10304", root);
    snprintf(native, sizeof(native), "%s/lib", root);
    mkdir(cache, 0700);
    mkdir(native, 0700);
    setenv(ANDROID_ENV_FILES_DIR, files, 1);
    setenv(ANDROID_ENV_CACHE_DIR, cache, 1);
    setenv(ANDROID_ENV_ASSETS_DIR, assets, 1);
    setenv(ANDROID_ENV_NATIVE_DIR, native, 1);

    /* Settings and pairing: the app's files directory, made if missing, persistent. */
    bool persistent = false;
    char *pref = path_pref(&persistent);
    CHECK(pref != NULL && strcmp(pref, files) == 0 && persistent && is_dir(files));
    free(pref);

    /* Cover art and co-op runtime files: a leaf of the cache directory, never its root. */
    char *app_cache = path_cache();
    snprintf(expect, sizeof(expect), "%s/moonlight-tv", cache);
    CHECK(app_cache != NULL && strcmp(app_cache, expect) == 0 && is_dir(expect));
    char *runtime = path_runtime_cache();
    CHECK(runtime != NULL && strcmp(runtime, expect) == 0);
    free(app_cache);
    free(runtime);

    char *assets_dir = path_assets();
    CHECK(assets_dir != NULL && strcmp(assets_dir, assets) == 0);
    free(assets_dir);

    /* The kept logs and co-op traces: in the files directory, where Share logs finds them. */
    snprintf(expect, sizeof(expect), "%s/aurora-diag.log", files);
    CHECK(strcmp(path_diag_log(), expect) == 0);
    snprintf(expect, sizeof(expect), "%s/coop_secondary.log", files);
    CHECK(strcmp(path_secondary_log(), expect) == 0);

    /* The co-op peer: the packaged executable beside the app's libraries, only once it can be executed. */
    char exe[512];
    snprintf(expect, sizeof(expect), "%s/%s", native, ANDROID_PEER_EXECUTABLE);
    CHECK(!path_self_executable(exe, sizeof(exe)));               /* not there yet */
    FILE *f = fopen(expect, "w");
    CHECK(f != NULL);
    if (f != NULL) fclose(f);
    chmod(expect, 0644);
    CHECK(!path_self_executable(exe, sizeof(exe)));               /* there, not executable */
    chmod(expect, 0755);
    CHECK(path_self_executable(exe, sizeof(exe)) && strcmp(exe, expect) == 0);
    CHECK(!path_self_executable(exe, 8));                         /* never a truncated path */
    char *alloc = path_self_executable_alloc();
    CHECK(alloc != NULL && strcmp(alloc, expect) == 0);
    free(alloc);

    /* Started by anything but the activity: no relative or missing directories are guessed. */
    setenv(ANDROID_ENV_FILES_DIR, "relative/files", 1);
    unsetenv(ANDROID_ENV_CACHE_DIR);
    persistent = true;
    CHECK(path_pref(&persistent) == NULL && !persistent);
    CHECK(path_cache() == NULL);
    unsetenv(ANDROID_ENV_NATIVE_DIR);
    CHECK(!path_self_executable(exe, sizeof(exe)));
}

static app_launch_params_t *launch(int argc, char **argv) {
    coop_requested_set = false;
    return app_handle_launch(NULL, argc, argv);
}

static void test_launch(void) {
    /* What EclipseActivity passes for `am start --es host_uuid <uuid> --ei host_app_id <id> --ez coop true`. */
    char *full[] = {"eclipse", "--host-uuid", "0f4e2a3c-1b5d-4c6e-9f7a-8b9c0d1e2f3a", "--host-app-id", "881448767",
                    "--coop"};
    app_launch_params_t *p = launch(6, full);
    CHECK(p != NULL);
    if (p != NULL) {
        CHECK(memcmp(p->default_host_uuid.data, "0f4e2a3c-1b5d-4c6e-9f7a-8b9c0d1e2f3a", 36) == 0);
        CHECK(p->default_app_id == 881448767 && p->coop_requested);
        CHECK(coop_requested_set && coop_requested_value);
        free(p);
    }

    /* A stream without co-op: co-op is per launch, so it is set false, never left from before. */
    char *plain[] = {"eclipse", "--host-uuid", "0f4e2a3c-1b5d-4c6e-9f7a-8b9c0d1e2f3a", "--host-app-id", "1"};
    p = launch(5, plain);
    CHECK(p != NULL && !p->coop_requested && p->default_app_id == 1 && coop_requested_set && !coop_requested_value);
    free(p);

    /* No launch parameters: the launcher, as on webOS. A bad or missing id is 0 (choose in the UI). */
    char *none[] = {"eclipse"};
    CHECK(launch(1, none) == NULL);
    char *bad_id[] = {"eclipse", "--host-app-id", "-5"};
    p = launch(3, bad_id);
    CHECK(p != NULL && p->default_app_id == 0);
    free(p);
    char *dangling[] = {"eclipse", "--host-uuid"};
    CHECK(launch(2, dangling) == NULL);

    /* The co-op peer's command line is not a launch. */
    char *peer[] = {"libeclipse_peer.so", "--coop-secondary", "/proc/self/fd/9/aurora-coop.sock"};
    secondary_socket = "/proc/self/fd/9/aurora-coop.sock";
    CHECK(launch(3, peer) == NULL && !coop_requested_set);
    secondary_socket = NULL;
}

int main(void) {
    char root[] = "/tmp/eclipse_android_paths_XXXXXX";
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    test_paths(root);
    test_launch();
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        printf("could not remove %s\n", root);
    }
    if (failures == 0) printf("Android paths and launch: OK\n");
    return failures == 0 ? 0 : 1;
}

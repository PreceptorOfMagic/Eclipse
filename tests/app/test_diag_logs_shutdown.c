/* Standalone loopback lifecycle regression using the production HTTP server and SDL.
 * Filesystem/logging/UUID adapters below replace unrelated application dependencies.
 *
 * cc -std=gnu11 -O1 -g -ffunction-sections -fdata-sections -DAPP_VERSION='"test"' \
 *   -Isrc/app -Ithird_party/commons/util/logging -Ithird_party/commons/util/uuidstr \
 *   $(sdl2-config --cflags) tests/app/test_diag_logs_shutdown.c \
 *   -Wl,--gc-sections $(sdl2-config --libs) -o /tmp/test_diag_logs_shutdown
 * timeout 15s /tmp/test_diag_logs_shutdown
 *
 * No host, TV, encoder, stream, or recording is involved. The unpatched server
 * hangs joining its worker when a connected browser stops reading a large reply.
 */
#include <assert.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/ioctl.h>

#include "../../src/app/util/diag_logs.c"

void path_join_to(char *out, size_t cap, const char *dir, const char *name) {
    snprintf(out, cap, "%s/%s", dir, name);
}

FILE *path_fopen(const char *path, const char *mode) { return fopen(path, mode); }

bool uuidstr_random(uuidstr_t *id) {
    memcpy(id, "12345678-1234-1234-1234-123456789abc", sizeof(*id));
    return true;
}

void commons_log_printf(commons_log_level level, const char *tag, const char *fmt, ...) {
    (void) level;
    (void) tag;
    (void) fmt;
}

static int connect_client(const char *url, char *target, size_t target_size) {
    char host[64], route[256];
    int port = 0;
    assert(sscanf(url, "http://%63[^:]:%d/%255s", host, &port, route) == 3);
    snprintf(target, target_size, "/%s", route);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int small = 1024;
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)) == 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == 0);
    const Uint32 deadline = SDL_GetTicks() + 2000;
    bool accepted = false;
    do {
        SDL_AtomicLock(&share_client_lock);
        accepted = share_client != SOCK_INVALID;
        if (accepted) {
            assert(setsockopt(share_client, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
        }
        SDL_AtomicUnlock(&share_client_lock);
        if (!accepted) SDL_Delay(1);
    } while (!accepted && !SDL_TICKS_PASSED(SDL_GetTicks(), deadline));
    assert(accepted);
    return fd;
}

static void stop_bounded(const char *description) {
    const Uint32 before = SDL_GetTicks();
    diag_logs_share_stop();
    const Uint32 elapsed = SDL_GetTicks() - before;
    assert(elapsed < 1000);
    assert(share_thread == NULL && share_client == SOCK_INVALID && listen_sock == SOCK_INVALID);
    assert(share_summary == NULL);
    printf("PASS: %s (%u ms)\n", description, elapsed);
}

int main(void) {
    alarm(12); /* a broken join must fail the test, never strand a test process */
    assert(SDL_Init(0) == 0);
    char directory[] = "/tmp/eclipse-share-test-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    char url[512], target[256];

    assert(diag_logs_share_start(directory, "test", url, sizeof(url)));
    int client = connect_client(url, target, sizeof(target));
    stop_bounded("close while a browser has sent no request");
    close(client);

    char *summary = malloc(32u * 1024u * 1024u);
    assert(summary != NULL);
    memset(summary, 'x', 32u * 1024u * 1024u - 1u);
    summary[32u * 1024u * 1024u - 1u] = '\0';
    assert(diag_logs_share_start(directory, summary, url, sizeof(url)));
    free(summary);
    client = connect_client(url, target, sizeof(target));
    char request[512];
    const int len = snprintf(request, sizeof(request), "GET %sall.txt HTTP/1.0\r\n\r\n", target);
    assert(send(client, request, (size_t) len, MSG_NOSIGNAL) == len);
    int available = 0;
    const Uint32 deadline = SDL_GetTicks() + 2000;
    do {
        assert(ioctl(client, FIONREAD, &available) == 0);
        if (!available) SDL_Delay(1);
    } while (!available && !SDL_TICKS_PASSED(SDL_GetTicks(), deadline));
    assert(available > 0); /* worker is serving; leave receive window full */
    SDL_Delay(100);
    stop_bounded("close while a browser stops reading a large download");
    close(client);

    for (int i = 0; i < 10; ++i) {
        assert(diag_logs_share_start(directory, "restart", url, sizeof(url)));
        stop_bounded("restart and idle close release socket/thread/summary");
    }
    diag_logs_share_stop(); /* idempotent */
    assert(rmdir(directory) == 0);
    SDL_Quit();
    alarm(0);
    return 0;
}

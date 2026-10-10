#!/usr/bin/env python3
"""Compile the actual POSIX adapter and exercise only disposable local paths."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = r'''
#include "platform/posix/runtime_files.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "%d: %s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)
static const char *cache;
char *path_runtime_cache(void) {
    if (!strcmp(cache, "NONE")) return NULL;
    /* Simulate the platform helper walking a real /tmp/aurora-* ancestor.
     * Its libc calls must bypass translation during initialization. */
    char *ancestor = strdup(cache); REQUIRE(ancestor);
    char *slash = strchr(ancestor + 5, '/'); if (slash) *slash = 0;
    REQUIRE(access(ancestor, F_OK) == 0); free(ancestor);
    return strdup(cache);
}
static const char *logical = "/tmp/aurora-runtime-fixture.sock";
static void *worker(void *arg) {
    char path[80];
    snprintf(path, sizeof(path), "/tmp/coop-fixture-%ld", (long)arg);
    FILE *f = fopen(path, "w"); REQUIRE(f); REQUIRE(fputs("private", f) >= 0); REQUIRE(fclose(f) == 0);
    struct stat st; REQUIRE(stat(path, &st) == 0); REQUIRE((st.st_mode & 0777) == 0600);
    f = fopen(path, "r"); REQUIRE(f); char text[20]; REQUIRE(fgets(text, sizeof(text), f));
    REQUIRE(strcmp(text, "private") == 0); fclose(f); REQUIRE(unlink(path) == 0);
    REQUIRE(access(path, F_OK) == -1 && errno == ENOENT); return NULL;
}
int main(int argc, char **argv) {
    REQUIRE(argc >= 3); cache = argv[2];
    if (!strcmp(argv[1], "files")) {
        pthread_t threads[8];
        for (long i = 0; i < 8; ++i) REQUIRE(pthread_create(&threads[i], NULL, worker, (void *)i) == 0);
        for (int i = 0; i < 8; ++i) REQUIRE(pthread_join(threads[i], NULL) == 0);
        FILE *f = fopen("/tmp/coop-mode", "w+"); REQUIRE(f); fputs("hello", f); rewind(f);
        REQUIRE(fgetc(f) == 'h'); fclose(f);
        f = fopen("/tmp/coop-mode", "a"); REQUIRE(f); fputs("!", f); fclose(f);
        f = fopen("/tmp/coop-mode", "r"); REQUIRE(f); char text[20]; REQUIRE(fgets(text, sizeof(text), f));
        REQUIRE(!strcmp(text, "hello!")); fclose(f); REQUIRE(unlink("/tmp/coop-mode") == 0);
        f = fopen("/tmp/coop-mode", "w"); REQUIRE(f); fputs("original", f); fclose(f);
        f = fopen("/tmp/coop-mode", "r+"); REQUIRE(f); fputs("X", f); fclose(f);
        f = fopen("/tmp/coop-mode", "r"); REQUIRE(f); REQUIRE(fgetc(f) == 'X'); fclose(f);
        f = fopen("/tmp/coop-mode", "w"); REQUIRE(f); fclose(f);
        struct stat empty; REQUIRE(stat("/tmp/coop-mode", &empty) == 0 && empty.st_size == 0);
        REQUIRE(unlink("/tmp/coop-mode") == 0);
        REQUIRE(argc == 4); f = fopen(argv[3], "r"); REQUIRE(f); REQUIRE(fgetc(f) == 'u'); fclose(f);
        char alias[108]; REQUIRE(eclipse_runtime_socket_path(logical, alias, sizeof(alias)) == 0);
        int fd = -1; REQUIRE(sscanf(alias, "/proc/self/fd/%d/", &fd) == 1);
        REQUIRE(fcntl(fd, F_GETFD) & FD_CLOEXEC);
        return 0;
    }
    if (!strcmp(argv[1], "refuse")) {
        REQUIRE(fopen("/tmp/coop-fixture-denied", "w") == NULL);
        char alias[108]; REQUIRE(eclipse_runtime_socket_path(logical, alias, sizeof(alias)) == -1);
        return 0;
    }
    if (!strcmp(argv[1], "entry")) {
        REQUIRE(fopen("/tmp/coop-fixture-denied", "w") == NULL);
        REQUIRE(unlink("/tmp/coop-fixture-denied") == -1);
        return 0;
    }
    if (!strcmp(argv[1], "server") || !strcmp(argv[1], "client")) {
        bool server = !strcmp(argv[1], "server");
        if (!server) for (int i = 0; i < 30; ++i) REQUIRE(open("/dev/null", O_RDONLY | O_CLOEXEC) >= 0);
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        REQUIRE(eclipse_runtime_socket_path(logical, addr.sun_path, sizeof(addr.sun_path)) == 0);
        int fd = socket(AF_UNIX, SOCK_STREAM, 0); REQUIRE(fd >= 0);
        if (server) {
            if (unlink(logical) != 0) REQUIRE(errno == ENOENT);
            REQUIRE(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0); REQUIRE(listen(fd, 1) == 0);
            printf("%s\n", addr.sun_path); fflush(stdout);
            int peer = accept(fd, NULL, NULL); REQUIRE(peer >= 0); char byte;
            REQUIRE(read(peer, &byte, 1) == 1 && byte == 'Q'); REQUIRE(write(peer, "A", 1) == 1);
            close(peer); close(fd); REQUIRE(unlink(logical) == 0);
        } else {
            REQUIRE(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
            REQUIRE(write(fd, "Q", 1) == 1); char byte; REQUIRE(read(fd, &byte, 1) == 1 && byte == 'A');
            close(fd); printf("%s\n", addr.sun_path);
        }
        return 0;
    }
    return 2;
}
'''.replace('#include <errno.h>', '#include <errno.h>\n#include <stdbool.h>')


def main():
    with tempfile.TemporaryDirectory(prefix="aurora-runtime-fixture-") as scratch:
        base = Path(scratch) / "source with spaces"
        base.mkdir()
        source = base / "fixture.c"
        source.write_text(SOURCE)
        exe = base / "fixture"
        header = ROOT / "src/app/platform/posix/runtime_files.h"
        common = ["cc", "-D_GNU_SOURCE", "-DECLIPSE_POSIX_RUNTIME=1", "-std=c11",
                  "-Wall", "-Wextra", "-Werror", "-O2", "-pthread", "-I" + str(ROOT / "src/app"),
                  "-include", str(header)]
        subprocess.run(common + [str(source), str(header.with_suffix('.c')), "-o", str(exe)], check=True)
        cache = base / ("long-cache-" + "x" * 100) / ("unicode-λ-" + "y" * 100)
        cache.mkdir(parents=True, mode=0o700)
        canary = base / "unrelated"
        canary.write_text("untouched")

        def run(mode, path, *args):
            return subprocess.run([str(exe), mode, str(path), *map(str, args)], check=True,
                                  capture_output=True, text=True, timeout=15)

        previous_umask = os.umask(0o002)
        cache.chmod(0o775)  # Existing application leaf from a permissive older install.
        try:
            run("files", cache, canary)
        finally:
            os.umask(previous_umask)
        assert cache.stat().st_mode & 0o777 == 0o700
        assert canary.read_text() == "untouched"
        assert not list((cache / "runtime").iterdir())
        assert (cache / "runtime").stat().st_mode & 0o777 == 0o700
        # An owned runtime leaf that was opened up is closed again and used, like its parent:
        # a webOS 26 boot reopens the whole app tree to 0775 (measured 2026-10-10), and refusing
        # it disabled every runtime file, co-op's socket included. Entries stay individually
        # checked (owner, type, link count), so nothing planted while it was open is trusted.
        for mode in (0o777, 0o775, 0o755):
            (cache / "runtime").chmod(mode)
            run("files", cache, canary)
            assert (cache / "runtime").stat().st_mode & 0o777 == 0o700
            assert canary.read_text() == "untouched"
        run("refuse", "NONE")
        link = base / "linked-cache"
        link.symlink_to(cache, target_is_directory=True)
        run("refuse", link)
        victim = cache / "runtime/coop-fixture-denied"
        victim.symlink_to(canary)
        run("entry", cache)
        victim.unlink()
        os.link(canary, victim)
        run("entry", cache)
        victim.unlink()
        assert canary.read_text() == "untouched"
        os.mkfifo(victim, 0o600)
        run("entry", cache)
        victim.unlink()
        victim.mkdir()
        run("entry", cache)
        victim.rmdir()
        shutil.rmtree(cache / "runtime")
        (cache / "runtime").symlink_to(base, target_is_directory=True)
        run("refuse", cache)
        (cache / "runtime").unlink()

        def start():
            process = subprocess.Popen([str(exe), "server", str(cache)], stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True)
            import select
            if not select.select([process.stdout], [], [], 10)[0]:
                process.kill(); process.wait(); raise AssertionError("server readiness timeout")
            alias = process.stdout.readline().strip()
            if not alias.startswith("/proc/self/fd/"):
                if process.poll() is None: process.kill()
                _, error = process.communicate(timeout=2)
                raise AssertionError((alias, error))
            return process, alias

        server, server_alias = start()
        try:
            client_alias = run("client", cache).stdout.strip()
            assert server_alias != client_alias, (server_alias, client_alias)
            assert server.wait(timeout=10) == 0, server.stderr.read()
        finally:
            if server.poll() is None: server.kill(); server.wait()
        assert not list((cache / "runtime").iterdir())
        # A forced close leaves only an owned socket inode; restart removes that
        # exact stale node, not the directory or another process's arbitrary files.
        server, _ = start()
        server.kill(); assert server.wait(timeout=10) < 0
        assert (cache / "runtime/aurora-runtime-fixture.sock").is_socket()
        server, _ = start()
        try:
            run("client", cache)
            assert server.wait(timeout=10) == 0, server.stderr.read()
        finally:
            if server.poll() is None: server.kill(); server.wait()
        assert not list((cache / "runtime").iterdir())
        # Compile-time diagnostic gate must leave libc calls and socket paths alone.
        diagnostic = base / "diagnostic.c"
        diagnostic.write_text('#include "platform/posix/runtime_files.h"\n'
                              '#ifdef ECLIPSE_RUNTIME_ACTIVE\n#error diagnostic paths changed\n#endif\n'
                              'int main(void) { return 0; }\n')
        subprocess.run(common + ["-DAURORA_TEST_HOOKS=1", str(diagnostic),
                                 str(header.with_suffix('.c')), "-o", str(base / "diagnostic")], check=True)
        subprocess.run([str(base / "diagnostic")], check=True)
    assert not Path(scratch).exists()
    print("PASS files/threads/modes, no fallback, symlink/hardlink refusal, different-FD long-path sockets, forced-close restart, diagnostic gate; all private paths cleaned")


if __name__ == "__main__":
    main()

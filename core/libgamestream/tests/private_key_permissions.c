#include "file_compat.h"

/* Keep this filesystem regression active in Release builds as well. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char fixture[] = "synthetic private-key fixture\n";

static void expect_mode(const char *path, mode_t mode) {
    struct stat st;
    assert(stat(path, &st) == 0);
    assert((st.st_mode & 07777) == mode);
}

static void expect_bytes(const char *path, int private_read) {
    unsigned char *data = NULL;
    size_t size = 0;
    assert((private_read ? gs_read_private_file_utf8(path, &data, &size)
                         : gs_read_file_utf8(path, &data, &size)) == 0);
    assert(size == sizeof(fixture) - 1);
    assert(memcmp(data, fixture, size) == 0);
    free(data);
}

static void expect_refused(const char *path) {
    unsigned char *data = (void *) 1;
    size_t size = 123;
    assert(gs_read_private_file_utf8(path, &data, &size) == -1);
    assert(data == NULL && size == 0);
    assert(gs_fopen_private_utf8(path, 1) == NULL);
}

int main(void) {
    char directory[] = "/tmp/eclipse-private-key-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    int previous = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(previous >= 0 && chdir(directory) == 0);
    mode_t old_mask = umask(0000);

    FILE *file = gs_fopen_private_utf8("key.pem", 1);
    assert(file != NULL);
    expect_mode("key.pem", 0600); /* Before any private bytes are written. */
    assert((fcntl(fileno(file), F_GETFD) & FD_CLOEXEC) != 0);
    assert(fwrite(fixture, 1, sizeof(fixture) - 1, file) == sizeof(fixture) - 1);
    assert(fclose(file) == 0);

    assert(chmod("key.pem", 0644) == 0);
    expect_bytes("key.pem", 1); /* Upgrade existing keys without re-pairing. */
    expect_mode("key.pem", 0600);
    assert(chmod("key.pem", 0400) == 0);
    expect_bytes("key.pem", 1);
    expect_mode("key.pem", 0600);

    assert(chmod("key.pem", 0644) == 0);
    assert(symlink("key.pem", "symlink.pem") == 0);
    expect_refused("symlink.pem");
    expect_mode("key.pem", 0644);
    expect_bytes("key.pem", 0);
    assert(unlink("symlink.pem") == 0);
    assert(symlink("missing.pem", "symlink.pem") == 0);
    expect_refused("symlink.pem");
    assert(access("missing.pem", F_OK) == -1 && errno == ENOENT);
    assert(unlink("symlink.pem") == 0);

    assert(link("key.pem", "hardlink.pem") == 0);
    expect_refused("hardlink.pem");
    expect_refused("key.pem");
    expect_mode("key.pem", 0644);
    expect_bytes("key.pem", 0);
    assert(unlink("hardlink.pem") == 0);

    assert(mkfifo("fifo.pem", 0600) == 0);
    expect_refused("fifo.pem"); /* Both operations must return without blocking. */
    assert(unlink("fifo.pem") == 0);
    assert(mkdir("directory.pem", 0700) == 0);
    expect_refused("directory.pem");
    assert(rmdir("directory.pem") == 0);

    file = gs_fopen_utf8("oversized.pem", "wb");
    assert(file != NULL && ftruncate(fileno(file), 16385) == 0 && fclose(file) == 0);
    unsigned char *too_large = NULL;
    size_t too_large_size = 0;
    assert(gs_read_private_file_utf8("oversized.pem", &too_large, &too_large_size) == -1);
    assert(errno == EFBIG && too_large == NULL && too_large_size == 0);
    expect_mode("oversized.pem", 0666);
    assert(unlink("oversized.pem") == 0);

    if (geteuid() == 0) {
        /* Actual unprivileged second UID exercises the legacy TV arrangement:
         * key belongs to developer, native app can replace it in writable dir. */
        assert(chmod(directory, 0777) == 0);
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            assert(setgid(65534) == 0 && setuid(65534) == 0);
            assert(geteuid() == 65534);
            assert(gs_fopen_private_utf8("key.pem", 1) == NULL);
            expect_mode("key.pem", 0644);
            expect_bytes("key.pem", 1);
            expect_mode("key.pem", 0600);
            struct stat migrated;
            assert(stat("key.pem", &migrated) == 0 && migrated.st_uid == 65534);
            expect_bytes("key.pem", 1);
            _exit(0);
        }
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        expect_bytes("key.pem", 0);
        assert(chown("key.pem", 0, (gid_t) -1) == 0 && chmod("key.pem", 0644) == 0);
        /* An unwritable legacy parent must fail without changing the old key. */
        assert(chmod(directory, 0755) == 0);
        child = fork();
        assert(child >= 0);
        if (child == 0) {
            assert(setgid(65534) == 0 && setuid(65534) == 0);
            expect_refused("key.pem");
            _exit(0);
        }
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        expect_mode("key.pem", 0644);
        expect_bytes("key.pem", 0);
        puts("actual second-UID migration and unwritable-parent refusal: PASS");
    } else {
        puts("second-UID migration: SKIP (requires isolated root fixture)");
    }

    /* Generation may replace a regular owned key, but only after securing it. */
    file = gs_fopen_private_utf8("key.pem", 1);
    assert(file != NULL);
    expect_mode("key.pem", 0600);
    struct stat st;
    assert(fstat(fileno(file), &st) == 0 && st.st_size == 0);
    assert(fclose(file) == 0);
    assert(unlink("key.pem") == 0);
    umask(old_mask);
    assert(fchdir(previous) == 0);
    close(previous);
    assert(rmdir(directory) == 0);
    puts("private-key creation, upgrade, no-follow, single-link, type and descriptor checks: PASS");
    return 0;
}

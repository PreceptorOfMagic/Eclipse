/* Process-level isolation regression for the coordinator's legacy orphan sweep.
 * No streams or hosts: helpers only pause, using private temporary socket paths.
 * Compile as test_coop_coord, adding this file instead of test_coop_coord.c.
 * Run in a private PID namespace with a separate PID 1, since the Codex host
 * uses a subreaper: unshare --user --map-root-user --pid --fork --mount-proc
 * sh -c '/path/to/test; result=$?; exit "$result"'
 * Do not run the test itself as PID 1: its live children would then have PPid 1. */
#include "coop_coord.h"
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t helpers[4];
static int failed;
static void cleanup(void) {
    for (unsigned i = 0; i < 4; ++i) {
        if (helpers[i] > 0) {
            kill(helpers[i], SIGKILL);
            waitpid(helpers[i], NULL, 0);
        }
    }
}
static void check(int yes, const char *what) {
    printf("[%s] %s\n", yes ? "PASS" : "FAIL", what);
    failed += !yes;
}
static int process_status(pid_t pid, long *parent) {
    char file[64], line[256], state = 0;
    snprintf(file, sizeof(file), "/proc/%ld/status", (long)pid);
    FILE *f = fopen(file, "r");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "PPid:", 5)) sscanf(line + 5, "%ld", parent);
        if (!strncmp(line, "State:", 6)) sscanf(line + 6, " %c", &state);
    }
    fclose(f);
    return state && state != 'Z';
}
static pid_t helper(const char *exe, const char *socket, int orphan) {
    int report[2], ready[2];
    if (pipe(report) || pipe(ready)) return -1;
    pid_t child = fork();
    if (child == 0) {
        close(report[0]); close(ready[0]);
        if (orphan) {
            pid_t grandchild = fork();
            if (grandchild != 0) {
                if (grandchild > 0 && write(report[1], &grandchild, sizeof(grandchild)) != sizeof(grandchild)) {
                    kill(grandchild, SIGKILL);
                    _exit(1);
                }
                _exit(grandchild < 0 ? 1 : 0);
            }
        } else {
            pid_t me = getpid();
            if (write(report[1], &me, sizeof(me)) != sizeof(me)) _exit(1);
        }
        close(report[1]);
        char fd[32]; snprintf(fd, sizeof(fd), "%d", ready[1]);
        execl(exe, exe, "--coop-secondary", socket, fd, (char *)NULL);
        _exit(127);
    }
    close(report[1]); close(ready[1]);
    pid_t worker = -1;
    ssize_t n = read(report[0], &worker, sizeof(worker));
    char byte;
    ssize_t armed = read(ready[0], &byte, 1);
    close(report[0]); close(ready[0]);
    if (orphan) waitpid(child, NULL, 0);
    return n == sizeof(worker) && armed == 1 ? worker : -1;
}
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--coop-secondary")) {
        int fd = atoi(argv[3]);
        if (write(fd, "R", 1) != 1) return 2;
        close(fd);
        for (;;) pause();
    }
    atexit(cleanup);
    char exe[PATH_MAX], dir[] = "/tmp/aurora-orphan-test-XXXXXX";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 0 || !mkdtemp(dir)) return 2;
    exe[n] = 0;
    char socket[PATH_MAX], other[PATH_MAX];
    snprintf(socket, sizeof(socket), "%s/coord.sock", dir);
    snprintf(other, sizeof(other), "%s/other.sock", dir);
    helpers[0] = helper(exe, socket, 0);
    helpers[1] = helper(exe, other, 0);
    helpers[2] = helper(exe, socket, 1);
    helpers[3] = helper(exe, other, 1);
    for (unsigned i = 0; i < 4; ++i) check(helpers[i] > 0, "helper exec reached ready");
    if (failed) return 1;
    long parent = -1;
    process_status(helpers[2], &parent);
    check(parent == 1, "orphan adopted by init (positive sweep precondition)");
    if (failed) return 1;
    check(coop_coord_start(socket), "coordinator starts");
    for (unsigned i = 0; i < 100; ++i) {
        if (!process_status(helpers[2], &parent)) break;
        usleep(1000);
    }
    check(!process_status(helpers[2], &parent), "matching orphan is terminated");
    check(process_status(helpers[0], &parent), "same-socket child with live parent survives");
    check(process_status(helpers[1], &parent), "other-socket child with live parent survives");
    check(process_status(helpers[3], &parent), "other-socket orphan survives");
    coop_coord_stop();
    unlink(socket); rmdir(dir);
    return failed ? 1 : 0;
}

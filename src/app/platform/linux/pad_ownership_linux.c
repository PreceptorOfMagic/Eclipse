/*
 * CONTROLLER OWNERSHIP ON LINUX (platform/pad_ownership.h): a controller given to the other PC stops reaching this
 * PC's own game and Steam, while Eclipse keeps reading it to forward it.
 *
 * The mechanism is the kernel's exclusive grab of an input device (EVIOCGRAB): while one open file of an evdev node
 * holds the grab, only that file receives the device's events -- every other reader, ALREADY open or opened later,
 * gets nothing. That is both "hide" and "evict" of the Windows version in one step, needs no administrator rights,
 * and ends by itself when the file is closed: if Eclipse dies, the kernel releases the pads (no crash recovery).
 *
 * Which file grabs: when Eclipse's own SDL reads the pad through that node, the grab is taken on SDL's open file
 * (found in /proc/self/fd; a dup shares the same open file, so the grab belongs to SDL's reader), so Eclipse keeps
 * receiving it. A node SDL does not read (SDL reads the pad through hidraw, or the node is a motion sensor or touchpad
 * SDL has not opened) is opened here and grabbed; nobody reads it.
 *
 * What a grab cannot cover: hidraw. PlayStation, Switch and some Bluetooth pads also have a raw HID node, and Steam
 * and SDL's HIDAPI driver read pads through it. A grab does not apply there and changing its permissions needs root,
 * so the scan names every other program holding a hidden pad's hidraw node (not closable) and the popup tells the
 * user. Pads with no hidraw node (Xbox pads through xpad or xone, the Xbox Wireless Adapter) are fully covered.
 */
#include "platform/pad_ownership.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "logging.h"

#define MAX_NODES 32

typedef struct grabbed_t {
    int fd;
    bool sdl_file;          /* the grab is on SDL's own reader (fd is our dup of it) */
    char node[32];          /* "event5" */
} grabbed_t;

static grabbed_t grabbed[MAX_NODES];
static int grabbed_count;
static pad_own_pad_t last[PAD_OWN_MAX_PADS];
static int last_count;
static SDL_mutex *lock;

static void ensure_lock(void) {
    if (lock == NULL) {
        lock = SDL_CreateMutex();
    }
}

/* realpath of a sysfs link, or "" */
static void sys_real(const char *path, char *out, size_t len) {
    char buf[PATH_MAX];
    if (realpath(path, buf) == NULL) {
        out[0] = 0;
        return;
    }
    SDL_strlcpy(out, buf, len);
}

/* The physical device an evdev node or hidraw node belongs to: the HID device (or USB interface, or GIP device)
 * that every node of one controller shares. */
static bool parent_of(const char *node, char *out, size_t len) {
    char path[PATH_MAX];
    if (strncmp(node, "event", 5) == 0) {
        snprintf(path, sizeof(path), "/sys/class/input/%s/device/device", node);
        sys_real(path, out, len);
        if (out[0] == 0) {
            /* a virtual device (uinput: Steam Input's, a remapper's) has no physical parent: the input device is it */
            snprintf(path, sizeof(path), "/sys/class/input/%s/device", node);
            sys_real(path, out, len);
        }
        return out[0] != 0;
    } else if (strncmp(node, "hidraw", 6) == 0) {
        snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device", node);
    } else {
        return false;
    }
    sys_real(path, out, len);
    return out[0] != 0;
}

/* Appends "name" to a comma list in out. */
static void list_add(char *out, size_t len, const char *name) {
    const size_t used = strlen(out);
    snprintf(out + used, len - used, "%s%s", used ? "," : "", name);
}

/* Every node of @p cls ("input" -> eventN, "hidraw" -> hidrawN) whose physical device is @p parent. */
static void nodes_of(const char *cls, const char *prefix, const char *parent, char *out, size_t len) {
    char dir[64];
    snprintf(dir, sizeof(dir), "/sys/class/%s", cls);
    DIR *d = opendir(dir);
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, prefix, strlen(prefix)) != 0) {
            continue;
        }
        char p[PATH_MAX];
        if (parent_of(e->d_name, p, sizeof(p)) && strcmp(p, parent) == 0) {
            list_add(out, len, e->d_name);
        }
    }
    closedir(d);
}

bool pad_own_supported(char *why, size_t why_len) {
    if (access("/dev/input", R_OK | X_OK) != 0) {
        if (why != NULL && why_len > 0) {
            SDL_strlcpy(why, "Eclipse cannot open this PC's input devices (/dev/input), so it cannot keep a controller "
                             "away from this PC.", why_len);
        }
        return false;
    }
    return true;
}

bool pad_own_resolve(SDL_JoystickID id, const char *name, pad_own_pad_t *out) {
    SDL_zerop(out);
    const char *path = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_JoystickGetDeviceInstanceID(i) == id) {
            path = SDL_JoystickPathForIndex(i);
            break;
        }
    }
    if (path == NULL || strncmp(path, "/dev/", 5) != 0) {
        commons_log_warn("PadOwn", "%s: no device node (virtual or unknown pad), cannot be hidden",
                         name != NULL ? name : "?");
        return false;
    }
    const char *base = strrchr(path, '/') + 1;
    char parent[PATH_MAX];
    if (!parent_of(base, parent, sizeof(parent))) {
        commons_log_warn("PadOwn", "%s: %s has no physical device in sysfs", name != NULL ? name : "?", path);
        return false;
    }
    /* instance = "event5,event6|hidraw3": every evdev node of the controller, then its raw HID nodes */
    char events[200] = "", raws[100] = "";
    nodes_of("input", "event", parent, events, sizeof(events));
    nodes_of("hidraw", "hidraw", parent, raws, sizeof(raws));
    if (events[0] == 0) {
        commons_log_warn("PadOwn", "%s: no evdev node beside %s", name != NULL ? name : "?", path);
        return false;
    }
    snprintf(out->instance, sizeof(out->instance), "%s|%s", events, raws);
    SDL_strlcpy(out->name, name != NULL ? name : "Controller", sizeof(out->name));
    out->kind = PAD_OWN_KIND_HID;
    commons_log_info("PadOwn", "resolved %s: SDL reads %s; nodes %s", out->name, path, out->instance);
    return true;
}

/* SDL's own open file of /dev/input/<node>, or -1 */
static int sdl_fd_of(const char *node) {
    char want[48];
    snprintf(want, sizeof(want), "/dev/input/%s", node);
    DIR *d = opendir("/proc/self/fd");
    if (d == NULL) {
        return -1;
    }
    int found = -1;
    const int own = dirfd(d);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const int fd = atoi(e->d_name);
        if (e->d_name[0] == '.' || fd == own) {
            continue;
        }
        char link[64], target[PATH_MAX];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        const ssize_t n = readlink(link, target, sizeof(target) - 1);
        if (n <= 0) {
            continue;
        }
        target[n] = 0;
        if (strcmp(target, want) == 0) {
            bool ours = false;
            for (int i = 0; i < grabbed_count; i++) {
                ours = ours || grabbed[i].fd == fd;
            }
            if (!ours) {
                found = fd;
                break;
            }
        }
    }
    closedir(d);
    return found;
}

static bool already_grabbed(const char *node) {
    for (int i = 0; i < grabbed_count; i++) {
        if (strcmp(grabbed[i].node, node) == 0) {
            return true;
        }
    }
    return false;
}

static bool grab_node(const char *node, char *err, size_t err_len) {
    if (already_grabbed(node) || grabbed_count >= MAX_NODES) {
        return true;
    }
    const int sdl = sdl_fd_of(node);
    int fd;
    if (sdl >= 0) {
        fd = fcntl(sdl, F_DUPFD_CLOEXEC, 0);
    } else {
        char path[48];
        snprintf(path, sizeof(path), "/dev/input/%s", node);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    }
    if (fd < 0) {
        snprintf(err, err_len, "cannot open %s: %s", node, strerror(errno));
        return false;
    }
    if (ioctl(fd, EVIOCGRAB, (void *) 1) != 0) {
        const int e = errno;
        close(fd);
        /* EBUSY: another program holds the grab already (an input remapper, a handheld daemon) */
        snprintf(err, err_len, "%s: %s", node, e == EBUSY ? "another program holds it exclusively" : strerror(e));
        return false;
    }
    grabbed_t *g = &grabbed[grabbed_count++];
    g->fd = fd;
    g->sdl_file = sdl >= 0;
    SDL_strlcpy(g->node, node, sizeof(g->node));
    commons_log_info("PadOwn", "grabbed %s (%s)", node, g->sdl_file ? "on Eclipse's own reader" : "own file, unread");
    return true;
}

static void release_all_locked(void) {
    for (int i = 0; i < grabbed_count; i++) {
        ioctl(grabbed[i].fd, EVIOCGRAB, (void *) 0);
        close(grabbed[i].fd);
        commons_log_info("PadOwn", "released %s", grabbed[i].node);
    }
    grabbed_count = 0;
}

bool pad_own_hide(const pad_own_pad_t *pads, int count, char *err, size_t err_len) {
    ensure_lock();
    SDL_LockMutex(lock);
    bool ok = true;
    for (int p = 0; p < count && ok; p++) {
        char events[200];
        SDL_strlcpy(events, pads[p].instance, sizeof(events));
        char *bar = strchr(events, '|');
        if (bar != NULL) {
            *bar = 0;
        }
        char *save = NULL;
        for (char *node = strtok_r(events, ",", &save); node != NULL && ok; node = strtok_r(NULL, ",", &save)) {
            ok = grab_node(node, err, err_len);
        }
    }
    if (!ok) {
        commons_log_warn("PadOwn", "hide failed (%s): every grab released", err);
        release_all_locked();
    } else {
        last_count = count < PAD_OWN_MAX_PADS ? count : PAD_OWN_MAX_PADS;
        memcpy(last, pads, sizeof(pad_own_pad_t) * (size_t) last_count);
    }
    SDL_UnlockMutex(lock);
    return ok;
}

int pad_own_last_hidden(pad_own_pad_t *out, int max) {
    ensure_lock();
    SDL_LockMutex(lock);
    const int n = last_count < max ? last_count : max;
    memcpy(out, last, sizeof(pad_own_pad_t) * (size_t) n);
    SDL_UnlockMutex(lock);
    return n;
}

/* Other processes (this user's; others cannot be inspected) holding /dev/<raw>. */
static void scan_raw(const char *raw, pad_own_scan_t *out) {
    char want[48];
    snprintf(want, sizeof(want), "/dev/%s", raw);
    DIR *proc = opendir("/proc");
    if (proc == NULL) {
        out->complete = false;
        return;
    }
    const pid_t self = getpid();
    struct dirent *pe;
    while ((pe = readdir(proc)) != NULL && out->count < PAD_OWN_MAX_HOLDERS) {
        const long pid = strtol(pe->d_name, NULL, 10);
        if (pid <= 0 || pid == self) {
            continue;
        }
        char fdpath[64];
        snprintf(fdpath, sizeof(fdpath), "/proc/%ld/fd", pid);
        DIR *fds = opendir(fdpath);
        if (fds == NULL) {
            out->unscanned++;
            continue;
        }
        bool holds = false;
        struct dirent *fe;
        while (!holds && (fe = readdir(fds)) != NULL) {
            char link[320], target[PATH_MAX];
            snprintf(link, sizeof(link), "%s/%s", fdpath, fe->d_name);
            const ssize_t n = readlink(link, target, sizeof(target) - 1);
            if (n > 0) {
                target[n] = 0;
                holds = strcmp(target, want) == 0;
            }
        }
        closedir(fds);
        if (!holds) {
            continue;
        }
        bool listed = false;
        for (int i = 0; i < out->count; i++) {
            listed = listed || out->holders[i].pid == (unsigned long) pid;
        }
        if (listed) {
            continue;
        }
        pad_own_holder_t *h = &out->holders[out->count++];
        h->pid = (unsigned long) pid;
        char comm_path[64];
        snprintf(comm_path, sizeof(comm_path), "/proc/%ld/comm", pid);
        FILE *f = fopen(comm_path, "r");
        if (f == NULL || fgets(h->exe, sizeof(h->exe), f) == NULL) {
            SDL_strlcpy(h->exe, "?", sizeof(h->exe));
        }
        if (f != NULL) {
            fclose(f);
        }
        h->exe[strcspn(h->exe, "\n")] = 0;
        /* Steam's client process is "steam"; its helpers carry their own names */
        SDL_strlcpy(h->display, strcmp(h->exe, "steam") == 0 ? "Steam" : h->exe, sizeof(h->display));
        h->closable = false;     /* nothing to gain: a reopened program would open the raw node again */
        h->admin = false;
        h->service[0] = 0;
        commons_log_info("PadOwn", "%s (pid %ld) holds %s, which a grab cannot cover", h->display, pid, want);
    }
    closedir(proc);
}

void pad_own_scan(pad_own_scan_t *out) {
    SDL_zerop(out);
    out->complete = true;
    pad_own_pad_t pads[PAD_OWN_MAX_PADS];
    const int n = pad_own_last_hidden(pads, PAD_OWN_MAX_PADS);
    for (int p = 0; p < n; p++) {
        const char *bar = strchr(pads[p].instance, '|');
        if (bar == NULL || bar[1] == 0) {
            continue;                    /* no hidraw node: the grab covers this pad completely */
        }
        char raws[100];
        SDL_strlcpy(raws, bar + 1, sizeof(raws));
        char *save = NULL;
        for (char *raw = strtok_r(raws, ",", &save); raw != NULL; raw = strtok_r(NULL, ",", &save)) {
            scan_raw(raw, out);
        }
    }
    /* Processes of other users are not this user's games; they are not reported as "unscanned" admin work. */
    out->unscanned = 0;
}

int pad_own_evict(const pad_own_scan_t *scan, char *report, size_t report_len) {
    (void) scan;
    if (report != NULL && report_len > 0) {
        SDL_strlcpy(report, "nothing to restart: the grab already cut off every program reading the pads' input "
                            "devices", report_len);
    }
    return 0;
}

bool pad_own_elevated(void) {
    return false;
}

bool pad_own_admin_needed(const pad_own_scan_t *scan) {
    (void) scan;
    return false;
}

pad_own_admin_result_t pad_own_evict_admin(char *report, size_t report_len) {
    if (report != NULL && report_len > 0) {
        report[0] = 0;
    }
    return PAD_OWN_ADMIN_DONE;
}

bool pad_own_admin_main(int argc, char *argv[], int *exit_code) {
    (void) argc;
    (void) argv;
    (void) exit_code;
    return false;
}

pad_own_admin_result_t pad_own_session_admin_begin(char *report, size_t report_len) {
    if (report != NULL && report_len > 0) {
        report[0] = 0;
    }
    return PAD_OWN_ADMIN_DONE;
}

bool pad_own_admin_ready(void) {
    return true;
}

bool pad_own_active(void) {
    ensure_lock();
    SDL_LockMutex(lock);
    const bool active = grabbed_count > 0;
    SDL_UnlockMutex(lock);
    return active;
}

void pad_own_session_end(void) {
    ensure_lock();
    SDL_LockMutex(lock);
    release_all_locked();
    SDL_UnlockMutex(lock);
}

void pad_own_startup_recover(void) {
    /* Nothing survives a crash: the kernel drops a grab with the file that holds it. */
}

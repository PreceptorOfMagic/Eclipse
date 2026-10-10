#include "util/path.h"

#include <SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

/*
 * CONFIG LIVES IN $HOME/conf, AND THE MOVE OUT OF THE INSTALL TREE WAS ABANDONED
 * DELIBERATELY (2026-08-14). Do not re-attempt it without re-measuring the premise.
 *
 * The move existed to stop `ares-install` destroying the pairing key and host list,
 * since $HOME on webOS IS the app install tree. Measured on webOS 33.31.68: THAT DOES
 * NOT HAPPEN. A canary file written into
 * /media/developer/apps/usr/palm/applications/com.aurora.gamestream/conf/ survived two
 * consecutive bare `ares-install` runs with no backup involved, as did hosts.ini,
 * moonlight.ini and key/. The installer replaces the app's own files and leaves the
 * directory's other contents alone. Old deploy-wrapper backups were not what
 * preserved those files; stale automatic restore has since been removed.
 *
 * Two earlier attempts and what each one taught:
 *
 * 1. /media/developer/com.aurora.gamestream/conf -- committed 9aa8e7f1, never deployed.
 *    IT COULD NEVER HAVE WORKED: /media/developer is `drwxrwxr-x root root` and the app
 *    runs as uid 6584 gid jailer, so path_dir_ensure fails, the $HOME fallback takes
 *    over, and the app quietly carries on exactly as before. It would have looked like
 *    a successful deploy that changed nothing.
 *
 * 2. /media/developer/temp/com.aurora.gamestream/conf -- built, deployed and VERIFIED
 *    WORKING, then reverted anyway. It is the one app-writable directory that is both
 *    persistent (real ext4 on mmcblk0p59; /media/internal is on the SAME partition but
 *    mounted READ-ONLY, so its world-writable mode bits are a trap) and untouched by an
 *    install. The app created it, migrated hosts.ini/moonlight.ini/key/ into it on
 *    launch, and ran from it correctly.
 *    IT WAS REVERTED BECAUSE IT SPLITS THE TOOLCHAIN'S BRAIN. Every tool here edits the
 *    OLD path -- restore_stable.sh rewrites moonlight.ini there, deploy_to_tv.sh backs
 *    up and restores there, and CLAUDE.md documents it as "the OLD path" that conf lives
 *    at. With the move in place the app reads a file nothing writes, so every config
 *    change made by every existing script silently stops taking effect. That is a far
 *    worse failure than the one being fixed, and it buys protection against a problem
 *    this firmware does not have.
 *
 * If a future webOS DOES start wiping conf on install, attempt 2 is the design that
 * works -- and the whole toolchain has to move with it, in the same change.
 *
 * LEFTOVER ON THE TV, AND IT IS A TRAP FOR ANY RE-ATTEMPT. Attempt 2 left a populated
 * /media/developer/temp/com.aurora.gamestream/conf behind, holding a hosts.ini,
 * moonlight.ini and key/ from 2026-08-14. It CANNOT be deleted from the dev shell: the
 * app created it as uid 6584 with mode 755, and `prisoner` (uid 6122) has no write
 * permission on it, so nothing short of the app itself or root can remove it. It is
 * inert while this file points at $HOME/conf. But the migration in attempt 2 copied
 * only files that were ABSENT, so re-enabling that path would find the stale hosts.ini
 * already there and PREFER IT over the live one. Delete that directory first.
 *
 * WEBOS 26 (2026-10-01): A FRESH INSTALL CANNOT CREATE $HOME/conf, SO IT IS NOW ONLY THE FIRST
 * CHOICE. Measured on 11.2.0 / 43.21.77: the install tree is `drwxrwxr-x 1000 1000` and the app
 * runs as its own uid in gid jailer, so mkdir fails and the app fell back to /tmp -- "Can't save
 * settings" on every launch and pairing lost at every power-off. The live app here only works
 * because a script created its conf/ as prisoner:jailer (775) long ago; the beta, installed
 * fresh, never had one. path_pref() now tries, in order:
 *   1. $HOME/conf -- UNCHANGED, and still preferred whenever it exists and is writable (or can be
 *      created). Every TV where it worked before, webOS 25 included, takes exactly the old path,
 *      and so does every script that edits it.
 *   2. /media/developer/temp/<app id>/conf -- attempt 2 above, which was VERIFIED on 33.31.68
 *      (webOS 25). Only reached where the old code went straight to /tmp. The app id is $HOME's
 *      last component, so the live and beta apps keep separate settings exactly as they do in
 *      their install trees. Evidence that it stays usable: the app uid is stable per app id
 *      (6584 wrote the 08-14 leftover AND the live hosts.ini on 09-30, across reinstalls and the
 *      webOS 26 update), and that leftover survived reboots, installs and the update.
 *   3. /tmp/moonlight-tv/conf -- unchanged last resort; the "Can't save settings" dialog stays.
 * 2026-10-08: actual active uninstall removed the install tree but left the
 * native-owned developer-temp profile behind. Unlike the abandoned outward
 * move above, moving an existing owned
 * fallback BACK into an absent HOME/conf makes it app-manager-owned storage.
 * Later actual upgrade preserved root0755; packaged userdata supplies a writable
 * install-owned container when direct HOME/conf cannot be created. Attempt only
 * an atomic no-replace rename; preserve the working fallback on
 * unsupported kernels, cross-mount paths, denied access or ambiguous ownership.
 * Existing HOME/conf still wins. Never merge profiles or recursively delete data.
 * Native migration and subsequent uninstall require their own TV measurement.
 *
 * The 08-14 leftover was backed up and removed from this TV on 2026-10-01 (it had become
 * group-writable, so prisoner could), so the trap above no longer applies to it.
 */

/* /media/developer/temp/<app id>/<leaf>, or NULL when $HOME does not end in a usable app id. */
static char *path_devtemp(const char *home, const char *leaf) {
    if (home == NULL) {
        return NULL;
    }
    size_t len = strlen(home);
    while (len > 1 && home[len - 1] == '/') {
        len--;
    }
    const char *id = home + len;
    while (id > home && id[-1] != '/') {
        id--;
    }
    const size_t idlen = (size_t) (home + len - id);
    if (idlen == 0 || (idlen == 1 && id[0] == '.') || (idlen == 2 && id[0] == '.' && id[1] == '.')) {
        return NULL;
    }
    char appdir[256];
    if (snprintf(appdir, sizeof(appdir), "/media/developer/temp/%.*s", (int) idlen, id) >= (int) sizeof(appdir)) {
        return NULL;
    }
    return path_join(appdir, leaf);
}

char *path_assets() {
    const char *basedir = SDL_getenv("HOME");
    return path_join(basedir, "assets");
}

/* The SDK predates renameat2. Linux v6.12 arch/arm/tools/syscall.tbl names
 * ARM syscall 382; include/uapi/linux/fs.h defines NOREPLACE as bit zero.
 * Keep an unsupported kernel as a safe migration failure, never use rename(). */
#if !defined(SYS_renameat2) && defined(__arm__) && defined(__NR_SYSCALL_BASE)
#define SYS_renameat2 (__NR_SYSCALL_BASE + 382)
#endif
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

static bool path_pref_migrate(const char *confdir, const char *fallback, bool managed) {
    if (confdir == NULL || fallback == NULL) return false;
#ifdef SYS_renameat2
    bool moved = false;
    int fromfd = -1, tofd = -1;
    char *from = strdup(fallback), *to = strdup(confdir);
    struct stat parent, profile, destination, current;
    if (from == NULL || to == NULL) goto done;
    char *fromleaf = strrchr(from, '/'), *toleaf = strrchr(to, '/');
    if (fromleaf == NULL || toleaf == NULL || fromleaf == from || toleaf == to) goto done;
    *fromleaf++ = 0;
    *toleaf++ = 0;
    fromfd = open(from, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    tofd = open(to, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fromfd < 0 || tofd < 0 || fstat(fromfd, &parent) != 0 ||
            parent.st_uid != geteuid() || (parent.st_mode & 0022) != 0 ||
            fstatat(fromfd, fromleaf, &profile, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISDIR(profile.st_mode) || profile.st_uid != geteuid() ||
            (managed && (profile.st_mode & 0022) != 0)) goto done;
    /* Validate the actual held rename target, not an earlier path lookup.
     * Managed acceptance must remain valid on the next launch as well. */
    if (managed && (fstat(tofd, &destination) != 0 ||
            ((destination.st_mode & 0022) != 0 &&
             (destination.st_mode & S_ISVTX) == 0))) goto done;
    if (syscall(SYS_renameat2, fromfd, fromleaf, tofd, toleaf, RENAME_NOREPLACE) != 0) goto done;
    moved = true;
    /* Publish directory metadata best-effort; after rename, never select the
     * old path again even if this filesystem rejects directory fsync. */
    (void) fsync(tofd);
    (void) fsync(fromfd);
    /* rmdir cannot remove non-empty directories. Recheck the path still names
     * the held, owned parent; preserve other files and never recurse. */
    if (lstat(from, &current) == 0 && S_ISDIR(current.st_mode) &&
            current.st_uid == geteuid() && current.st_dev == parent.st_dev &&
            current.st_ino == parent.st_ino) (void) rmdir(from);
done:
    if (fromfd >= 0) close(fromfd);
    if (tofd >= 0) close(tofd);
    free(from);
    free(to);
    return moved;
#else
    (void) confdir;
    (void) fallback;
    (void) managed;
    return false;
#endif
}

/* Preserve usable existing profiles before creating anything. The installer
 * preserves older read-only app-root modes, even when the new IPK says 0777.
 * Its new packaged userdata directory is writable without altering HOME/conf.
 * This measured uninstall-residue case supersedes the old outward-move premise:
 * operational tools resolve a single existing profile and refuse ambiguous
 * coexistence because their developer UID may have different access. */
static bool path_pref_existing(const char *path) {
    struct stat info;
    return path != NULL && lstat(path, &info) == 0 && S_ISDIR(info.st_mode) &&
            access(path, W_OK | X_OK) == 0;
}

/*
 * WEBOS 26 RESETS THE WHOLE DEVELOPER APP TREE TO 0775 AT EVERY BOOT. Measured 2026-10-10 after a
 * self-reboot: the app root, userdata/, userdata/conf and userdata/cache (and every file in them)
 * all had the boot instant as their change time; userdata went 1777 -> 0775 (owner 1000:1000,
 * sticky bit gone) and conf went 0700 -> 0775 (6584:jailer). The checks below then refused the
 * profile, and the app started on an empty one: every pairing and setting "lost" after a reboot,
 * though the files were intact. Two things follow:
 *  - A group-writable container is only shared with US when it is one of OUR groups. userdata
 *    after a boot is 1000:1000 0775, and the app (gid jailer plus system groups) is not in 1000,
 *    so no other app can swap our profile inside it. World-writable without sticky stays unsafe.
 *  - Our own profile directory comes back group-writable to jailer, which other jailed apps and
 *    the developer login share. We own it, so we close it again (0700) before using it; with
 *    the directory closed, the group-writable files inside are unreachable to anyone else.
 */
static bool path_in_my_groups(gid_t gid) {
    if (gid == getegid()) return true;
    gid_t groups[64];
    int count = getgroups((int) (sizeof(groups) / sizeof(groups[0])), groups);
    for (int i = 0; i < count; ++i) {
        if (groups[i] == gid) return true;
    }
    return false;
}

static bool path_container_shared(const struct stat *info) {
    if ((info->st_mode & S_ISVTX) != 0) return false;
    return (info->st_mode & 0002) != 0 ||
           ((info->st_mode & 0020) != 0 && path_in_my_groups(info->st_gid));
}

/* Validate the new managed namespace independently of legacy direct profiles.
 * Sticky shared containers protect native-owned child names from other UIDs;
 * their installer owner still has normal owner authority. Existing inherited
 * app-root permissions remain OS-owned; this is not a universal jail claim. */
static int path_managed_parent(const char *path, bool create) {
    if (path == NULL) return -1;
    char *parent = strdup(path);
    if (parent == NULL) return -1;
    char *leaf = strrchr(parent, '/');
    int fd = -1;
    struct stat info;
    if (leaf == NULL || leaf == parent) goto done;
    *leaf = 0;
    if (create && path_dir_ensure(parent) != 0) goto done;
    fd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0 && (fstat(fd, &info) != 0 || path_container_shared(&info))) {
        close(fd);
        fd = -1;
    }
done:
    free(parent);
    return fd;
}

static bool path_managed_existing(const char *path) {
    int fd = path_managed_parent(path, false);
    if (fd < 0) return false;
    const char *leaf = strrchr(path, '/') + 1;
    struct stat info;
    bool valid = fstatat(fd, leaf, &info, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISDIR(info.st_mode) && info.st_uid == geteuid();
    /* Ours but reopened by the boot-time reset (see path_container_shared): close it again, and
     * only trust it if the mode really is private now. */
    if (valid && (info.st_mode & 0022) != 0) {
        valid = fchmodat(fd, leaf, 0700, 0) == 0 &&
            fstatat(fd, leaf, &info, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(info.st_mode) &&
            info.st_uid == geteuid() && (info.st_mode & 0022) == 0;
    }
    valid = valid && faccessat(fd, leaf, W_OK | X_OK, 0) == 0;
    close(fd);
    return valid;
}

/* New conf/cache are native-owned0700. Never adopt another UID's child or
 * follow a symlink; preserve safe existing and atomically moved directory modes. */
static int path_managed_ensure(const char *path) {
    /* AN EXISTING DIRECTORY OF OURS IS USED AS IT IS (re-closed if a boot reopened it). After a
     * webOS 26 boot userdata/ is 1000:1000 0775, which the app cannot write, and the create path
     * below (path_dir_ensure() demands a WRITABLE parent) then refused a cache directory that
     * already existed. Measured 2026-10-10: the release runtime folder, and with it every co-op
     * start ("the second co-op PC did not begin streaming"), failed that way after a reboot. */
    if (path_managed_existing(path)) return 0;
    int fd = path_managed_parent(path, true);
    if (fd < 0) return -1;
    const char *leaf = strrchr(path, '/') + 1;
    int result = mkdirat(fd, leaf, 0700);
    if (result != 0 && errno != EEXIST) { close(fd); return -1; }
    close(fd);
    return path_managed_existing(path) ? 0 : -1;
}

static char *path_pref_select(char *confdir, char *managed, char *fallback, bool *persistent) {
    char *selected = NULL;
    struct stat info;
    *persistent = true;
    if (path_pref_existing(confdir)) selected = confdir;
    else if (path_managed_existing(managed)) selected = managed;
    else if (fallback != NULL && lstat(fallback, &info) == 0 &&
            S_ISDIR(info.st_mode) && access(fallback, W_OK | X_OK) == 0) {
        if (path_pref_migrate(confdir, fallback, false)) selected = confdir;
        else {
            if (path_pref_existing(confdir)) selected = confdir;
            /* Validate only the container; creating conf here would block
             * no-replace migration and hide the existing external profile. */
            int container = selected == NULL ? path_managed_parent(managed, true) : -1;
            if (container >= 0) {
                close(container);
                if (path_pref_migrate(managed, fallback, true)) selected = managed;
            }
            if (selected == NULL) {
                if (path_pref_existing(confdir)) selected = confdir;
                else if (path_managed_existing(managed)) selected = managed;
                else selected = fallback;
            }
        }
    } else if (path_dir_ensure(confdir) == 0) selected = confdir;
    else if (path_managed_ensure(managed) == 0) selected = managed;
    else if (fallback != NULL && path_dir_ensure(fallback) == 0) selected = fallback;
    else {
        selected = strdup("/tmp/moonlight-tv/conf");
        path_dir_ensure(selected);
        *persistent = false;
    }
    if (selected != confdir) free(confdir);
    if (selected != managed) free(managed);
    if (selected != fallback) free(fallback);
    return selected;
}

char *path_pref(bool *persistent) {
    const char *basedir = SDL_getenv("HOME");
    return path_pref_select(path_join(basedir, "conf"),
                            path_join(basedir, "userdata/conf"),
                            path_devtemp(basedir, "conf"), persistent);
}

static char *path_cache_select_owned(char *cachedir, char *managed) {
    if (path_dir_ensure(cachedir) == 0) {
        free(managed);
        return cachedir;
    }
    free(cachedir);
    if (path_managed_ensure(managed) == 0) return managed;
    free(managed);
    return NULL;
}

static char *path_cache_select(char *cachedir, char *managed) {
    cachedir = path_cache_select_owned(cachedir, managed);
    if (cachedir != NULL) return cachedir;
    cachedir = strdup("/tmp/moonlight-tv/cache");
    path_dir_ensure(cachedir);
    return cachedir;
}

char *path_cache() {
    const char *basedir = SDL_getenv("HOME");
    return path_cache_select(path_join(basedir, "cache"), path_join(basedir, "userdata/cache"));
}

static bool path_owned_dir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    bool owned = fd >= 0 && fstat(fd, &st) == 0 && st.st_uid == geteuid();
    if (fd >= 0) close(fd);
    return owned;
}

/* Takes ownership of all three. devtemp may be NULL. */
static char *path_runtime_cache_select(char *direct, char *managed, char *devtemp) {
    if (direct != NULL && path_dir_ensure(direct) == 0 && path_owned_dir(direct)) {
        free(managed);
        free(devtemp);
        return direct;
    }
    free(direct);
    if (managed != NULL && path_managed_ensure(managed) == 0) {
        free(devtemp);
        return managed;
    }
    free(managed);
    /* THE SAME FALLBACK THE SETTINGS FOLDER HAS. If the TV reboots between installing and first
     * opening the app, userdata/ is already 1000:1000 0775 (the boot reset) and nothing can be
     * created in it, so neither conf nor cache exists there. Settings then live in
     * /media/developer/temp/<app id>/conf; without this the runtime folder -- co-op's sockets
     * and the diagnostics file -- had no home at all and co-op could never start on that
     * install. runtime_init() still demands an owned directory and closes it to 0700. */
    if (devtemp != NULL && path_dir_ensure(devtemp) == 0 && path_owned_dir(devtemp)) {
        return devtemp;
    }
    free(devtemp);
    return NULL;
}

char *path_runtime_cache(void) {
    const char *basedir = SDL_getenv("HOME");
    if (basedir == NULL || basedir[0] != '/') return NULL;
    return path_runtime_cache_select(path_join(basedir, "cache"), path_join(basedir, "userdata/cache"),
                                     path_devtemp(basedir, "cache"));
}

bool path_self_executable(char *dest, size_t maxlen) {
    if (dest == NULL || maxlen < 2) {
        return false;
    }
    /* Ask the kernel which image is running. This works for live/beta package
     * ids without reconstructing webOS's install root or binary name and is the
     * same platform seam the future Linux build uses. */
    const ssize_t written = readlink("/proc/self/exe", dest, maxlen - 1);
    if (written < 0 || (size_t) written >= maxlen - 1) {
        return false;
    }
    dest[written] = '\0';
    return true;
}

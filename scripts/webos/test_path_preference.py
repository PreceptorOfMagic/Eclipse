#!/usr/bin/env python3
"""Compile actual webOS selection/path code; exercise only private filesystem paths."""
from pathlib import Path
import os, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[2]
if os.geteuid() == 0:
    raise SystemExit('Run unprivileged: write-permission regressions are invalid as root')
with tempfile.TemporaryDirectory(prefix='eclipse-webos-path-') as temp:
    base = Path(temp)
    (base/'SDL.h').write_text('#include <stdlib.h>\n#include <limits.h>\n#define SDL_getenv getenv\n')
    (base/'probe.c').write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "app/platform/webos/path_webos.c"
static const char *fixture_root;
static const char *fault;
long __real_syscall(long, ...);
int __real_fstat(int, struct stat *);
int __real_fstatat(int, const char *, struct stat *, int);
int __wrap_fstatat(int fd, const char *name, struct stat *st, int flags) {
    int result = __real_fstatat(fd, name, st, flags);
    if (result == 0 && strcmp(fault, "foreignmanaged") == 0 && strcmp(name, "conf") == 0)
        st->st_uid = geteuid() + 1;
    return result;
}
int __wrap_fstat(int fd, struct stat *st) {
    int result = __real_fstat(fd, st);
    if (result == 0 && strcmp(fault, "foreign") == 0) st->st_uid = geteuid() + 1;
    if (result == 0 && (strcmp(fault, "foreigngroup") == 0 || strcmp(fault, "runtime-bootreset") == 0 ||
                        strcmp(fault, "runtime-devtemp") == 0)) {
        /* webOS 26 after a boot: userdata is 1000:1000 0775, a group the app is not in. */
        char proc[64], path[4096];
        snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
        ssize_t count = readlink(proc, path, sizeof(path) - 1);
        if (count >= 9) {
            path[count] = 0;
            if (strcmp(path + count - 9, "/userdata") == 0) st->st_gid = (gid_t) 0x7ffffff0;
        }
    }
    if (result == 0 && strcmp(fault, "runtime-foreign") == 0) {
        char proc[64], path[4096];
        snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
        ssize_t count = readlink(proc, path, sizeof(path) - 1);
        if (count >= 0) {
            path[count] = 0;
            if (strstr(path, "/home/cache") != NULL) st->st_uid = geteuid() + 1;
        }
    }
    return result;
}
long __wrap_syscall(long number, ...) {
    if (number != SYS_renameat2) exit(93);
    va_list ap;
    va_start(ap, number);
    int oldfd = va_arg(ap, int);
    const char *oldname = va_arg(ap, const char *);
    int newfd = va_arg(ap, int);
    const char *newname = va_arg(ap, const char *);
    int flags = va_arg(ap, int);
    va_end(ap);
    if (strcmp(fault, "enosys") == 0) { errno = ENOSYS; return -1; }
    if (strcmp(fault, "exdev") == 0) { errno = EXDEV; return -1; }
    if (strcmp(fault, "eacces") == 0) { errno = EACCES; return -1; }
    if (strcmp(fault, "race") == 0 && mkdirat(newfd, newname, 0755) != 0) exit(94);
    return __real_syscall(number, oldfd, oldname, newfd, newname, flags);
}
int __real_path_dir_ensure(const char *);
int __wrap_path_dir_ensure(const char *path) {
    size_t len = strlen(fixture_root);
    if (strncmp(path, fixture_root, len) != 0 || (path[len] != '/' && path[len] != '\0')) {
        fprintf(stderr, "Refusing non-fixture directory: %s\n", path);
        exit(90);
    }
    return __real_path_dir_ensure(path);
}
int main(int argc, char **argv) {
    if (argc != 6) return 91;
    fixture_root = argv[1];
    fault = argv[5];
    bool persistent = false;
    char *selected;
    /* The runtime cache is probed through its selector so the developer-temp fallback can be a
     * fixture path: the real one is /media/developer/temp/<app id>/cache. Only runtime-devtemp
     * offers one; the other runtime cases keep "no fallback available". */
    if (strcmp(fault, "runtime-denied") == 0) {
        selected = path_runtime_cache_select(path_join(argv[2], "cache"), path_join(argv[2], "userdata/cache"), NULL);
        if (selected != NULL) { free(selected); return 96; }
        puts("REFUSED");
        return 0;
    }
    if (strcmp(fault, "runtime-foreign") == 0 || strcmp(fault, "runtime-bootreset") == 0 ||
        strcmp(fault, "runtime-devtemp") == 0) {
        char *devtemp = strcmp(fault, "runtime-devtemp") == 0 ? strdup(argv[4]) : NULL;
        selected = path_runtime_cache_select(path_join(argv[2], "cache"), path_join(argv[2], "userdata/cache"), devtemp);
        if (selected == NULL) return 97;
        puts(selected); free(selected); return 0;
    }
    if (strcmp(fault, "cache") == 0) {
        selected = path_cache_select(strdup(argv[2]), strdup(argv[3]));
        persistent = true;
    } else selected = path_pref_select(strdup(argv[2]), strdup(argv[3]), strdup(argv[4]), &persistent);
    if (!selected) return 92;
    printf("%d %s\n", (int)persistent, selected);
    free(selected);
    return 0;
}
''')
    subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror',
                    '-I'+str(base),'-I'+str(ROOT/'src'),'-I'+str(ROOT/'src/app'),
                    str(base/'probe.c'),str(ROOT/'src/app/util/path.c'),
                    '-Wl,--wrap=path_dir_ensure,--wrap=syscall,--wrap=fstat,--wrap=fstatat','-o',str(base/'probe')],check=True)
    def invoke(home, fallback, expected, fault="pass", managed=None):
        managed = managed or home.parent/'userdata'/home.name
        answer = subprocess.check_output([str(base/'probe'),str(base),str(home),str(managed),str(fallback),fault],text=True).strip()
        assert answer == '1 '+str(expected), answer
    def case(name):
        p=base/name;p.mkdir();return p/'home'/'conf',p/'external'/'conf'
    def marker(path, value):
        path.mkdir(parents=True);(path/'moonlight.ini').write_text(value)
    home, fall = case('fresh')
    invoke(home,fall,home);assert home.is_dir() and not fall.exists()
    print('PASS fresh install creates HOME profile only')
    home, fall = case('both')
    marker(home,'HOME');marker(fall,'EXTERNAL')
    invoke(home,fall,home)
    assert (home/'moonlight.ini').read_text()=='HOME' and (fall/'moonlight.ini').read_text()=='EXTERNAL'
    print('PASS existing HOME wins and neither profile content changes')
    home, fall = case('upgrade')
    home.parent.mkdir();home.parent.chmod(0o555);marker(fall,'PERSIST ACROSS UPGRADE')
    invoke(home,fall,fall);assert not home.exists()
    profile_inode=fall.stat().st_ino
    key=fall/'key.pem';key.write_text('SYNTHETIC FIXTURE ONLY');key.chmod(0o600)
    key_inode=key.stat().st_ino
    home.parent.chmod(0o755)
    invoke(home,fall,home)
    assert home.stat().st_ino==profile_inode and (home/'key.pem').stat().st_ino==key_inode
    assert (home/'key.pem').stat().st_mode & 0o777 == 0o600
    assert (home/'moonlight.ini').read_text()=='PERSIST ACROSS UPGRADE' and not fall.parent.exists()
    print('PASS root upgrade atomically moves owned profile; contents/inodes/key0600 preserved, empty parent removed')
    home, fall = case('unwritable-home')
    marker(home,'PRESERVE');home.chmod(0o555);marker(fall,'WORKING')
    invoke(home,fall,home.parent/'userdata'/'conf');home.chmod(0o755)
    assert (home/'moonlight.ini').read_text()=='PRESERVE'
    print('PASS unwritable existing HOME preserved while active fallback moves to nested storage')
    home, fall = case('unwritable-fallback')
    marker(fall,'UNWRITABLE');fall.chmod(0o555)
    invoke(home,fall,home);assert (fall/'moonlight.ini').read_text()=='UNWRITABLE';fall.chmod(0o755)
    print('PASS unusable fallback does not displace usable new HOME')
    home, fall = case('fallback-file')
    fall.parent.mkdir();fall.write_text('NOT A DIRECTORY')
    invoke(home,fall,home);assert fall.read_text()=='NOT A DIRECTORY'
    print('PASS fallback regular file is not selected')
    home, fall = case('fallback-symlink')
    target=base/'foreign-profile';marker(target,'FOREIGN')
    fall.parent.mkdir();fall.symlink_to(target,target_is_directory=True)
    invoke(home,fall,home);assert (target/'moonlight.ini').read_text()=='FOREIGN'
    print('PASS fallback symlink is not promoted over new HOME')
    home, fall = case('home-empty')
    home.mkdir(parents=True);marker(fall,'OLD LEFTOVER')
    invoke(home,fall,home);assert not (home/'moonlight.ini').exists()
    print('PASS existing empty HOME still wins over stale fallback')
    home, fall = case('home-blocked')
    home.parent.mkdir();home.parent.chmod(0o555)
    invoke(home,fall,fall);assert fall.is_dir() and not home.exists();home.parent.chmod(0o755)
    print('PASS failed HOME creation still creates prior fallback path')
    for fault in ['enosys','exdev','eacces']:
        home, fall=case('failure-'+fault)
        home.parent.mkdir();marker(fall,'PRESERVE '+fault)
        inode=fall.stat().st_ino
        invoke(home,fall,fall,fault)
        assert not home.exists() and fall.stat().st_ino==inode
        assert (fall/'moonlight.ini').read_text()=='PRESERVE '+fault
        print('PASS injected '+fault+' retains usable external profile without creating HOME')
    home,fall=case('racing-home')
    home.parent.mkdir();marker(fall,'DO NOT OVERWRITE')
    invoke(home,fall,home,'race')
    assert home.is_dir() and not list(home.iterdir())
    assert (fall/'moonlight.ini').read_text()=='DO NOT OVERWRITE'
    print('PASS destination created immediately before actual rename syscall is not replaced, even empty')
    home,fall=case('parent-other-data')
    home.parent.mkdir();marker(fall,'MOVE');(fall.parent/'unrelated').write_text('KEEP')
    invoke(home,fall,home)
    assert not fall.exists() and (fall.parent/'unrelated').read_text()=='KEEP'
    print('PASS migration retains non-empty external parent and unrelated files')
    home,fall=case('writable-parent')
    home.parent.mkdir();marker(fall,'KEEP');fall.parent.chmod(0o777)
    invoke(home,fall,fall);assert not home.exists();fall.parent.chmod(0o755)
    print('PASS shared-writable external parent is not migrated')
    home,fall=case('foreign-parent')
    home.parent.mkdir();marker(fall,'KEEP')
    invoke(home,fall,fall,'foreign');assert not home.exists()
    print('PASS synthetic foreign-UID parent refuses migration without changing profile')
    home,fall=case('nested-existing')
    nested=home.parent/'userdata'/'conf';marker(nested,'NESTED');marker(fall,'LEGACY')
    invoke(home,fall,nested);assert not home.exists()
    assert (fall/'moonlight.ini').read_text()=='LEGACY'
    print('PASS existing nested profile wins without new direct profile or merge')
    home,fall=case('direct-nested-existing')
    nested=home.parent/'userdata'/'conf';marker(home,'DIRECT');marker(nested,'NESTED');marker(fall,'LEGACY')
    invoke(home,fall,home)
    assert (nested/'moonlight.ini').read_text()=='NESTED' and (fall/'moonlight.ini').read_text()=='LEGACY'
    print('PASS existing direct profile wins over nested and external without changes')
    home,fall=case('nested-migration')
    nested=home.parent/'userdata'/'conf';nested.parent.mkdir(parents=True);nested.parent.chmod(0o1777)
    home.parent.chmod(0o555);marker(fall,'NESTED MIGRATION');inode=fall.stat().st_ino
    invoke(home,fall,nested)
    assert nested.stat().st_ino==inode and not fall.parent.exists() and not home.exists()
    assert (nested/'moonlight.ini').read_text()=='NESTED MIGRATION';home.parent.chmod(0o755)
    print('PASS read-only root migrates legacy inode into writable packaged userdata')
    for mode in (0o775, 0o777):
        home,fall=case('nested-shared-profile-'+oct(mode))
        nested=home.parent/'userdata'/'conf';nested.parent.mkdir(parents=True);nested.parent.chmod(0o1777)
        home.parent.chmod(0o555);marker(fall,'KEEP SHARED LEGACY');fall.chmod(mode)
        inode=fall.stat().st_ino
        invoke(home,fall,fall);invoke(home,fall,fall)
        assert fall.stat().st_ino==inode and not nested.exists() and not home.exists()
        assert (fall/'moonlight.ini').read_text()=='KEEP SHARED LEGACY'
        assert fall.stat().st_mode & 0o777 == mode
        home.parent.chmod(0o755)
        print('PASS shared legacy profile '+oct(mode)+' stays authoritative across two selections')
    home,fall=case('nested-fresh')
    nested=home.parent/'userdata'/'conf';nested.parent.mkdir(parents=True);nested.parent.chmod(0o1777)
    home.parent.chmod(0o555)
    invoke(home,fall,nested);assert nested.is_dir() and not home.exists() and not fall.exists()
    assert nested.stat().st_mode & 0o777 == 0o700
    home.parent.chmod(0o755)
    print('PASS read-only root creates nested profile before external storage')
    home,fall=case('nested-unsupported')
    nested=home.parent/'userdata'/'conf';nested.parent.mkdir(parents=True);home.parent.chmod(0o555)
    marker(fall,'UNCHANGED');invoke(home,fall,fall,'enosys')
    assert not nested.exists() and not home.exists() and (fall/'moonlight.ini').read_text()=='UNCHANGED'
    home.parent.chmod(0o755)
    print('PASS unsupported migration retains legacy profile without empty nested profile')
    home,fall=case('nested-cache')
    home=home.with_name('cache');nested=home.parent/'userdata'/'cache'
    nested.parent.mkdir(parents=True);home.parent.chmod(0o555)
    invoke(home,fall,nested,'cache',nested);assert nested.is_dir() and not home.exists()
    assert nested.stat().st_mode & 0o777 == 0o700
    home.parent.chmod(0o755)
    print('PASS read-only root uses nested cache before shared temporary storage')
    home,fall=case('unsafe-managed-parent')
    nested=home.parent/'userdata'/'conf';marker(nested,'UNTRUSTED');nested.parent.chmod(0o777)
    home.parent.chmod(0o555);marker(fall,'KEEP EXTERNAL')
    invoke(home,fall,fall);assert (nested/'moonlight.ini').read_text()=='UNTRUSTED'
    home.parent.chmod(0o755)
    print('PASS nonsticky shared userdata is neither adopted nor migrated into')
    # webOS 26 resets the developer app tree to 0775 at every boot (measured 2026-10-10).
    home,fall=case('after-boot-reset')
    nested=home.parent/'userdata'/'conf';marker(nested,'PAIRINGS');nested.chmod(0o775)
    nested.parent.chmod(0o775);home.parent.chmod(0o555);marker(fall,'EMPTY FALLBACK')
    invoke(home,fall,nested,'foreigngroup')
    assert (nested/'moonlight.ini').read_text()=='PAIRINGS'
    assert nested.stat().st_mode & 0o777 == 0o700
    assert (fall/'moonlight.ini').read_text()=='EMPTY FALLBACK'
    home.parent.chmod(0o755)
    print('PASS boot-reset profile (0775 in a 1000:1000 0775 userdata) is re-closed to 0700 and kept')
    home,fall=case('after-boot-reset-own-group')
    nested=home.parent/'userdata'/'conf';marker(nested,'UNTRUSTED');nested.chmod(0o775)
    nested.parent.chmod(0o775);home.parent.chmod(0o555);marker(fall,'KEEP EXTERNAL')
    invoke(home,fall,fall)
    assert (nested/'moonlight.ini').read_text()=='UNTRUSTED'
    assert nested.stat().st_mode & 0o777 == 0o775
    home.parent.chmod(0o755)
    print('PASS a userdata writable by one of our own groups is still refused and left untouched')
    home,fall=case('foreign-managed-child')
    nested=home.parent/'userdata'/'conf';marker(nested,'FOREIGN');home.parent.chmod(0o555)
    marker(fall,'KEEP EXTERNAL');invoke(home,fall,fall,'foreignmanaged')
    assert (nested/'moonlight.ini').read_text()=='FOREIGN';home.parent.chmod(0o755)
    print('PASS synthetic foreign-owned managed child is never adopted or overwritten')
    home,fall=case('runtime-cache-refusal')
    home.parent.mkdir(parents=True);home.parent.chmod(0o555)
    try:
        answer = subprocess.check_output([str(base/'probe'),str(base),str(home.parent),str(fall),str(fall),'runtime-denied'],text=True)
        assert answer.strip() == 'REFUSED'
        assert not list(home.parent.iterdir())
    finally:
        home.parent.chmod(0o755)
    print('PASS release runtime cache refuses unavailable owned paths without creating global fallback')
    # webOS 26 after a boot: userdata is 1000:1000 0775 -- group-writable by a group the app is not
    # in, and NOT writable by the app -- and our cache inside it is reopened to 0775.
    home,fall=case('runtime-cache-after-boot-reset')
    nested=home.parent/'userdata'/'cache';nested.mkdir(parents=True);nested.chmod(0o775)
    nested.parent.chmod(0o575);home.parent.chmod(0o555)
    try:
        answer = subprocess.check_output([str(base/'probe'),str(base),str(home.parent),str(fall),str(fall),'runtime-bootreset'],text=True)
        assert answer.strip() == str(nested), answer
        assert nested.stat().st_mode & 0o777 == 0o700
    finally:
        home.parent.chmod(0o755);nested.parent.chmod(0o755)
    print('PASS release runtime cache keeps the existing owned cache under a boot-reset, unwritable userdata')
    # Installed, then the TV rebooted BEFORE the first launch: userdata is already 1000:1000 0775 and
    # unwritable, so no cache can be created in it. The runtime folder falls back to developer temp,
    # as settings do, instead of having no home (which left co-op unable to start on that install).
    home,fall=case('runtime-devtemp-before-first-launch')
    (home.parent/'userdata').mkdir(parents=True);(home.parent/'userdata').chmod(0o575);home.parent.chmod(0o555)
    devtemp=fall.parent/'cache'
    try:
        answer = subprocess.check_output([str(base/'probe'),str(base),str(home.parent),str(fall),str(devtemp),'runtime-devtemp'],text=True)
        assert answer.strip() == str(devtemp), answer
        assert devtemp.is_dir() and not (home.parent/'userdata'/'cache').exists()
    finally:
        home.parent.chmod(0o755);(home.parent/'userdata').chmod(0o755)
    print('PASS runtime cache falls back to developer temp when userdata was reset before the first launch')
    home,fall=case('runtime-foreign-cache')
    direct=home.parent/'cache';direct.mkdir(parents=True);(direct/'canary').write_text('KEEP')
    nested=home.parent/'userdata'/'cache';nested.parent.mkdir();nested.parent.chmod(0o1777)
    answer = subprocess.check_output([str(base/'probe'),str(base),str(home.parent),str(fall),str(fall),'runtime-foreign'],text=True)
    assert answer.strip() == str(nested)
    assert nested.stat().st_mode & 0o777 == 0o700
    assert (direct/'canary').read_text() == 'KEEP'
    print('PASS foreign-owned direct cache is preserved while release runtime selects owned nested cache')
print('PASS all isolated paths removed; no TV/default HOME or developer-temp path used')

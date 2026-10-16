#!/usr/bin/env python3
"""Compile every C source of the Android app, as the Android build configures it, on the desktop.

Without an NDK this is the next best thing to building: each source file the Android build compiles into
the app is compiled with -fsyntax-only, with the desktop build's flags (compile_commands.json) turned into
the Android build's -- TARGET_ANDROID, OS_ANDROID, __ANDROID__, the Android feature switches from
CMakeLists.txt, the NDK's -Werror=format-security -- with the NDK media/window/log headers stubbed (tests/android/ndk-stubs) and the JDK's
jni.h. The C library's headers are the host's (glibc), with __GLIBC__ undefined so code that tests for
glibc takes its Android branch. Functions the Android C library lacks are not detected here.

    typecheck_android.py <source root> <build dir> <work dir> <jni include dir>...
"""
import json
import os
import re
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT, BUILD, WORK = sys.argv[1:4]
JNI_DIRS = sys.argv[4:]


def feature_switches():
    """FEATURE_* as the Android build sets them: CMakeLists.txt's defaults, then its TARGET_ANDROID block."""
    text = open(os.path.join(ROOT, 'CMakeLists.txt')).read()
    switches = {}
    defaults = re.search(r'(set\(FEATURE_\w+ (?:ON|OFF)\)\s*)+', text)
    for name, value in re.findall(r'set\((FEATURE_\w+) (ON|OFF)\)', defaults.group(0)):
        switches[name] = value == 'ON'
    # The `if (TARGET_ANDROID)` block that sets features: there are several such blocks.
    blocks = [m.group(1) for m in re.finditer(r'\nif \(TARGET_ANDROID\)\n(.*?)\n(?:elseif|else|endif)', text, re.S)]
    android = [b for b in blocks if 'set(FEATURE_' in b]
    if len(android) != 1:
        sys.exit('expected one TARGET_ANDROID block setting FEATURE_* in CMakeLists.txt, found %d' % len(android))
    for name, value in re.findall(r'set\((FEATURE_\w+) (ON|OFF)\)', android[0]):
        switches[name] = value == 'ON'
    return switches


def write_config():
    template = open(os.path.join(ROOT, 'src/app/config.h.in')).read()
    desktop = open(os.path.join(BUILD, 'config.h')).read()
    switches = feature_switches()
    out = []
    for line in template.splitlines():
        m = re.match(r'#cmakedefine01 (\w+)', line)
        if m:
            out.append('#define %s %d' % (m.group(1), 1 if switches.get(m.group(1), False) else 0))
        elif '@' in line:
            name = line.split()[1]
            out.append(re.search(r'#define %s .*' % name, desktop).group(0))
        else:
            out.append(line)
    os.makedirs(WORK, exist_ok=True)
    open(os.path.join(WORK, 'config.h'), 'w').write('\n'.join(out) + '\n')
    open(os.path.join(WORK, 'no_glibc.h'), 'w').write('#include <features.h>\n#undef __GLIBC__\n#undef __GLIBC_MINOR__\n')
    return switches


def android_flags(command):
    args = shlex.split(command)
    flags, skip = [], False
    for a in args[1:]:
        if skip:
            skip = False
            continue
        if a in ('-o', '-c', '-MF', '-MT', '-MQ'):
            skip = True
            continue
        if a.endswith('.c') or a.startswith('-MD') or a == '-fPIE' or a == '-DOS_LINUX' or \
                a.startswith('-DGAMECONTROLLERDB_PLATFORM') or a.startswith('-DAPPID'):
            continue
        flags.append(a)
    return args[0], flags


def main():
    switches = write_config()
    commands = json.load(open(os.path.join(BUILD, 'compile_commands.json')))
    app = [c for c in commands if '/src/app/' in c['file'] or c['file'].endswith('/src/main.c')]
    app = [c for c in app if '/tests/' not in c['file']]
    template = next(c for c in commands if c['file'].endswith('/src/app/app.c'))
    android_dir = os.path.join(ROOT, 'src/app/platform/android')
    extra = sorted(os.path.join(android_dir, f) for f in os.listdir(android_dir) if f.endswith('.c'))
    extra.append(os.path.join(ROOT, 'third_party/commons/util/logging/logging_android.c'))
    app += [dict(template, file=f) for f in extra]
    # Sources the Android build does not compile (each platform's own, or a backend Android replaces).
    skip = ('/platform/windows/', '/platform/webos/', '/platform/sdl/path_sdl.c', '/app_common.c',
            '/i18n_gettext.c')
    stubs = os.path.join(ROOT, 'tests/android/ndk-stubs/include')
    defines = ['-DTARGET_ANDROID=1', '-DOS_ANDROID=1', '-D__ANDROID__=1', '-D__ANDROID_API__=28',
               '-DAPPID="io.github.preceptorofmagic.eclipse"', '-DGAMECONTROLLERDB_PLATFORM="Android"',
               '-DECLIPSE_ANDROID_SELFTEST=1',
               # The NDK's own defaults (build/cmake/flags.cmake): a printf-style call whose format is not a
               # literal and has no arguments does not compile for Android.
               '-Wformat', '-Werror=format-security']
    def check(c):
        compiler, flags = android_flags(c['command'])
        cmd = [compiler, '-include', os.path.join(WORK, 'no_glibc.h'), '-I' + WORK] + flags + defines + \
              ['-I' + stubs] + ['-I' + d for d in JNI_DIRS] + ['-I' + android_dir, '-fsyntax-only']
        r = subprocess.run(cmd + [c['file']], cwd=c['directory'], capture_output=True, text=True)
        return c['file'], r

    todo = [c for c in app if not any(s in c['file'] for s in skip)]
    failures, checked = 0, len(todo)
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for path, r in pool.map(check, todo):
            if r.returncode != 0:
                failures += 1
                errors = [l for l in r.stderr.splitlines() if 'error' in l]
                print('FAIL %s\n    %s' % (os.path.relpath(path, ROOT), '\n    '.join(errors[:8])))
    on = sorted(k for k, v in switches.items() if v)
    print('Android type-check: %d sources, %d failed (features on: %s)' % (checked, failures, ', '.join(on)))
    return 1 if failures or checked < 100 else 0


if __name__ == '__main__':
    sys.exit(main())

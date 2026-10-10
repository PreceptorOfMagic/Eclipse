#!/usr/bin/env python3
"""Private real Pulse autospawn/auth test. Run as an existing non-root account.

HOME is retained unchanged; every writable XDG path and startup module is private.
No audio device or hardware module is opened. Only exact private daemon PID is stopped.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import pwd
import select
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def client():
    root = Path(os.environ['ECLIPSE_PULSE_TEST_ROOT'])
    conf = Path(os.environ['PULSE_CLIENTCONFIG'])
    assert conf.is_relative_to(root)
    # Exercise enabled autospawn even on distributions that disable it by default.
    with conf.open('a') as f:
        f.write('\nautospawn = yes\n')
    p = ctypes.CDLL('libpulse.so.0')
    signatures = [
        ('pa_mainloop_new', [], ctypes.c_void_p),
        ('pa_mainloop_get_api', [ctypes.c_void_p], ctypes.c_void_p),
        ('pa_context_new', [ctypes.c_void_p, ctypes.c_char_p], ctypes.c_void_p),
        ('pa_context_connect', [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p], ctypes.c_int),
        ('pa_mainloop_iterate', [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p], ctypes.c_int),
        ('pa_context_get_state', [ctypes.c_void_p], ctypes.c_int),
        ('pa_context_errno', [ctypes.c_void_p], ctypes.c_int),
        ('pa_strerror', [ctypes.c_int], ctypes.c_char_p),
        ('pa_context_disconnect', [ctypes.c_void_p], None),
        ('pa_context_unref', [ctypes.c_void_p], None),
        ('pa_mainloop_free', [ctypes.c_void_p], None),
    ]
    for name, args, result in signatures:
        fn = getattr(p, name)
        fn.argtypes, fn.restype = args, result
    loop = p.pa_mainloop_new()
    context = p.pa_context_new(p.pa_mainloop_get_api(loop), b'Eclipse isolated autospawn test')
    try:
        assert p.pa_context_connect(context, None, 0, None) == 0
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            p.pa_mainloop_iterate(loop, 0, None)
            state = p.pa_context_get_state(context)
            if state == 4:
                break
            if state in (5, 6):
                raise RuntimeError(p.pa_strerror(p.pa_context_errno(context)).decode())
            time.sleep(.01)
        assert p.pa_context_get_state(context) == 4, 'Pulse did not become READY'
        pidfile = root / 'runtime/pulse/pid'
        daemon_pid = int(pidfile.read_text().strip())
        app_cookie = (root / 'state/eclipse/pulse/cookie').read_bytes()
        daemon_cookie = (root / 'config/pulse/cookie').read_bytes()
        assert len(app_cookie) == len(daemon_cookie) == 256
        assert app_cookie != daemon_cookie, 'Expected independent freshly generated cookies'
        result = {
            'connected_ready': True,
            'uid': os.geteuid(),
            'home_unchanged': os.environ['HOME'],
            'daemon_pid': daemon_pid,
            'cookie_fd_target': os.readlink('/proc/self/fd/9'),
            'client_cookie_sha256': hashlib.sha256(app_cookie).hexdigest(),
            'daemon_cookie_sha256': hashlib.sha256(daemon_cookie).hexdigest(),
            'cookies_differ': True,
            'auth_anonymous': False,
            'audio_device_opened': False,
        }
        (root / 'result.json').write_text(json.dumps(result, indent=2))
    finally:
        p.pa_context_disconnect(context)
        p.pa_context_unref(context)
        p.pa_mainloop_free(loop)


def stop_owned(root):
    pidfile = root / 'runtime/pulse/pid'
    if not pidfile.exists():
        return 'already exited'
    assert pidfile.stat().st_uid == os.geteuid()
    pid = int(pidfile.read_text().strip())
    proc = Path('/proc') / str(pid)
    if not proc.exists():
        return 'already exited'
    assert proc.stat().st_uid == os.geteuid(), 'Refusing foreign UID'
    assert (proc / 'comm').read_text().strip() == 'pulseaudio', 'Refusing unexpected private PID'
    started = (proc / 'stat').read_text().rsplit(')', 1)[1].split()[19]
    fd = os.pidfd_open(pid)
    try:
        # Re-read the private PID record after acquiring a stable process handle.
        assert int(pidfile.read_text().strip()) == pid
        assert (proc / 'stat').read_text().rsplit(')', 1)[1].split()[19] == started, 'PID identity changed'
        poll = select.poll()
        poll.register(fd, select.POLLIN)
        signal.pidfd_send_signal(fd, signal.SIGTERM)
        if not poll.poll(5000):
            signal.pidfd_send_signal(fd, signal.SIGKILL)
            assert poll.poll(2000), 'Private daemon did not exit'
            return 'forced exit (test failure)'
        return 'SIGTERM exited'
    finally:
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--launcher-dir', type=Path, default=Path(__file__).resolve().parents[2] / 'deploy/linux-portable')
    args = parser.parse_args()
    assert os.geteuid() != 0, 'Autospawn must be tested as a non-root account'
    assert os.environ.get('HOME') == pwd.getpwuid(os.geteuid()).pw_dir, 'Keep the account normal HOME value'
    assert not Path(os.environ['HOME']).exists(), 'Use the existing nobody account with absent HOME for this fresh test'
    assert shutil.which('pulseaudio'), 'Installed Pulse daemon required'
    assert hasattr(os, 'pidfd_open') and hasattr(signal, 'pidfd_send_signal')
    root = Path(tempfile.mkdtemp(prefix='eclipse-pulse-autospawn-', dir='/tmp'))
    print('Fixture:', root, 'UID:', os.geteuid(), flush=True)
    cleanup_complete = False
    try:
        for name in ['config', 'data', 'state', 'cache', 'runtime', 'app/bin']:
            (root / name).mkdir(mode=0o700, parents=True)
        for name in ['eclipse', 'package-functions.sh']:
            shutil.copy2(args.launcher_dir / name, root / 'app' / name)
        stub = root / 'app/bin/moonlight-tv'
        stub.write_text('#!/bin/sh\nexec ' + shlex.quote(sys.executable) + ' ' + shlex.quote(str(Path(__file__).resolve())) + ' --client\n')
        stub.chmod(0o700)
        startup = root / 'private.pa'
        startup.write_text('load-module module-native-protocol-unix auth-anonymous=0\nload-module module-null-sink\n')
        env = {
            'PATH': '/usr/bin:/bin', 'HOME': os.environ['HOME'], 'LANG': 'C.UTF-8',
            'XDG_CONFIG_HOME': str(root / 'config'), 'XDG_DATA_HOME': str(root / 'data'),
            'XDG_STATE_HOME': str(root / 'state'), 'XDG_CACHE_HOME': str(root / 'cache'),
            'XDG_RUNTIME_DIR': str(root / 'runtime'), 'PULSE_SCRIPT': str(startup),
            'ECLIPSE_PULSE_TEST_ROOT': str(root),
        }
        assert not (root / 'runtime/pulse/pid').exists()
        run = subprocess.run([str(root / 'app/eclipse')], env=env, capture_output=True, timeout=20)
        if run.returncode:
            log = root / 'state/eclipse/eclipse.log'
            raise RuntimeError(run.stderr.decode(errors='replace') + (log.read_text(errors='replace') if log.exists() else ''))
        result = json.loads((root / 'result.json').read_text())
        result['daemon_cleanup'] = stop_owned(root)
        assert result['daemon_cleanup'] != 'forced exit (test failure)'
        cleanup_complete = True
        result['daemon_cookie_scope'] = 'Daemon-owned shared-service state, inside this disposable private XDG root; not an Eclipse-owned cookie.'
        print(json.dumps(result, indent=2), flush=True)
    finally:
        if not cleanup_complete:
            try:
                print('Cleanup:', stop_owned(root), flush=True)
                cleanup_complete = True
            except Exception as exc:
                print('CLEANUP FAILED; private fixture retained:', root, repr(exc), flush=True)
        if cleanup_complete:
            shutil.rmtree(root)
            print('Private fixture files removed; no hardware modules or actual Eclipse client launched.', flush=True)


if __name__ == '__main__':
    if sys.argv[1:] == ['--client']:
        client()
    else:
        main()

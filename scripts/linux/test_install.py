#!/usr/bin/env python3
"""Isolated installer regression tests; no real app, streaming or user profile mutation."""
import hashlib
import contextlib
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def run(script, env, *args, success=True):
    result = subprocess.run([str(script), *args], env=env, text=True, capture_output=True)
    if (result.returncode == 0) != success:
        raise AssertionError(f'{script}: {result.returncode}\n{result.stdout}\n{result.stderr}')
    return result


def stop_process(proc):
    if proc.poll() is None:
        proc.kill()
    proc.wait()


with tempfile.TemporaryDirectory(prefix='eclipse-install-test-') as tmp, contextlib.ExitStack() as processes:
    root = Path(tmp)
    home = root / 'home space'
    home.mkdir()
    env = dict(os.environ, HOME=str(home), XDG_DATA_HOME=str(home / 'data'),
               XDG_STATE_HOME=str(home / 'state'), XDG_CACHE_HOME=str(home / 'cache'))
    package = root / 'portable "quote" $dollar `tick` %percent \\slash café'
    shutil.copytree(ROOT / 'deploy/linux-portable', package)
    (package / 'bin').mkdir()
    shutil.copy(shutil.which('python3'), package / 'bin/moonlight-tv')
    (package / '.eclipse-package').write_text('eclipse-linux-v1\n')
    # Desktop launch exercises GLib's actual Desktop Entry parser, not a copy of our escaping.
    marker = root / 'launched'
    (package / 'eclipse').write_text('#!/bin/sh\nprintf done > ' + shlex.quote(str(marker)) + '\n')
    for size in (16, 24, 32, 48, 64, 128, 256):
        icon = package / f'share/icons/hicolor/{size}x{size}/apps/eclipse.png'
        icon.parent.mkdir(parents=True)
        icon.write_bytes(b'test icon')

    def sums(folder):
        entries = []
        for path in sorted(folder.rglob('*')):
            if path.is_file() and path.name != 'SHA256SUMS':
                # sha256sum's escaping when a filename contains a backslash is immaterial here:
                # package-relative fixture names are all ordinary.
                entries.append(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(folder)}\n')
        (folder / 'SHA256SUMS').write_text(''.join(entries))

    sums(package)
    # Canonical root aliases and symlinked owned parents must fail before mutation.
    for alias in ('//', '/tmp/..'):
        run(package / 'install.sh', dict(env, XDG_DATA_HOME=alias), success=False)
    owned_parent = home / 'data/eclipse'
    owned_parent.parent.mkdir(parents=True)
    outside = root / 'outside-install'
    outside.mkdir()
    owned_parent.symlink_to(outside, target_is_directory=True)
    run(package / 'install.sh', env, success=False)
    assert not list(outside.iterdir())
    owned_parent.unlink()
    run(package / 'install-desktop-entry.sh', env)
    desktop = home / 'data/applications/eclipse.desktop'
    result = subprocess.run(['gio', 'launch', str(desktop)], env=env, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr + desktop.read_text()
    for _ in range(30):
        if marker.exists():
            break
        time.sleep(.1)
    assert marker.read_text() == 'done', 'Desktop Exec failed in a path containing shell metacharacters'
    run(package / 'install.sh', env)
    dest = home / 'data/eclipse/app'
    assert dest.is_dir() and str(dest).replace('\\', '\\\\') in desktop.read_text()
    # Refreshing the installed package is idempotent.
    run(dest / 'install.sh', env)
    # A running old version is closed before replacing its files.
    old = subprocess.Popen(['python3', '-c', 'import time; time.sleep(120)'], executable=str(dest / 'bin/moonlight-tv'), env=env)
    processes.callback(stop_process, old)
    try:
        run(package / 'install.sh', env)
        assert old.wait(timeout=3) != 0
    finally:
        if old.poll() is None:
            old.kill()
            old.wait()
    data_dirs = [home / 'data/com.limelight/moonlight-tv', home / 'state/eclipse', home / 'cache/moonlight-tv']
    for directory in data_dirs:
        directory.mkdir(parents=True)
        (directory / 'canary').write_text('owned data')
    settings_parent = home / 'data/com.limelight'
    outside_settings = root / 'outside-settings'
    settings_parent.rename(outside_settings)
    settings_parent.symlink_to(outside_settings, target_is_directory=True)
    run(dest / 'uninstall.sh', env, success=False)
    assert dest.exists() and (outside_settings / 'moonlight-tv/canary').exists()
    settings_parent.unlink()
    outside_settings.rename(settings_parent)
    unrelated = home / 'data/another-app'
    unrelated.mkdir()
    (unrelated / 'canary').write_text('keep')
    # Uninstall only closes this exact executable path, not an unrelated process with the same name.
    other = root / 'unrelated/moonlight-tv'
    other.parent.mkdir()
    shutil.copy(shutil.which('python3'), other)
    other_proc = subprocess.Popen(['python3', '-c', 'import time; time.sleep(120)'], executable=str(other), env=env)
    processes.callback(stop_process, other_proc)
    ready = root / 'stubborn-ready'
    stubborn = ('import signal,time; from pathlib import Path; '
                'signal.signal(signal.SIGTERM, signal.SIG_IGN); '
                f'Path({str(ready)!r}).touch(); time.sleep(120)')
    app_proc = subprocess.Popen(['python3', '-c', stubborn], executable=str(dest / 'bin/moonlight-tv'), env=env)
    processes.callback(stop_process, app_proc)
    for _ in range(50):
        if ready.exists():
            break
        time.sleep(.1)
    assert ready.exists(), 'Stubborn uninstall fixture did not start'
    try:
        run(dest / 'uninstall.sh', env, '--keep-settings')
        assert app_proc.wait(timeout=3) == -9 and other_proc.poll() is None
        assert not dest.exists() and not desktop.exists()
        assert all((path / 'canary').exists() for path in data_dirs)
        run(package / 'install.sh', env)
        # Portable uninstall must preserve the installed copy's menu entry.
        run(package / 'uninstall.sh', env, '--keep-settings')
        assert desktop.exists() and dest.exists() and not package.exists()
        run(dest / 'uninstall.sh', env)
        assert not dest.exists() and not desktop.exists()
        assert not any(path.exists() for path in data_dirs)
        assert (unrelated / 'canary').read_text() == 'keep'
    finally:
        for proc in (other_proc, app_proc):
            if proc.poll() is None:
                proc.kill()
            proc.wait()
    # A copied uninstall helper without its package marker refuses self-deletion.
    rogue = root / 'not-a-package'
    shutil.copytree(ROOT / 'deploy/linux-portable', rogue)
    run(rogue / 'uninstall.sh', env, success=False)
    assert rogue.exists()
    bad_env = dict(env, XDG_DATA_HOME='relative/path')
    run(rogue / 'install.sh', bad_env, success=False)
print('PASS: special-path desktop launch, verified install/update, exact-process shutdown, full uninstall, keep-settings, ownership and unsafe-path guards')

#!/usr/bin/env python3
"""Exercise the real system libpulse against private authentication-only UNIX sockets.

Requires Linux, Python3 and libpulse.so.0. No Pulse daemon, audio device, Eclipse
client, physical display or real profile is used. All fixture files are removed.
"""
from pathlib import Path
import os, shutil, tempfile, subprocess, json, hashlib, stat
root = Path(__file__).resolve().parents[2] / 'deploy/linux-portable'
with tempfile.TemporaryDirectory(prefix='epulse-', dir='/tmp') as tmp:
    top = Path(tmp)
    app = top / 'app'
    (app / 'bin').mkdir(parents=True)
    for n in ['eclipse', 'package-functions.sh']:
        shutil.copy2(root / n, app / n)
    shutil.copy2(Path(__file__).with_name('pulse_cookie_probe.py'), app / 'bin/moonlight-tv')
    (app / 'bin/moonlight-tv').chmod(493)
    helpers = top / 'tools'
    helpers.mkdir()
    (helpers / 'zenity').write_text('#!/bin/sh\nexit 0\n')
    (helpers / 'zenity').chmod(493)

    def make(name):
        home = top / name
        home.mkdir()
        env = os.environ.copy()
        for k in ['PULSE_COOKIE', 'PULSE_CLIENTCONFIG', 'PULSE_CONFIG_PATH', 'PULSE_SERVER', 'DISPLAY', 'WAYLAND_DISPLAY', 'MESA_SHADER_CACHE_DIR', '__GL_SHADER_DISK_CACHE_PATH']:
            env.pop(k, None)
        env.update(HOME=str(home), XDG_CONFIG_HOME=str(home / 'config'), XDG_DATA_HOME=str(home / 'data'), XDG_STATE_HOME=str(home / 'state'), XDG_CACHE_HOME=str(home / 'cache'), XDG_RUNTIME_DIR=str(home / 'runtime'), PROBE_OUT=str(home), PATH=str(helpers) + ':' + env['PATH'])
        return (home, env)

    def cookie(f, b):
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(b * 256)
        return f.read_bytes()

    def run(h, e, expected=None, good=True):
        r = subprocess.run([str(app / 'eclipse')], env=e, capture_output=True, timeout=8)
        assert (r.returncode == 0) == good, (h.name, r.returncode, r.stderr.decode(), (h / 'state/eclipse/eclipse.log').read_text() if (h / 'state/eclipse/eclipse.log').exists() else '')
        if not good:
            assert not (h / 'auth-packet').exists()
            print(h.name, 'PASS failure before libpulse')
            return
        got = json.loads((h / 'environment.json').read_text())
        packet = (h / 'auth-packet').read_bytes()
        if expected is None:
            expected = (h / 'state/eclipse/pulse/cookie').read_bytes()
        assert expected in packet, (h.name, 'auth does not match selected cookie')
        print(h.name, 'PASS actual libpulse auth selected expected cookie')
        return got
    h, e = make('fresh space')
    g = run(h, e)
    assert g['PULSE_CLIENTCONFIG'] == str(h / 'state/eclipse/pulse/client.conf')
    assert g['PULSE_COOKIE'] is None
    assert g['held_cookie'] == str(h / 'state/eclipse/pulse/cookie')
    assert not (h / 'config').exists()
    assert stat.S_IMODE((h / 'state/eclipse/pulse/cookie').stat().st_mode) == 384
    h, e = make('default')
    f = h / 'config/pulse/cookie'
    b = cookie(f, b'A')
    g = run(h, e, b)
    assert f.read_bytes() == b and g['PULSE_CLIENTCONFIG'] is None and (not (h / 'state/eclipse/pulse').exists())
    h, e = make('legacy')
    f = h / '.pulse-cookie'
    b = cookie(f, b'B')
    g = run(h, e, b)
    assert f.read_bytes() == b and g['PULSE_CLIENTCONFIG'] is None and (not (h / 'config').exists())
    h, e = make('explicitcookie')
    f = h / 'auth'
    b = cookie(f, b'C')
    e['PULSE_COOKIE'] = str(f)
    g = run(h, e, b)
    assert g['PULSE_COOKIE'] == str(f) and g['PULSE_CLIENTCONFIG'] is None
    h, e = make('explicitconfig')
    f = h / 'auth'
    b = cookie(f, b'D')
    conf = h / 'custom.conf'
    conf.write_text('cookie-file = ' + str(f) + '\n')
    e['PULSE_CLIENTCONFIG'] = str(conf)
    g = run(h, e, b)
    assert g['PULSE_CLIENTCONFIG'] == str(conf) and (not (h / 'state/eclipse/pulse').exists())
    h, e = make('localdropin')
    a = h / 'authA'
    b = h / 'authB'
    cookie(a, b'E')
    expected = cookie(b, b'F')
    conf = h / '.pulse/client.conf'
    conf.parent.mkdir()
    conf.write_text('cookie-file = ' + str(a) + '\n')
    drop = Path(str(conf) + '.d')
    drop.mkdir()
    (drop / '20-auth.conf').write_text('cookie-file = ' + str(b) + '\n')
    before = {x: hashlib.sha256(x.read_bytes()).hexdigest() for x in [a, b, conf, drop / '20-auth.conf']}
    g = run(h, e, expected)
    assert all((hashlib.sha256(f.read_bytes()).hexdigest() == v for f, v in before.items()))
    assert not (h / 'config').exists()
    h, e = make('configpath')
    a = h / 'auth'
    expected = cookie(a, b'G')
    conf = h / 'chosen/client.conf'
    conf.parent.mkdir()
    conf.write_text('cookie-file = ' + str(a) + '\n')
    e['PULSE_CONFIG_PATH'] = str(conf.parent)
    run(h, e, expected)
    assert not (h / 'config').exists() and not (h / 'state/eclipse/pulse').exists()
    h, e = make('symlinkconfig')
    a = h / 'auth'
    expected = cookie(a, b'H')
    local = h / '.pulse'
    local.mkdir()
    original = h / 'elsewhere/config'
    original.parent.mkdir()
    original.write_text('.include auth.conf\n')
    (local / 'client.conf').symlink_to(original)
    (local / 'auth.conf').write_text('cookie-file = ' + str(a) + '\n')
    run(h, e, expected)
    for name, key in [('emptycookie', 'PULSE_COOKIE'), ('emptyconfig', 'PULSE_CLIENTCONFIG')]:
        h, e = make(name)
        expected = cookie(h / 'config/pulse/cookie', b'J')
        e[key] = ''
        g = run(h, e, expected)
        assert g[key] == '' and (not (h / 'state/eclipse/pulse').exists())
    h, e = make('concurrent')
    jobs = []
    for i in range(2):
        output = h / str(i)
        output.mkdir()
        ee = dict(e, PROBE_OUT=str(output))
        jobs.append((output, subprocess.Popen([str(app / 'eclipse')], env=ee, stdout=subprocess.PIPE, stderr=subprocess.PIPE)))
    for output, job in jobs:
        stdout, stderr = job.communicate(timeout=8)
        assert job.returncode == 0, stderr
    expected = (h / 'state/eclipse/pulse/cookie').read_bytes()
    assert len(expected) == 256
    for output, _ in jobs:
        assert expected in (output / 'auth-packet').read_bytes()
    assert not (h / 'config').exists()
    print('concurrent PASS same persistent cookie and no global file')
    h, e = make('ownedlink')
    d = h / 'state/eclipse'
    d.mkdir(parents=True)
    (h / 'outside').mkdir()
    (d / 'pulse').symlink_to(h / 'outside')
    run(h, e, good=False)
    assert not list((h / 'outside').iterdir())
    h, e = make('comment#path')
    run(h, e)
    assert not (h / 'config').exists()
    h, e = make('semicolon;path')
    run(h, e)
    assert not (h / 'config').exists()
    h, e = make('unicode café')
    run(h, e)
    assert not (h / 'config').exists()
    h, e = make('custom-special')
    conf = h / 'custom#dir/client.conf'
    conf.parent.mkdir()
    expected = cookie(h / 'auth', b'K')
    conf.write_text('cookie-file = ' + str(h / 'auth') + '\n')
    e['PULSE_CONFIG_PATH'] = str(conf.parent)
    run(h, e, expected)
    assert not (h / 'state/eclipse/pulse').exists()
    h, e = make('distinct-concurrent')
    jobs = []
    for i, value in enumerate([b'L', b'M']):
        output = h / str(i)
        output.mkdir()
        expected = cookie(output / 'auth', value)
        (output / 'client.conf').write_text('cookie-file = ' + str(output / 'auth') + '\n')
        ee = dict(e, PROBE_OUT=str(output), PULSE_CONFIG_PATH=str(output))
        jobs.append((output, expected, subprocess.Popen([str(app / 'eclipse')], env=ee, stdout=subprocess.PIPE, stderr=subprocess.PIPE)))
    for output, expected, job in jobs:
        stdout, stderr = job.communicate(timeout=8)
        assert job.returncode == 0, stderr
        assert expected in (output / 'auth-packet').read_bytes()
    assert not (h / 'state/eclipse/pulse').exists() and not (h / 'config').exists()
    print('distinct-concurrent PASS each explicit configuration retained its own authentication')
    for name, broken in [('broken-dropin', 'broken'), ('unreadable-dropin', 'unreadable'), ('malformed-dropin', 'malformed')]:
        h, e = make(name)
        conf = h / '.pulse/client.conf'
        conf.parent.mkdir()
        conf.write_text('# preserved main config\n')
        drop = Path(str(conf) + '.d')
        drop.mkdir()
        if broken == 'broken':
            (drop / '01-bad.conf').symlink_to(h / 'missing')
        elif broken == 'unreadable':
            (drop / '01-bad.conf').write_text('# inaccessible drop-in\n')
            (drop / '01-bad.conf').chmod(0)
        else:
            (drop / '01-bad.conf').write_text('this is not a valid Pulse setting\n')
        expected = cookie(h / 'auth', b'N')
        (drop / '20#;good.conf').write_text('.include ../auth.conf\n')
        (conf.parent / 'auth.conf').write_text('cookie-file = ' + str(h / 'auth') + '\n')
        before = {x: x.read_bytes() for x in [conf, drop / '20#;good.conf', conf.parent / 'auth.conf']}
        g = run(h, e, expected)
        assert g['held_config_parent'] == str(conf.parent)
        assert (h / 'state/eclipse/pulse/client.conf.d').is_symlink()
        shutil.rmtree(h / 'state/eclipse')
        assert all(x.read_bytes() == content for x, content in before.items())
        assert drop.is_dir()
    h, e = make('broken-legacy')
    (h / '.pulse').mkdir()
    (h / '.pulse/client.conf').symlink_to(h / 'missing')
    conf = h / '.config/pulse/client.conf'
    conf.parent.mkdir(parents=True)
    expected = cookie(h / 'auth', b'O')
    conf.write_text('cookie-file = ' + str(h / 'auth') + '\n')
    run(h, e, expected)
    h, e = make('main#;parent')
    conf = h / '.pulse/client.conf'
    conf.parent.mkdir()
    conf.write_text('.include ../auth.conf\n')
    # An inherited directory handle must retain .. include resolution even when
    # the original directory name itself cannot be quoted by the Pulse parser.
    expected = cookie(top / 'special-parent-auth', b'P')
    (h / 'auth.conf').write_text('cookie-file = ' + str(top / 'special-parent-auth') + '\n')
    run(h, e, expected)
    assert not (h / 'config').exists()
    print('ALL22 cases passed; actual libpulse authentication packets only, no daemon/audio device/client launched; fixture tree removed.')

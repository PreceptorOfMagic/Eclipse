#!/usr/bin/env python3
"""Isolated filesystem, fake-SSH and IPK fixtures; never contact a TV."""
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('ipk_identity', ROOT / 'scripts/webos/ipk_app_id.py')
identity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(identity)


def ipk(path, app, control=None, installed=None, duplicate=False):
    members = {'debian-binary': b'2.0\n'}
    for archive, files in [('control.tar.gz', [('control', 'Package: ' + (control or app) + '\n')]),
                           ('data.tar.gz', [('usr/palm/applications/' + (installed or app) + '/appinfo.json', json.dumps({'id': app}))])]:
        output = io.BytesIO()
        with tarfile.open(fileobj=output, mode='w:gz') as tar:
            for name, content in files * (2 if duplicate and archive == 'data.tar.gz' else 1):
                data = content.encode(); entry = tarfile.TarInfo(name); entry.size = len(data)
                tar.addfile(entry, io.BytesIO(data))
        members[archive] = output.getvalue()
    data = b'!<arch>\n'
    for name, body in members.items():
        data += f'{name+"/":<16}{0:<12}{0:<6}{0:<6}{"100644":<8}{len(body):<10}`\n'.encode() + body
        if len(body) % 2: data += b'\n'
    path.write_bytes(data)


def run(*args, **kw):
    return subprocess.run(args, text=True, capture_output=True, timeout=10, **kw)


with tempfile.TemporaryDirectory(prefix='eclipse-profile-tools-') as tmp:
    base = Path(tmp); home = base / 'app root'; external = base / 'external'
    home.mkdir(); external.mkdir()
    helper = ROOT / 'scripts/lib/tv_profile.sh'
    def select():
        return run('bash', '-c', '. "$1"; _tv_existing_conf "$2" "$3"', 'fixture', str(helper), str(home), str(external))
    assert select().returncode == 1 and not list(home.iterdir()) and not list(external.iterdir())
    (external/'conf').mkdir(); assert select().stdout.strip() == str(external/'conf')
    (home/'userdata/conf').mkdir(parents=True); assert select().returncode == 2
    (external/'conf').rmdir(); assert select().stdout.strip() == str(home/'userdata/conf')
    (home/'conf').mkdir(); assert select().returncode == 2
    (home/'userdata/conf').rmdir(); (home/'conf').chmod(0o555)
    assert select().stdout.strip() == str(home/'conf'), 'prisoner access must not change precedence'
    (home/'conf').chmod(0o755); (home/'conf').rmdir(); (external/'conf').mkdir(); (home/'conf').symlink_to(external/'conf')
    assert select().returncode == 2 and (external/'conf').is_dir()
    (home/'conf').unlink()
    (home/'userdata').chmod(0o000)
    if os.geteuid() != 0:
        assert select().returncode == 2, 'unsearchable parent must not redirect to external'
    (home/'userdata').chmod(0o755)
    fake = base/'fake-ssh.py'
    fake.write_text('import subprocess,sys\ns=sys.stdin.read().replace("/media/developer/apps/usr/palm/applications",sys.argv[1]).replace("/media/developer/temp",sys.argv[2])\nsys.exit(subprocess.run(["sh","-s"],input=s,text=True).returncode)\n')
    apps = base/'apps'; fallbacks = base/'fallbacks'
    for app in ['com.aurora.gamestream', 'com.aurora.gamestream.beta']:
        (apps/app/'userdata/conf').mkdir(parents=True)
        result = run('bash','-c','. "$1"; resolve_tv_conf "$2" python3 "$3" "$4" "$5"','fixture',str(helper),app,str(fake),str(apps),str(fallbacks))
        assert result.returncode == 0 and result.stdout.strip() == str(apps/app/'userdata/conf')
    result = run('bash','-c','. "$1"; resolve_tv_conf "bad;touch unsafe" false','fixture',str(helper))
    assert result.returncode == 2
    for app in ['com.aurora.gamestream', 'com.aurora.gamestream.beta']:
        package = base/(app+'.ipk'); ipk(package,app); assert identity.app_id(str(package)) == app
    for kwargs in [{'control':'other'}, {'installed':'other'}, {'app':'bad;command'}, {'duplicate':True}]:
        package = base/'bad.ipk'; options={'app':'com.aurora.gamestream',**kwargs}; ipk(package,**options)
        try: identity.app_id(str(package))
        except ValueError: pass
        else: raise AssertionError('bad metadata accepted: '+str(kwargs))
    # Run the real deploy script with copied helpers and commands that only log.
    # A wrong-ID IPK must stop before any backup/install. A failed current backup
    # must keep the old snapshot and NEVER push it or chmod the TV profile.
    repo=base/'repo'; (repo/'dist').mkdir(parents=True); (repo/'scripts/tv').mkdir(parents=True)
    for name in ['scripts/lib/net_env.sh','scripts/lib/tv_profile.sh','scripts/webos/deploy_to_tv.sh','scripts/webos/ipk_app_id.py']:
        dst=repo/name; dst.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(ROOT/name,dst)
    sync=repo/'scripts/tv/sync_ares_device.sh'; sync.write_text('#!/bin/sh\nexit 0\n'); sync.chmod(0o755)
    user=base/'user'; commands=user/'.local/bin'; commands.mkdir(parents=True)
    log=base/'calls.txt'; backup=user/'.aurora-tv-conf-backup'; backup.mkdir(); (backup/'hosts.ini').write_text('old snapshot canary')
    for command in ['ssh','scp','ares-install']:
        p=commands/command
        if command=='ssh': body='case "$*" in *"sh -s") cat >/dev/null; echo /media/developer/apps/usr/palm/applications/com.aurora.gamestream/userdata/conf;; *) exit 0;; esac\n'
        elif command=='scp': body='exit 1\n'
        else: body='exit 0\n'
        p.write_text('#!/bin/sh\nprintf "%s\\n" "'+command+' $*" >> "'+str(log)+'"\n'+body); p.chmod(0o755)
    env=dict(os.environ,HOME=str(user),TV_HOST='fixture.invalid')
    target=repo/'dist/candidate.ipk'
    ipk(target,'com.aurora.gamestream.beta')
    result=run('bash',str(repo/'scripts/webos/deploy_to_tv.sh'),'--no-build',env=env)
    assert result.returncode != 0 and 'wrong-variant' in result.stderr and not log.exists(), result.stderr
    ipk(target,'com.aurora.gamestream')
    result=run('bash',str(repo/'scripts/webos/deploy_to_tv.sh'),'--no-build',env=env)
    assert result.returncode == 0 and 'backup INCOMPLETE' in result.stdout, (result.stdout,result.stderr)
    calls=log.read_text(); assert 'ares-install ' in calls and 'chmod' not in calls
    assert not any(line.startswith('scp ') and str(backup)+'/.' in line for line in calls.splitlines())
    assert (backup/'hosts.ini').read_text()=='old snapshot canary' and not Path(str(backup)+'.new').exists()
print('PASS single profile resolution/ambiguity refusal/no creation/symlink refusal/live-beta routing; IPK identity guards; deploy wrong-variant refusal/incomplete backup/no automatic restore or chmod')

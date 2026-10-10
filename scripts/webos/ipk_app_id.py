#!/usr/bin/env python3
"""Print the checked app identity from an IPK without extracting its files."""
import io
import json
import re
import subprocess
import sys
import tarfile


def app_id(ipk):
    members = subprocess.check_output(['ar', 't', ipk], text=True).splitlines()
    for name in ['debian-binary', 'control.tar.gz', 'data.tar.gz']:
        if members.count(name) != 1:
            raise ValueError('IPK must contain exactly one ' + name)
    with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(['ar', 'p', ipk, 'data.tar.gz'])), mode='r:gz') as archive:
        infos = [m for m in archive.getmembers() if m.name.lstrip('./').endswith('/appinfo.json')]
        if len(infos) != 1 or not infos[0].isfile() or infos[0].size > 65536:
            raise ValueError('IPK must contain one regular appinfo.json')
        identity = json.load(archive.extractfile(infos[0]))['id']
        if not isinstance(identity, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', identity) or identity in ('.', '..'):
            raise ValueError('Unsafe app ID')
        if infos[0].name.removeprefix('./') != 'usr/palm/applications/' + identity + '/appinfo.json':
            raise ValueError('appinfo identity disagrees with install path')
    with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(['ar', 'p', ipk, 'control.tar.gz'])), mode='r:gz') as archive:
        controls = [m for m in archive.getmembers() if m.name.removeprefix('./') == 'control']
        if len(controls) != 1 or not controls[0].isfile() or controls[0].size > 65536:
            raise ValueError('IPK must contain one regular control file')
        packages = [line.partition(':')[2].strip() for line in archive.extractfile(controls[0]).read().decode().splitlines() if line.startswith('Package:')]
        if packages != [identity]:
            raise ValueError('Control Package disagrees with appinfo identity')
    return identity


if __name__ == '__main__':
    try:
        if len(sys.argv) != 2:
            raise ValueError('usage: ipk_app_id.py PACKAGE.ipk')
        print(app_id(sys.argv[1]))
    except (OSError, ValueError, KeyError, tarfile.TarError, subprocess.SubprocessError) as error:
        sys.exit('IPK identity: ' + str(error))

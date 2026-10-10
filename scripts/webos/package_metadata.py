#!/usr/bin/env python3
"""Record the exact source/build and payload hashes after CPack staging/stripping."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

source, build, package = (Path(arg).resolve() for arg in sys.argv[1:])


def git(directory, *args):
    return subprocess.check_output(['git', '-C', str(directory), *args], text=True).strip()


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


info = json.loads((package / 'appinfo.json').read_text())
revision = git(source, 'rev-parse', 'HEAD')
status = git(source, 'status', '--porcelain=v1', '--untracked-files=all', '--ignore-submodules=none')
cache = {}
for line in (build / 'CMakeCache.txt').read_text().splitlines():
    match = re.match(r'([^#/:][^:]*):[^=]+=(.*)', line)
    if match:
        cache[match[1]] = match[2]
lines = ['Eclipse webOS source manifest', f'Root source revision: {revision}',
         f'Root status entries: {len(status.splitlines())}',
         'Modified source requires the accompanying exact source archive; a Git revision alone is insufficient.', '',
         'Resolved source dependencies (worktree delta SHA-256 includes repository-owned patches):']
for name in ('core/moonlight-common-c', 'core/moonlight-common-c/enet', 'third_party/commons',
             'third_party/lvgl', 'third_party/libmicrodns', 'third_party/ss4s'):
    directory = source / name
    if not (directory / ".git").exists():
        raise SystemExit(f"Uninitialized source dependency: {name}")
    delta = subprocess.check_output(['git', '-C', str(directory), 'diff', '--binary', '--no-ext-diff', 'HEAD'])
    lines.append(f'{name}: {git(directory, "rev-parse", "HEAD")} delta-sha256={hashlib.sha256(delta).hexdigest()}')
lines.extend(['', 'Repository patches and build recipes:'])
for path in sorted((source / 'patches/client-deps').glob('*')):
    if path.is_file():
        lines.append(f'{path.relative_to(source)} sha256={digest(path)}')
for name in ('scripts/bootstrap_client_deps.sh', 'scripts/webos/build_for_lg.sh',
             'scripts/webos/easy_build.sh', 'cmake/PackageWebOS.cmake'):
    lines.append(f'{name} sha256={digest(source / name)}')
lines.extend(['', 'ExternalProject resolved acquisition records:'])
for pattern in ('*-urlinfo.txt', '*-gitinfo.txt'):
    for record in sorted(build.rglob(pattern)):
        lines.append(str(record.relative_to(build)))
        for line in record.read_text().splitlines():
            # Never publish local account/build paths embedded in CMake's records.
            if re.match(r'^(url(?:\(s\))?|hash|repository|remote|tag|method)=', line):
                lines.append(f'  {line}')
for directory in sorted(build.glob('*-prefix/src/*')):
    if directory.is_dir() and (directory / '.git').exists():
        lines.append(f'{directory.relative_to(build)} revision={git(directory, "rev-parse", "HEAD")}')
lines.extend(['', 'Data assets:',
              'SDL_GameControllerDB revision: 5a12daa568d19344f9b6e9286ef5929833b25c7c (Linux mappings transformed to webOS)',
              'Material Icons licence revision: 27e9ef1dbeedc13d682fece4a58e1eda4cb0961a',
              'Material Icons licence sha256: 58d1e17ffe5109a7ae296caafcadfdbe6a7d176f0bc4ab01e12a689b0499d8bd',
              'Project-owned assets/translation sources: accompanying exact Eclipse source archive.',
              '', 'The source archive must be distributed beside this IPK. See the release source manifest.'])
(package / 'SOURCE-MANIFEST.txt').write_text('\n'.join(lines) + '\n')
build_info = ['Eclipse webOS build', f'Application ID: {info["id"]}', f'Version: {info["version"]}',
              f'Source revision: {revision}', f'Source status entries: {len(status.splitlines())}',
              f'Configuration: {cache.get("CMAKE_BUILD_TYPE", "unknown")}',
              'Signing: unsigned developer-mode package',
              'No system services or startup tasks are installed by this package.',
              'Closing/uninstalling and hardware compatibility require device testing of this exact IPK.']
(package / 'BUILDINFO.txt').write_text('\n'.join(build_info) + '\n')
files = [path for path in package.rglob('*') if path.is_file() and path.name != 'SHA256SUMS']
(package / 'SHA256SUMS').write_text(''.join(f'{digest(path)}  {path.relative_to(package)}\n' for path in sorted(files)))
print(f'webOS package metadata: {len(files)} files, {info["id"]}, {revision}')

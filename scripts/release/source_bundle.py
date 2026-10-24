#!/usr/bin/env python3
"""Archive committed project sources and verified, pre-applied dependency patches.

This is the project source component only. External libraries' source archives and
build toolchains are separate release inputs. No mutable module bytes are shipped:
module worktrees must match Git objects plus the committed bootstrap patch exactly.
"""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import shlex
import stat
import subprocess
import tarfile
import tempfile

ROOT_PATHS = ('CMakeLists.txt', 'LICENSE.txt', 'LICENSES.md', 'COPYRIGHT',
              'ACKNOWLEDGEMENTS.md', 'README.md', 'SUPPORT.md', '.gitmodules',
              'cmake', 'core', 'src', 'third_party', 'res', 'patches', 'deploy',
              'experimental/gpu-hevc', 'tests', 'scripts/bootstrap_client_deps.sh',
              'scripts/windows', 'scripts/linux', 'scripts/webos', 'scripts/android', 'scripts/release',
              'scripts/lib', 'scripts/tv/sync_ares_device.sh',
              'docs/licenses', 'packaging')


def git(repo, *args, **kwargs):
    return subprocess.check_output(['git', '-C', str(repo), *args], **kwargs)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def safe_name(name):
    path = PurePosixPath(name)
    if not name or path.is_absolute() or '..' in path.parts or '\n' in name or '\r' in name:
        raise RuntimeError(f'Unsafe archive path: {name!r}')
    return name


def source_path(name):
    """Omit known generated test tools/bytecode, never source or build inputs."""
    return '__pycache__' not in PurePosixPath(name).parts and not name.endswith('.pyc') and not name.startswith('tests/coop/pkg/')


def tree_entries(repo, revision, root=False):
    args = ['ls-tree', '-rz', revision]
    if root:
        args += ['--', *ROOT_PATHS]
    entries = {}
    for record in git(repo, *args).split(b'\0'):
        if record:
            metadata, name = record.split(b'\t', 1)
            mode, kind, oid = metadata.decode().split()
            entries[safe_name(name.decode())] = (mode, kind, oid)
    return entries


class Blobs:
    """Read immutable blobs without one subprocess per source file."""
    def __init__(self, repo):
        self.process = subprocess.Popen(['git', '-C', str(repo), 'cat-file', '--batch'],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def read(self, oid):
        self.process.stdin.write((oid + '\n').encode())
        self.process.stdin.flush()
        header = self.process.stdout.readline().decode().split()
        if len(header) != 3 or header[1] != 'blob':
            raise RuntimeError(f'Cannot read Git blob {oid}: {header}')
        data = self.process.stdout.read(int(header[2]))
        if len(data) != int(header[2]) or self.process.stdout.read(1) != b'\n':
            raise RuntimeError(f'Truncated Git blob {oid}')
        return data

    def close(self):
        self.process.stdin.close()
        self.process.stdout.close()
        if self.process.wait() != 0:
            raise RuntimeError('Git object reader failed')


def patch_plan(root, revision):
    """Use the bootstrap's committed module/base/patch triples as the one owner."""
    script = git(root, 'show', revision + ':scripts/bootstrap_client_deps.sh').decode()
    tokens = shlex.split(script.replace('\\\n', ''), comments=True)
    patches = {}
    for index, token in enumerate(tokens):
        if token != 'ensure_patched_submodule':
            continue
        try:
            module, base, path = tokens[index + 1:index + 4]
        except ValueError as error:
            raise RuntimeError('Unrecognized dependency bootstrap patch invocation') from error
        safe_name(module)
        safe_name(path)
        if not re.fullmatch('[0-9a-f]{40}', base) or module in patches:
            raise RuntimeError(f'Invalid or duplicate patch declaration: {module}')
        patches[module] = (base, path, git(root, 'show', revision + ':' + path))
    return patches


def expected_module(repo, base, patch, temporary):
    """Apply the committed patch in a disposable bare Git repo, never the worktree."""
    if patch is None:
        return repo, base, b''
    object_repo = Path(tempfile.mkdtemp(dir=temporary))
    subprocess.run(['git', 'init', '--bare', '-q', str(object_repo)], check=True)
    objects = Path(git(repo, 'rev-parse', '--git-common-dir').decode().strip())
    objects = (repo / objects).resolve() / 'objects'
    if '\n' in str(objects):
        raise RuntimeError('Object directory contains a line break')
    (object_repo / 'objects/info/alternates').write_text(str(objects) + '\n')
    env = dict(os.environ, GIT_INDEX_FILE=str(object_repo / 'source.index'))
    git(object_repo, 'read-tree', base, env=env)
    git(object_repo, 'apply', '--cached', '--binary', '--whitespace=nowarn', '-', input=patch, env=env)
    tree = git(object_repo, 'write-tree', env=env).decode().strip()
    delta = git(object_repo, 'diff', '--binary', '--no-ext-diff', base, tree)
    return object_repo, tree, delta


def verify_file(path, mode, data):
    try:
        info = path.lstat()
        if mode == '120000':
            actual = os.fsencode(os.readlink(path)) if stat.S_ISLNK(info.st_mode) else None
        else:
            actual = path.read_bytes() if stat.S_ISREG(info.st_mode) else None
            if actual is not None and bool(info.st_mode & 0o111) != (mode == '100755'):
                raise RuntimeError(f'Unexpected source executable mode: {path}')
    except FileNotFoundError as error:
        raise RuntimeError(f'Missing expected source file: {path}') from error
    if actual is None or sha(actual) != sha(data):
        raise RuntimeError(f'Source differs from pinned Git + committed patch: {path}')


def collect(repo, revision, prefix, result, modules, patches, temporary, root=False):
    actual_repo, actual_tree, expected_delta = repo, revision, b''
    declaration = None if root else patches.get(prefix.rstrip('/'))
    if declaration:
        base, patch_name, patch = declaration
        if base != revision:
            raise RuntimeError(f'{prefix}: patch base does not match the pinned gitlink')
        actual_repo, actual_tree, expected_delta = expected_module(repo, revision, patch, temporary)
    entries = tree_entries(actual_repo, actual_tree, root=root)
    if not root:
        # Expected additions are included; any other untracked source is refused.
        untracked = set(git(repo, 'ls-files', '--others', '--exclude-standard', '-z').decode().split('\0')) - {''}
        unknown = untracked - set(entries)
        indexed = set(git(repo, 'ls-files', '--cached', '-z').decode().split('\0')) - {''}
        unknown.update(name for name in indexed - set(entries) if os.path.lexists(repo / name))
        if unknown:
            raise RuntimeError(f'{prefix}: unexpected untracked or staged source inputs: {sorted(unknown)}')
        # Deleted files must actually be absent from the builder's worktree.
        for name, (_, kind, _) in tree_entries(repo, revision).items():
            if kind == 'blob' and name not in entries and os.path.lexists(repo / name):
                raise RuntimeError(f'{prefix}: patch-deleted source still exists: {name}')
        modules.append({'path': prefix.rstrip('/'), 'commit': revision,
                        'expected_source_tree': git(actual_repo, 'rev-parse', actual_tree + '^{tree}').decode().strip(),
                        'applied_patch': {'path': declaration[1], 'sha256': sha(declaration[2])} if declaration else None,
                        'expected_delta_sha256': sha(expected_delta),
                        'working_git_diff_sha256': sha(git(repo, 'diff', '--binary', '--no-ext-diff', 'HEAD')),
                        'verified_patch_additions': sorted(untracked)})
    reader = Blobs(actual_repo)
    try:
        for name, (mode, kind, oid) in entries.items():
            archive_name = prefix + name
            if kind == 'commit':
                module = repo / name
                if not (module / '.git').exists():
                    raise RuntimeError(f'Uninitialized source dependency: {archive_name}')
                actual = git(module, 'rev-parse', 'HEAD').decode().strip()
                if actual != oid:
                    raise RuntimeError(f'{archive_name}: HEAD {actual} does not match gitlink {oid}')
                collect(module, oid, archive_name + '/', result, modules, patches, temporary)
            elif kind == 'blob':
                data = reader.read(oid)
                if not root:
                    verify_file(repo / name, mode, data)
                if mode == '120000':
                    target = data.decode()
                    resolved = posixpath.normpath(posixpath.join(posixpath.dirname(archive_name), target))
                    if target.startswith('/') or resolved == '..' or resolved.startswith('../'):
                        raise RuntimeError(f'Source symlink escapes archive: {archive_name}')
                if source_path(archive_name):
                    result[archive_name] = (mode, data)
    finally:
        reader.close()


def create_bundle(root, output):
    root = root.resolve()
    if git(root, 'diff', 'HEAD', '--ignore-submodules=all', '--', *ROOT_PATHS).strip():
        raise RuntimeError('Commit release sources before archiving')
    if git(root, 'ls-files', '--others', '--exclude-standard', '--', *ROOT_PATHS).strip():
        raise RuntimeError('Untracked release source files must be committed first')
    revision = git(root, 'rev-parse', 'HEAD').decode().strip()
    timestamp = int(git(root, 'show', '-s', '--format=%ct', 'HEAD'))
    files, modules = {}, []
    patches = patch_plan(root, revision)
    with tempfile.TemporaryDirectory(prefix='eclipse-source-objects-') as temporary:
        collect(root, revision, '', files, modules, patches, temporary, root=True)
    missing = set(patches) - {module['path'] for module in modules}
    if missing:
        raise RuntimeError(f'Bootstrap patches refer to missing submodules: {sorted(missing)}')
    notes = files.get('scripts/release/BUILD-FROM-SOURCE.md')
    if notes is None:
        raise RuntimeError('Missing source-archive build instructions')
    files['BUILD-FROM-SOURCE.md'] = notes
    manifest = {'schema': 2, 'project': 'Eclipse', 'commit': revision, 'submodules': modules,
                'scope': 'Project sources only; external dependency source packages remain separate release inputs.',
                'files': {name: {'sha256': sha(data), 'git_mode': mode} for name, (mode, data) in sorted(files.items())}}
    files['SOURCE-BUNDLE.json'] = ('100644', (json.dumps(manifest, indent=2) + '\n').encode())
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + '.tmp')
    try:
        with temporary.open('wb') as stream, gzip.GzipFile(fileobj=stream, mode='wb', mtime=0, filename='') as gz:
            with tarfile.open(fileobj=gz, mode='w') as archive:
                for name, (mode, data) in sorted(files.items()):
                    info = tarfile.TarInfo('eclipse-source-' + revision[:12] + '/' + name)
                    info.uid = info.gid = 0
                    info.mtime = timestamp
                    info.mode = 0o755 if mode == '100755' else 0o644
                    if mode == '120000':
                        info.type = tarfile.SYMTYPE
                        info.linkname = data.decode()
                        archive.addfile(info)
                    else:
                        info.size = len(data)
                        archive.addfile(info, io.BytesIO(data))
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    digest = sha(output.read_bytes())
    output.with_suffix(output.suffix + '.sha256').write_text(f'{digest}  {output.name}\n')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    manifest = create_bundle(args.root, args.output)
    print(f'{args.output}: {len(manifest["files"])} source files, {len(manifest["submodules"])} verified modules')


if __name__ == '__main__':
    main()

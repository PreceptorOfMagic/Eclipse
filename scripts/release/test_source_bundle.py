#!/usr/bin/env python3
"""Temporary Git fixtures only: never archive or modify the real project."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('source_bundle', Path(__file__).with_name('source_bundle.py'))
bundle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bundle)


def git(repo, *args):
    return subprocess.check_output(['git', '-C', str(repo), *args], stderr=subprocess.PIPE)


def init(repo):
    repo.mkdir()
    git(repo, 'init', '-q')
    git(repo, 'config', 'user.name', 'Source fixture')
    git(repo, 'config', 'user.email', 'fixture@example.invalid')


def commit(repo):
    git(repo, 'add', '.')
    git(repo, 'commit', '-qm', 'Fixture source')
    return git(repo, 'rev-parse', 'HEAD').decode().strip()


class SourceBundleTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='source-bundle-fixture-')
        self.addCleanup(self.temp.cleanup)
        self.parent = Path(self.temp.name)
        origin = self.parent / 'origin'
        init(origin)
        (origin / 'value.h').write_text('#define VALUE 1\n')
        (origin / 'unchanged.txt').write_text('pinned\n')
        (origin / 'removed.txt').write_text('the patch removes this file\n')
        self.base = commit(origin)
        (origin / 'value.h').write_text('#include "added.h"\n')
        (origin / 'removed.txt').unlink()
        (origin / 'added.h').write_text('#define VALUE 42\n')
        (origin / 'added-link.h').symlink_to('added.h')
        (origin / 'executable.sh').write_text('#!/bin/sh\nexit 0\n')
        (origin / 'executable.sh').chmod(0o755)
        git(origin, 'add', '.')
        self.patch = git(origin, 'diff', '--cached', '--binary', 'HEAD')
        self.root = self.parent / 'project'
        init(self.root)
        git(self.root, '-c', 'protocol.file.allow=always', 'submodule', 'add', '-q', str(origin), 'third_party/demo')
        self.module = self.root / 'third_party/demo'
        for folder in ('scripts/release', 'res', 'patches/client-deps', 'src', 'tests/coop/pkg', 'tests/__pycache__'):
            (self.root / folder).mkdir(parents=True, exist_ok=True)
        (self.root / 'scripts/bootstrap_client_deps.sh').write_text(
            'ensure_patched_submodule \\\n  third_party/demo \\\n  ' + self.base + ' \\\n  patches/client-deps/demo.patch\n')
        (self.root / 'patches/client-deps/demo.patch').write_bytes(self.patch)
        (self.root / 'scripts/release/BUILD-FROM-SOURCE.md').write_text('Fixture Git-free source instructions\n')
        (self.root / 'CMakeLists.txt').write_text('project(fixture C)\n')
        (self.root / 'res/eclipse.ico').write_bytes(b'icon-fixture')
        (self.root / 'src/main.c').write_text('#include "../third_party/demo/value.h"\n')
        (self.root / 'tests/coop/pkg/probe').write_bytes(b'not-source')
        (self.root / 'tests/__pycache__/fixture.pyc').write_bytes(b'bytecode')
        self.revision = commit(self.root)
        git(self.module, 'apply', str(self.root / 'patches/client-deps/demo.patch'))
        self.output = self.parent / 'snapshot.tar.gz'

    def create(self):
        return bundle.create_bundle(self.root, self.output)

    def test_patched_new_files_modes_symlinks_and_reproducibility(self):
        manifest = self.create()
        original = self.output.read_bytes()
        self.create()
        self.assertEqual(original, self.output.read_bytes())
        prefix = 'eclipse-source-' + self.revision[:12] + '/'
        with tarfile.open(self.output) as archive:
            names = archive.getnames()
            self.assertEqual(archive.extractfile(prefix + 'third_party/demo/added.h').read(), b'#define VALUE 42\n')
            self.assertEqual(archive.extractfile(prefix + 'third_party/demo/value.h').read(), b'#include "added.h"\n')
            self.assertEqual(archive.getmember(prefix + 'third_party/demo/executable.sh').mode, 0o755)
            link = archive.getmember(prefix + 'third_party/demo/added-link.h')
            self.assertTrue(link.issym())
            self.assertEqual(link.linkname, 'added.h')
            self.assertIn(prefix + 'res/eclipse.ico', names)
            self.assertIn(prefix + 'BUILD-FROM-SOURCE.md', names)
            self.assertNotIn(prefix + 'third_party/demo/removed.txt', names)
            self.assertFalse(any('__pycache__' in name or '/tests/coop/pkg/' in name or '/.git/' in name for name in names))
            recorded = json.load(archive.extractfile(prefix + 'SOURCE-BUNDLE.json'))
            self.assertEqual(recorded, manifest)
        module = manifest['submodules'][0]
        self.assertEqual(module['commit'], self.base)
        self.assertEqual(module['applied_patch']['sha256'], bundle.sha(self.patch))
        self.assertIn('added.h', module['verified_patch_additions'])
        self.assertEqual(manifest['files']['third_party/demo/added.h']['sha256'], bundle.sha(b'#define VALUE 42\n'))
        # The index and module history were never changed to construct the archive.
        self.assertFalse(git(self.module, 'diff', '--cached').strip())
        self.assertEqual(git(self.module, 'rev-parse', 'HEAD').decode().strip(), self.base)

    def test_webos_operation_helpers_are_included(self):
        helpers = {
            'scripts/lib/net_env.sh': '. "$(dirname "${BASH_SOURCE[0]}")/tv_profile.sh"\n',
            'scripts/lib/tv_profile.sh': '# profile resolver fixture\n',
            'scripts/tv/sync_ares_device.sh': '. "../lib/net_env.sh"\n',
        }
        for name, content in helpers.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
            path.chmod(0o755)
        revision = commit(self.root)
        manifest = self.create()
        prefix = 'eclipse-source-' + revision[:12] + '/'
        with tarfile.open(self.output) as archive:
            for name, content in helpers.items():
                self.assertEqual(archive.extractfile(prefix + name).read(), content.encode())
                self.assertEqual(archive.getmember(prefix + name).mode, 0o755)
                self.assertEqual(manifest['files'][name]['sha256'], bundle.sha(content.encode()))

    def test_unknown_module_input_is_refused(self):
        (self.module / 'unexpected.c').write_text('not in the committed patch\n')
        with self.assertRaisesRegex(RuntimeError, 'unexpected untracked'):
            self.create()
        self.assertFalse(self.output.exists())

    def test_modified_patch_addition_is_refused(self):
        (self.module / 'added.h').write_text('#define VALUE 999\n')
        with self.assertRaisesRegex(RuntimeError, 'differs from pinned Git'):
            self.create()

    def test_unexpected_staged_source_is_refused(self):
        (self.module / 'unexpected.c').write_text('staging is not provenance\n')
        git(self.module, 'add', 'unexpected.c')
        with self.assertRaisesRegex(RuntimeError, 'unexpected untracked or staged'):
            self.create()

    def test_changed_executable_mode_is_refused(self):
        (self.module / 'executable.sh').chmod(0o644)
        with self.assertRaisesRegex(RuntimeError, 'executable mode'):
            self.create()

    def test_unexpected_tracked_change_is_refused(self):
        (self.module / 'unchanged.txt').write_text('unreviewed mutation\n')
        with self.assertRaisesRegex(RuntimeError, 'differs from pinned Git'):
            self.create()

    def test_missing_expected_file_is_refused(self):
        (self.module / 'added.h').unlink()
        with self.assertRaisesRegex(RuntimeError, 'Missing expected source'):
            self.create()

    def test_changed_root_source_is_refused(self):
        (self.root / 'src/main.c').write_text('not committed\n')
        with self.assertRaisesRegex(RuntimeError, 'Commit release sources'):
            self.create()

    def test_wrong_module_revision_is_refused(self):
        git(self.module, 'config', 'user.name', 'Fixture')
        git(self.module, 'config', 'user.email', 'fixture@example.invalid')
        commit(self.module)
        with self.assertRaisesRegex(RuntimeError, 'does not match gitlink'):
            self.create()


if __name__ == '__main__':
    unittest.main()

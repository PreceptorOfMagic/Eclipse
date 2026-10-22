#!/usr/bin/env python3
"""Exact raw-header and semantic regression fixture; no device interaction."""
import gzip
import importlib.util
import io
from pathlib import Path
import tarfile

spec = importlib.util.spec_from_file_location('normalizer', Path(__file__).with_name('normalize_ipk_dirs.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
APP = 'com.eclipse.fixture'
ROOT = 'usr/palm/applications/' + APP
BUILT = 1792368000


def make_ipk(extra_profile=False):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode='w', format=tarfile.USTAR_FORMAT) as archive:
        for path in ['usr', 'usr/palm/applications/another.app', ROOT, ROOT + '/userdata', ROOT + '/bin']:
            entry = tarfile.TarInfo(path)
            entry.type = tarfile.DIRTYPE
            entry.mode = 0o711 if path == 'usr' else 0o775 if path.endswith('another.app') else 0o777
            # ares-package under Node 22 writes mtime 0 (1970-01-01); 'usr' keeps a real one.
            entry.uid, entry.gid, entry.mtime = 1000, 1000, 123456 if path == 'usr' else 0
            archive.addfile(entry)
        for path, data, mode in [(ROOT + '/bin/app', b'\x00EXECUTABLE\xff\n', 0o755),
                                 (ROOT + '/NOTICE', b'Exact notice bytes\n', 0o644)]:
            entry = tarfile.TarInfo(path)
            entry.size, entry.mode, entry.mtime = len(data), mode, 123457 if path.endswith('NOTICE') else 0
            entry.uid, entry.gid = 1000, 1000
            archive.addfile(entry, io.BytesIO(data))
        if extra_profile:
            entry = tarfile.TarInfo(ROOT + '/userdata/conf')
            entry.type = tarfile.DIRTYPE
            archive.addfile(entry)
    data = gzip.compress(stream.getvalue(), mtime=123458)
    def member(name, body):
        header = f'{name + "/":<16}{123459:<12}{1000:<6}{1000:<6}{100644:<8}{len(body):<10}`\n'.encode('ascii')
        assert len(header) == 60
        return header + body + (b'\n' if len(body) % 2 else b'')
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode='w', format=tarfile.USTAR_FORMAT) as archive:
        body = b'Package: ' + APP.encode('ascii') + b'\n'
        entry = tarfile.TarInfo('control')
        entry.size, entry.mode, entry.mtime = len(body), 0o666, 0
        archive.addfile(entry, io.BytesIO(body))
    control = gzip.compress(stream.getvalue(), mtime=0)
    fixed = member('debian-binary', b'2.0\n')
    return b'!<arch>\n' + fixed + member('control.tar.gz', control) + member('data.tar.gz', data), fixed


original, fixed = make_ipk()
result, changes = module.normalize(original, APP, BUILT)
assert result[8:8 + len(fixed)] == fixed
assert len(changes) == 5
assert {'member': 'control.tar.gz', 'mtime_fixed': 1} in changes
assert {'member': 'data.tar.gz', 'mtime_fixed': 5} in changes
assert module.normalize(result, APP, BUILT + 1) == (result, [])

def members(blob, wanted=b'data.tar.gz'):
    pos = 8
    while pos < len(blob):
        header = blob[pos:pos + 60]
        size = int(header[48:58])
        body = blob[pos + 60:pos + 60 + size]
        if header[:16].strip().rstrip(b'/') == wanted:
            with tarfile.open(fileobj=io.BytesIO(gzip.decompress(body)), mode='r:') as archive:
                return {m.name: (m, archive.extractfile(m).read() if m.isfile() else None) for m in archive}
        pos += 60 + size + size % 2

for wanted in (b'control.tar.gz', b'data.tar.gz'):
    before, after = members(original, wanted), members(result, wanted)
    assert before.keys() == after.keys()
    for path in before:
        old, old_bytes = before[path]
        new, new_bytes = after[path]
        assert old_bytes == new_bytes
        assert (old.uid, old.gid, old.size, old.type, old.linkname) == (new.uid, new.gid, new.size, new.type, new.linkname)
        assert new.mtime == (BUILT if old.mtime == 0 else old.mtime)
        expected_mode = old.mode
        if old.isdir() and (path == ROOT or path.startswith(ROOT + '/')):
            expected_mode = 0o1777 if path == ROOT + '/userdata' else 0o755
        assert new.mode == expected_mode
for invalid in [make_ipk(extra_profile=True)[0], original.replace(b'data.tar.gz/', b'wrongtar.gz/')]:
    try:
        module.normalize(invalid, APP, BUILT)
    except ValueError:
        pass
    else:
        raise AssertionError('Unsafe/missing userdata package accepted')
print('PASS only directory mode, zero mtime and checksum fields changed; all files/control/other metadata exact')
print('PASS mtime 0 -> build time in control.tar.gz and data.tar.gz; real mtimes kept')
print('PASS app directories0755, userdata1777, idempotence, missing/prefilled rejection')

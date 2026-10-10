#!/usr/bin/env python3
"""Normalize only tar directory mode/checksum fields; leave payload bytes intact."""
import gzip
import io
import json
import os
from pathlib import Path
import re
import stat
import sys
import tarfile
import tempfile


def normalize(blob, app_id):
    if not re.fullmatch(r'[a-z0-9]+(?:[.+-][a-z0-9]+)*', app_id):
        raise ValueError('Unsafe application ID')
    if not blob.startswith(b'!<arch>\n'):
        raise ValueError('Expected ar IPK')
    chunks = []
    offset = 8
    found = 0
    changes = []
    while offset < len(blob):
        header = blob[offset:offset + 60]
        if len(header) != 60 or header[58:] != b'`\n':
            raise ValueError('Invalid ar member header')
        size = int(header[48:58].decode('ascii').strip())
        end = offset + 60 + size
        if size < 0 or end + size % 2 > len(blob):
            raise ValueError('Invalid ar member size')
        name = header[:16].decode('ascii').strip().rstrip('/')
        contents = blob[offset + 60:end]
        padding = blob[end:end + size % 2]
        if name == 'data.tar.gz':
            found += 1
            if contents[:4] != b'\x1f\x8b\x08\x00':
                raise ValueError('Unsupported gzip header')
            before = gzip.decompress(contents)
            after = bytearray(before)
            allowed = set()
            app_root = 'usr/palm/applications/' + app_id
            userdata = app_root + '/userdata'
            seen_userdata = 0
            seen_app_root = 0
            with tarfile.open(fileobj=io.BytesIO(before), mode='r:') as archive:
                for item in archive:
                    path = item.name.removeprefix('./').rstrip('/')
                    if path == app_root:
                        seen_app_root += 1
                        if not item.isdir():
                            raise ValueError('Application root must be a directory')
                    if path == userdata:
                        seen_userdata += 1
                        if not item.isdir():
                            raise ValueError('userdata must be an empty directory')
                    if path.startswith(userdata + '/') or path == app_root + '/conf' or path.startswith(app_root + '/conf/'):
                        raise ValueError('Package must not contain a user profile')
                    if not item.isdir() or not (path == app_root or path.startswith(app_root + '/')):
                        continue
                    start = item.offset
                    block = after[start:start + 512]
                    raw_name = bytes(block[:100]).split(b'\0', 1)[0]
                    prefix = bytes(block[345:500]).split(b'\0', 1)[0]
                    if prefix:
                        raw_name = prefix + b'/' + raw_name
                    if block[156:157] != b'5' or raw_name.decode('utf-8').removeprefix('./').rstrip('/') != path:
                        raise ValueError('Unsupported extended directory header')
                    mode = 0o1777 if path == userdata else 0o755
                    if item.mode == mode:
                        continue
                    block[100:108] = f'{mode:07o}\0'.encode('ascii')
                    block[148:156] = b'        '
                    block[148:156] = f'{sum(block):06o}\0 '.encode('ascii')
                    after[start:start + 512] = block
                    allowed.update(range(start + 100, start + 108))
                    allowed.update(range(start + 148, start + 156))
                    changes.append({'path': path, 'old': oct(item.mode), 'new': oct(mode)})
            if seen_app_root != 1:
                raise ValueError('Expected exactly one application root directory')
            if seen_userdata != 1:
                raise ValueError('Expected exactly one empty userdata directory')
            if any(a != b and i not in allowed for i, (a, b) in enumerate(zip(before, after))):
                raise ValueError('Unexpected tar mutation')
            if len(before) != len(after):
                raise ValueError('Unexpected tar size change')
            packed = gzip.compress(bytes(after), compresslevel=6, mtime=0)
            # The SDK gzip header has no optional fields. Preserve its metadata.
            contents = contents[:10] + packed[10:]
            header = header[:48] + f'{len(contents):<10}'.encode('ascii') + header[58:]
            padding = b'\n' if len(contents) % 2 else b''
        chunks.append(header + contents + padding)
        offset = end + size % 2
    if found != 1:
        raise ValueError('Expected exactly one data.tar.gz member')
    return b'!<arch>\n' + b''.join(chunks), changes


def main():
    if len(sys.argv) != 3:
        raise SystemExit('Usage: normalize_ipk_dirs.py PACKAGE.ipk APP_ID')
    path = Path(sys.argv[1])
    if path.is_symlink() or not path.is_file():
        raise SystemExit('Expected regular package file')
    payload, changes = normalize(path.read_bytes(), sys.argv[2])
    # A second pass verifies all directory headers and produces no more changes.
    verified, further = normalize(payload, sys.argv[2])
    if verified != payload or further:
        raise SystemExit('Directory normalization is not idempotent')
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + '.', delete=False) as output:
        temporary = Path(output.name)
        output.write(payload)
        output.flush()
        os.fsync(output.fileno())
    try:
        temporary.chmod(stat.S_IMODE(path.stat().st_mode))
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)
    print(json.dumps({'package': str(path), 'directory_changes': changes}, indent=2))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Test-only native Pulse authentication capture; never opens an audio device."""
import os, ctypes, socket, time, json, tempfile
from pathlib import Path
out = Path(os.environ['PROBE_OUT'])
socketdir = tempfile.TemporaryDirectory(prefix='epa-', dir='/tmp')
path = socketdir.name + '/native'
server = socket.socket(socket.AF_UNIX)
server.bind(path)
server.listen(1)
server.setblocking(False)
os.environ['PULSE_SERVER'] = 'unix:' + path
p = ctypes.CDLL('libpulse.so.0')
for n, args, res in [('pa_mainloop_new', [], ctypes.c_void_p), ('pa_mainloop_get_api', [ctypes.c_void_p], ctypes.c_void_p), ('pa_context_new', [ctypes.c_void_p, ctypes.c_char_p], ctypes.c_void_p), ('pa_context_connect', [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p], ctypes.c_int), ('pa_mainloop_iterate', [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p], ctypes.c_int), ('pa_context_disconnect', [ctypes.c_void_p], None), ('pa_context_unref', [ctypes.c_void_p], None), ('pa_mainloop_free', [ctypes.c_void_p], None)]:
    f = getattr(p, n)
    f.argtypes = args
    f.restype = res
p.pa_context_get_state.argtypes = [ctypes.c_void_p]
p.pa_context_get_state.restype = ctypes.c_int
p.pa_context_errno.argtypes = [ctypes.c_void_p]
p.pa_context_errno.restype = ctypes.c_int
p.pa_strerror.argtypes = [ctypes.c_int]
p.pa_strerror.restype = ctypes.c_char_p
loop = p.pa_mainloop_new()
ctx = p.pa_context_new(p.pa_mainloop_get_api(loop), b'Eclipse auth fixture')
conn = None
data = b''
try:
    assert p.pa_context_connect(ctx, None, 1, None) == 0
    for _ in range(3000):
        p.pa_mainloop_iterate(loop, 0, None)
        if conn is None:
            try:
                conn, _ = server.accept()
                conn.setblocking(False)
            except BlockingIOError:
                pass
        if conn:
            try:
                data += conn.recv(65536)
            except BlockingIOError:
                pass
        if len(data) >= 289:
            break
        time.sleep(0.001)
    assert len(data) >= 289, (len(data), conn is not None, p.pa_context_get_state(ctx), p.pa_strerror(p.pa_context_errno(ctx)))
    (out / 'auth-packet').write_bytes(data)
    metadata = {k: os.environ.get(k) for k in ['PULSE_CLIENTCONFIG', 'PULSE_COOKIE']}
    metadata['held_config_parent'] = os.readlink('/proc/self/fd/8') if Path('/proc/self/fd/8').exists() else None
    metadata['held_cookie'] = os.readlink('/proc/self/fd/9') if Path('/proc/self/fd/9').exists() else None
    (out / 'environment.json').write_text(json.dumps(metadata))
finally:
    p.pa_context_disconnect(ctx)
    p.pa_context_unref(ctx)
    p.pa_mainloop_free(loop)
    if conn:
        conn.close()
    server.close()
    Path(path).unlink()
    socketdir.cleanup()

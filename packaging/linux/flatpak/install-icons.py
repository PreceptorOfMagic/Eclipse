# The Windows icon's PNG images, unchanged (as scripts/linux/build_portable.sh), named for the app id.
import os, struct, sys
data = open(sys.argv[1], 'rb').read()
count = struct.unpack('<HHH', data[:6])[2]
sizes = set()
for i in range(count):
    w, h, _, _, _, _, size, off = struct.unpack('<BBBBHHII', data[6 + 16 * i:22 + 16 * i])
    w, h = w or 256, h or 256
    blob = data[off:off + size]
    if blob[:8] != b'\x89PNG\r\n\x1a\n':
        sys.exit(f'eclipse.ico entry {w}x{h} is not a PNG image')
    out = os.path.join(sys.argv[2], f'{w}x{h}', 'apps')
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, 'io.github.PreceptorOfMagic.Eclipse.png'), 'wb').write(blob)
    sizes.add(w)
if sizes != {16, 24, 32, 48, 64, 128, 256}:
    sys.exit(f'unexpected icon sizes {sorted(sizes)}')

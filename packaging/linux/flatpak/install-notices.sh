#!/bin/sh
# install-notices.sh <licence dir> -- the third-party licences and THIRD-PARTY-NOTICES.txt of the Flatpak, as
# scripts/linux/build_portable.sh assembles them for the tarball. Run from the Eclipse module's source root, where
# the manifest has unpacked each bundled library's pinned archive again under notices/<module> (the archives the
# modules above were built from; flatpak-builder reuses its downloads) and the two pinned licence files in notices/.
# The versions below are the manifest's pins; the Freedesktop runtime's own libraries carry their own licences.
set -eu

DEST="$1"
SRC="$DEST/licenses/source"
EXT="$DEST/licenses/external"
DEP="$DEST/licenses/dependencies"
mkdir -p "$SRC" "$EXT" "$DEP"

install -m644 docs/licenses/common/* "$EXT/"
install -m644 notices/SDL_GameControllerDB-LICENSE.txt "$EXT/SDL_GameControllerDB-LICENSE.txt"
install -m644 notices/Material-Icons-LICENSE.txt "$EXT/Material-Icons-Apache-2.0.txt"
install -m644 docs/licenses/webos/Moonlight-Common-C-Reed-Solomon-BSD.txt \
    docs/licenses/webos/libmicrodns-compat-ISC-style.txt "$SRC/"
install -m644 third_party/libmicrodns/AUTHORS "$SRC/libmicrodns-AUTHORS.txt"

# name | source | path in the checkout | file name in licenses/source (build_portable.sh SOURCE_LICENSES)
SOURCE_LICENSES="moonlight-common-c|https://github.com/moonlight-stream/moonlight-common-c|core/moonlight-common-c/LICENSE.txt|moonlight-common-c-GPL-3.0.txt
ENet|https://github.com/lsalzman/enet|core/moonlight-common-c/enet/LICENSE|ENet-MIT.txt
SS4S|https://github.com/mariotaku/ss4s|third_party/ss4s/LICENSE|SS4S-LGPL-3.0.txt
commons-c|https://github.com/mariotaku/commons-c|third_party/commons/LICENSE|commons-c-MIT.txt
LVGL|https://github.com/lvgl/lvgl|third_party/lvgl/LICENCE.txt|LVGL-MIT.txt
h264bitstream|https://github.com/aizvorski/h264bitstream|third_party/h264bitstream/LICENSE|h264bitstream-LGPL-2.1.txt
libmicrodns|https://github.com/videolabs/libmicrodns|third_party/libmicrodns/COPYING|libmicrodns-LGPL-2.1.txt"
echo "$SOURCE_LICENSES" | while IFS='|' read -r name url path file; do
    install -m644 "$path" "$SRC/$file"
done

install -m644 notices/sdl2/LICENSE.txt "$DEP/SDL2-Zlib.txt"
install -m644 notices/sdl2/src/hidapi/LICENSE.txt "$DEP/SDL2-HIDAPI-licence-options.txt"
install -m644 notices/sdl2/src/hidapi/LICENSE-bsd.txt "$DEP/SDL2-HIDAPI-BSD.txt"
install -m644 notices/sdl2/src/video/yuv2rgb/LICENSE "$DEP/SDL2-yuv2rgb-BSD.txt"
install -m644 notices/sdl2-image/LICENSE.txt "$DEP/SDL2_image-Zlib.txt"
install -m644 notices/ffmpeg/COPYING.LGPLv2.1 "$DEP/FFmpeg-LGPL-2.1.txt"
install -m644 notices/ffmpeg/LICENSE.md "$DEP/FFmpeg-LICENSE.md"
install -m644 notices/mbedtls/LICENSE "$DEP/mbedTLS-Apache-2.0.txt"
install -m644 notices/curl/COPYING "$DEP/curl-COPYING.txt"
install -m644 notices/opus/COPYING "$DEP/Opus-BSD-3-Clause.txt"
install -m644 notices/inih/LICENSE.txt "$DEP/inih-BSD-3-Clause.txt"
# The MIT licence of nv-codec-headers is the leading comment of this header.
python3 - notices/nv-codec-headers/include/ffnvcodec/dynlink_loader.h "$DEP/nv-codec-headers-MIT.txt" << 'EOF'
import re, sys
text = open(sys.argv[1]).read()
m = re.match(r'\s*/\*.*?\*/', text, re.S)
if not m or 'Permission is hereby granted' not in m.group(0): sys.exit('nv-codec-headers licence comment not found')
open(sys.argv[2], 'w').write(m.group(0) + '\n')
EOF

{
    echo 'Eclipse Flatpak - third-party notices'
    echo
    echo 'Full licence texts are under licenses/ in this folder. Eclipse itself is distributed under GPL-3.0; see LICENSE.txt.'
    echo 'The Freedesktop runtime (org.freedesktop.Platform 25.08) provides the remaining libraries under their own licences.'
    echo
    echo 'SOURCE COMPONENTS COMPILED INTO THE CLIENT'
    echo "$SOURCE_LICENSES" | while IFS='|' read -r name url path file; do
        echo "- $name"; echo "  Source: $url"; echo "  Licence: licenses/source/$file"
    done
    echo
    echo 'LIBRARIES BUILT FROM PINNED SOURCE (exact archive URL and SHA-256 in the Flatpak manifest, io.github.PreceptorOfMagic.Eclipse.yml)'
    echo '- SDL2-2.32.10 (Zlib): lib/libSDL2-2.0.so.0 -- licenses/dependencies/SDL2-Zlib.txt'
    echo '- SDL2_image-2.8.12 (Zlib): lib/libSDL2_image-2.0.so.0 -- licenses/dependencies/SDL2_image-Zlib.txt'
    echo '- ffmpeg-9.0.1 (LGPL-2.1-or-later; built without GPL or non-free parts): lib/libavcodec, libavutil,'
    echo '  libswscale -- licenses/dependencies/FFmpeg-LGPL-2.1.txt, FFmpeg-LICENSE.md'
    echo '- mbedtls-3.6.7 (Apache-2.0): lib/libmbedtls, libmbedx509, libmbedcrypto -- licenses/dependencies/mbedTLS-Apache-2.0.txt'
    echo '- curl-8.22.0 (curl licence; with scripts/linux/portable/patches/curl-8.22.0-mbedtls-pem-key-blob.patch): lib/libcurl.so.4 --'
    echo '  licenses/dependencies/curl-COPYING.txt'
    echo '- opus-1.6.1 (BSD-3-Clause), linked into bin/moonlight-tv -- licenses/dependencies/Opus-BSD-3-Clause.txt'
    echo '- inih r58 (BSD-3-Clause), linked into bin/moonlight-tv -- licenses/dependencies/inih-BSD-3-Clause.txt'
    echo '- nv-codec-headers n13.0.19.1 (MIT), compiled into FFmpeg -- licenses/dependencies/nv-codec-headers-MIT.txt'
    echo
    echo 'EXTERNAL DATA AND EMBEDDED ASSETS'
    echo '- LVGL embedded fonts (Montserrat, Font Awesome, Unscii): licenses/external/LVGL-Embedded-Fonts-NOTICE.md'
    echo '- Moonlight Common C Reed-Solomon: licenses/source/Moonlight-Common-C-Reed-Solomon-BSD.txt'
    echo '- libmicrodns compatibility code and authors: licenses/source/libmicrodns-compat-ISC-style.txt, libmicrodns-AUTHORS.txt'
    echo '- SDL2 embedded HIDAPI (BSD option) and yuv2rgb: licenses/dependencies/SDL2-HIDAPI-BSD.txt, SDL2-yuv2rgb-BSD.txt'
    echo '- SDL_GameControllerDB at revision 5a12daa568d19344f9b6e9286ef5929833b25c7c: bin/gamecontrollerdb.txt'
    echo '  Source: https://github.com/gabomdq/SDL_GameControllerDB -- licenses/external/SDL_GameControllerDB-LICENSE.txt'
    echo '- Google Material Icons (embedded font; licence pinned at revision 27e9ef1dbeedc13d682fece4a58e1eda4cb0961a)'
    echo '  Source: https://github.com/google/material-design-icons -- licenses/external/Material-Icons-Apache-2.0.txt'
    echo '- Mozilla CA certificate list as of 2026-09-25: bin/curl-ca-bundle.crt'
    echo '  Source: https://curl.se/ca/cacert-2026-09-25.pem -- Mozilla Public License 2.0, licenses/external/MPL-2.0.txt'
} > "$DEST/THIRD-PARTY-NOTICES.txt"

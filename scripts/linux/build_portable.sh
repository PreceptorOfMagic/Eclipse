#!/usr/bin/env bash
# build_portable.sh -- build the Linux x86_64 client and assemble the portable package:
#   dist/eclipse-linux-x86_64/  and  dist/eclipse-linux-x86_64.tar.gz (+ .sha256)
#
# The Linux twin of scripts/windows/build_portable.ps1: the same pinned controller database, licences,
# third-party notices, source manifest, BUILDINFO contract and SHA256SUMS. The PE checks become ELF ones:
# RUNPATH ($ORIGIN), the NEEDED closure, the glibc floor, builder paths, and a load under the jammy loader.
# Prerequisites (once per builder): portable/make_sysroot.sh, then portable/build_deps.sh.
#   --clean               start from an empty build directory
#   --no-archive          assemble the folder only
#   --require-clean-tree  refuse a modified checkout (release builds)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PORTABLE="$ROOT/scripts/linux/portable"
. "$PORTABLE/env.sh"

CLEAN=0; MAKE_ARCHIVE=1; REQUIRE_CLEAN=0
for a in "$@"; do
    case "$a" in
        --clean) CLEAN=1 ;; --no-archive) MAKE_ARCHIVE=0 ;; --require-clean-tree) REQUIRE_CLEAN=1 ;;
        *) echo "usage: $0 [--clean] [--no-archive] [--require-clean-tree]" >&2; exit 2 ;;
    esac
done

CONFIG=Release
BUILD="$ROOT/build/linux-x86_64"
PKG_NAME=eclipse-linux-x86_64
PKG="$ROOT/dist/$PKG_NAME"
ARCHIVE="$ROOT/dist/$PKG_NAME.tar.gz"
DL="$ROOT/build/downloads-linux"      # outside $BUILD: --clean keeps verified downloads
GLIBC_FLOOR=2.35                      # Ubuntu 22.04 "jammy", the sysroot every binary is linked against

# Same pins as the Windows package (build_portable.ps1).
CONTROLLER_DB_REV=5a12daa568d19344f9b6e9286ef5929833b25c7c
CONTROLLER_DB_SHA=07ec5b753e685c4829987919b27905cc3c45c8e18c11b3046e275fcf38b0f3cb
CONTROLLER_DB_LICENSE_SHA=97f35b302b361680ec1e891e95d2d52097bb95abff361434916d99dc1305f127
MATERIAL_REV=27e9ef1dbeedc13d682fece4a58e1eda4cb0961a
MATERIAL_LICENSE_SHA=58d1e17ffe5109a7ae296caafcadfdbe6a7d176f0bc4ab01e12a689b0499d8bd
# Windows ships MSYS2's CA bundle; Linux ships Mozilla's roots as published by curl.se, pinned by date.
CA_DATE=2026-09-25
CA_SHA=a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505

# Libraries the package relies on the system for. Never bundled: glibc must be the system's, and the rest
# must match the system's own fonts, configuration and drivers. Anything else a binary NEEDs must be packaged.
GLIBC_LIBS="ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1"
DESKTOP_LIBS="libfontconfig.so.1 libfreetype.so.6 libexpat.so.1 libuuid.so.1"   # checked by the launcher
OPTIONAL_LIBS="libasound.so.2 libpulse.so.0 libpulse-simple.so.0"                # audio modules only
# NEEDED by FFmpeg but absent from some systems: bundled from the sysroot, used by the launcher ONLY when the
# system lacks that one library (the system's copy matches its VA drivers and Mesa).
FALLBACK_LIBS="libva:libva.so.2 libva-drm:libva-drm.so.2 libdrm:libdrm.so.2"

die() { echo "build_portable: $*" >&2; exit 1; }
step() { echo "== $*"; }
needed() { readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }
is_elf() { [ "$(head -c 4 "$1" 2>/dev/null | od -An -c | tr -d ' ')" = '177ELF' ]; }
built() {   # $1 = file name -> its path in the build tree (not CMake's scratch dirs)
    local f; f="$(find "$BUILD" -name "$1" -not -path '*/CMakeFiles/*' -print -quit)"
    [ -n "$f" ] || die "the build did not produce $1"
    echo "$f"
}
verified_download() {   # url sha256 destination
    if [ -f "$3" ] && echo "$2  $3" | sha256sum -c --quiet - 2> /dev/null; then return 0; fi
    local tmp="$3.download.$$"
    curl -fsSL -o "$tmp" "$1" || { rm -f "$tmp"; die "download failed: $1"; }
    echo "$2  $tmp" | sha256sum -c --quiet - || { rm -f "$tmp"; die "hash mismatch for $1"; }
    mv -f "$tmp" "$3"
}

for tool in cmake make gcc patchelf readelf strings python3 git curl tar gzip sha256sum; do
    command -v "$tool" > /dev/null || die "$tool is required"
done
[ -x "$SYSROOT/lib64/ld-linux-x86-64.so.2" ] || die "no jammy sysroot at $SYSROOT (run portable/make_sysroot.sh)"
[ -f "$DEPS_DIR/lib/libSDL2-2.0.so.0" ] && [ -f "$DEPS_DIR/lib/libavcodec.so.63" ] && [ -f "$DEPS_DIR/lib/libmbedcrypto.so.16" ] ||
    die "dependencies are not built (run portable/build_deps.sh)"

# ---------------------------------------------------------------------------------------------------------
step "source state"
COMMIT="$(git -C "$ROOT" rev-parse HEAD)"
DIRTY_ENTRIES="$(git -C "$ROOT" status --porcelain=v1 --untracked-files=all --ignore-submodules=none | wc -l)"
if [ "$REQUIRE_CLEAN" = 1 ] && [ "$DIRTY_ENTRIES" -gt 0 ]; then
    die "release packaging requires a clean checkout ($DIRTY_ENTRIES modified entries)"
fi
SOURCE_STATE="$COMMIT (clean before dependency bootstrap)"
[ "$DIRTY_ENTRIES" -gt 0 ] && SOURCE_STATE="$COMMIT (modified before dependency bootstrap; NOT release-reproducible)"
(cd "$ROOT" && ./scripts/bootstrap_client_deps.sh)

step "verified downloads"
mkdir -p "$DL"
DB_SRC="$DL/gamecontrollerdb-$CONTROLLER_DB_REV.txt"
DB_LICENSE_SRC="$DL/SDL_GameControllerDB-LICENSE-$CONTROLLER_DB_REV.txt"
MATERIAL_LICENSE_SRC="$DL/Material-Icons-LICENSE-$MATERIAL_REV.txt"
CA_SRC="$DL/cacert-$CA_DATE.pem"
verified_download "https://raw.githubusercontent.com/gabomdq/SDL_GameControllerDB/$CONTROLLER_DB_REV/gamecontrollerdb.txt" \
    "$CONTROLLER_DB_SHA" "$DB_SRC"
verified_download "https://raw.githubusercontent.com/gabomdq/SDL_GameControllerDB/$CONTROLLER_DB_REV/LICENSE" \
    "$CONTROLLER_DB_LICENSE_SHA" "$DB_LICENSE_SRC"
verified_download "https://raw.githubusercontent.com/google/material-design-icons/$MATERIAL_REV/LICENSE" \
    "$MATERIAL_LICENSE_SHA" "$MATERIAL_LICENSE_SRC"
verified_download "https://curl.se/ca/cacert-$CA_DATE.pem" "$CA_SHA" "$CA_SRC"

# ---------------------------------------------------------------------------------------------------------
step "build ($CONFIG)"
BUILD_JOBS="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"
[[ "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] || die "CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer"
[ "$CLEAN" = 1 ] && rm -rf "$BUILD"
# __FILE__ and any debug record must not carry the builder's account or checkout path. The build dir sits
# inside the checkout, so its (more specific) mapping comes last: GCC applies the last matching one.
PREFIX_MAPS="-ffile-prefix-map=$ROOT=/src -ffile-prefix-map=$BUILD=/build"
cmake -S "$ROOT" -B "$BUILD" -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE="$CONFIG" -DBUILD_TESTS=OFF \
    -DCMAKE_C_FLAGS="$CFLAGS $PREFIX_MAPS" -DCMAKE_CXX_FLAGS="$CXXFLAGS $PREFIX_MAPS" > "$BUILD.configure.log" 2>&1 ||
    { tail -30 "$BUILD.configure.log"; die "configure failed"; }
cmake --build "$BUILD" -j "$BUILD_JOBS" > "$BUILD.build.log" 2>&1 || { tail -40 "$BUILD.build.log"; die "build failed"; }

# ---------------------------------------------------------------------------------------------------------
step "assemble $PKG"
rm -rf "$PKG" "$ARCHIVE" "$ARCHIVE.sha256"
mkdir -p "$PKG/bin" "$PKG/lib" "$PKG/share/locale" "$PKG/licenses/source" "$PKG/licenses/external" \
    "$PKG/licenses/dependencies"

install -m755 "$(built moonlight-tv)" "$PKG/bin/moonlight-tv"
# The client's own libraries and the SS4S modules. ss4s_modules.ini is read beside ss4s_modules.so (dladdr);
# the modules are dlopen()ed by bare name from the executable, i.e. through ITS RUNPATH ($ORIGIN/../lib).
for f in libgamestream.so libmoonlight-common-c.so libmicrodns.so.1; do install -m644 "$(built "$f")" "$PKG/lib/$f"; done
MODULES_DIR="$(dirname "$(built ss4s_modules.so)")"
for f in "$MODULES_DIR"/*.so; do install -m644 "$f" "$PKG/lib/$(basename "$f")"; done
install -m644 "$MODULES_DIR/ss4s_modules.ini" "$PKG/lib/ss4s_modules.ini"

# The NEEDED closure, like the Windows PE import walk: every library a packaged binary needs is packaged,
# a declared system library, or a declared fallback -- anything else fails the build.
queue=("$PKG/bin/moonlight-tv" "$PKG"/lib/*.so*)
declare -A seen=()
while [ ${#queue[@]} -gt 0 ]; do
    f="${queue[0]}"; queue=("${queue[@]:1}")
    for n in $(needed "$f"); do
        [ -n "${seen[$n]:-}" ] && continue
        seen[$n]=1
        [ -e "$PKG/lib/$n" ] && continue
        if [ -e "$DEPS_DIR/lib/$n" ]; then
            install -m644 "$(readlink -f "$DEPS_DIR/lib/$n")" "$PKG/lib/$n"
            queue+=("$PKG/lib/$n")
            continue
        fi
        case " $GLIBC_LIBS $DESKTOP_LIBS $OPTIONAL_LIBS " in *" $n "*) continue ;; esac
        case " $FALLBACK_LIBS " in *":$n "*) continue ;; esac
        die "$(basename "$f") needs $n: not packaged, not a declared system library, not a fallback"
    done
done
for pair in $FALLBACK_LIBS; do
    mkdir -p "$PKG/lib/fallback/${pair%%:*}"
    install -m644 "$(readlink -f "$SYSROOT/usr/lib/x86_64-linux-gnu/${pair#*:}")" "$PKG/lib/fallback/${pair%%:*}/${pair#*:}"
done

# Relocatable: nothing may point back into the build tree or an absolute path.
patchelf --set-rpath '$ORIGIN/../lib' "$PKG/bin/moonlight-tv"
for f in "$PKG"/lib/*.so*; do patchelf --set-rpath '$ORIGIN' "$f"; done
for f in "$PKG"/lib/fallback/*/*; do patchelf --remove-rpath "$f"; done

# Assets the client reads beside its executable (path_assets() = the executable's directory).
install -m644 "$DB_SRC" "$PKG/bin/gamecontrollerdb.txt"
install -m644 "$ROOT/deploy/webos/assets/gamecontrollerdb_extra.txt" "$PKG/bin/gamecontrollerdb_extra.txt"
install -m644 "$CA_SRC" "$PKG/bin/curl-ca-bundle.crt"

# Translations: the launcher points TEXTDOMAINDIR here (on Windows the client derives the same share/locale).
MO_COUNT=0
for mo in "$BUILD"/mo/*/LC_MESSAGES/moonlight-tv.mo; do
    lang="$(basename "$(dirname "$(dirname "$mo")")")"
    install -Dm644 "$mo" "$PKG/share/locale/$lang/LC_MESSAGES/moonlight-tv.mo"
    MO_COUNT=$((MO_COUNT + 1))
done
[ "$MO_COUNT" -gt 0 ] || die "no translations were built"

# Menu icon: the Windows icon's PNG images, unchanged.
python3 - "$ROOT/res/eclipse.ico" "$PKG/share/icons/hicolor" << 'EOF'
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
    open(os.path.join(out, 'eclipse.png'), 'wb').write(blob)
    sizes.add(w)
if sizes != {16, 24, 32, 48, 64, 128, 256}:
    sys.exit(f'unexpected icon sizes {sorted(sizes)}')
EOF

install -m755 "$ROOT/deploy/linux-portable/eclipse" "$PKG/eclipse"
for script in install-desktop-entry.sh install.sh uninstall.sh package-functions.sh; do
    install -m755 "$ROOT/deploy/linux-portable/$script" "$PKG/$script"
done
printf 'eclipse-linux-v1\n' > "$PKG/.eclipse-package"
install -m644 "$ROOT/deploy/linux-portable/README.txt" "$PKG/README.txt"

# ---------------------------------------------------------------------------------------------------------
step "licences and notices"
install -m644 "$ROOT/LICENSE.txt" "$PKG/LICENSE.txt"
install -m644 "$ROOT/COPYRIGHT" "$ROOT/ACKNOWLEDGEMENTS.md" "$PKG/"
install -m644 "$ROOT"/docs/licenses/common/* "$PKG/licenses/external/"
install -m644 "$ROOT/docs/licenses/webos/Moonlight-Common-C-Reed-Solomon-BSD.txt" \
    "$ROOT/docs/licenses/webos/libmicrodns-compat-ISC-style.txt" "$PKG/licenses/source/"
install -m644 "$ROOT/third_party/libmicrodns/AUTHORS" "$PKG/licenses/source/libmicrodns-AUTHORS.txt"
install -m644 "$DB_LICENSE_SRC" "$PKG/licenses/external/SDL_GameControllerDB-LICENSE.txt"
install -m644 "$MATERIAL_LICENSE_SRC" "$PKG/licenses/external/Material-Icons-Apache-2.0.txt"
# name | source | path in the checkout | file name in licenses/source
SOURCE_LICENSES="moonlight-common-c|https://github.com/moonlight-stream/moonlight-common-c|core/moonlight-common-c/LICENSE.txt|moonlight-common-c-GPL-3.0.txt
ENet|https://github.com/lsalzman/enet|core/moonlight-common-c/enet/LICENSE|ENet-MIT.txt
SS4S|https://github.com/mariotaku/ss4s|third_party/ss4s/LICENSE|SS4S-LGPL-3.0.txt
commons-c|https://github.com/mariotaku/commons-c|third_party/commons/LICENSE|commons-c-MIT.txt
LVGL|https://github.com/lvgl/lvgl|third_party/lvgl/LICENCE.txt|LVGL-MIT.txt
h264bitstream|https://github.com/aizvorski/h264bitstream|third_party/h264bitstream/LICENSE|h264bitstream-LGPL-2.1.txt
libmicrodns|https://github.com/videolabs/libmicrodns|third_party/libmicrodns/COPYING|libmicrodns-LGPL-2.1.txt"
while IFS='|' read -r name url path file; do
    [ -f "$ROOT/$path" ] || die "source licence for $name missing at $path"
    install -m644 "$ROOT/$path" "$PKG/licenses/source/$file"
done <<< "$SOURCE_LICENSES"

# The pinned dependency sources (build_deps.sh): licence files straight from the verified tarballs.
eval "$(sed -n '/^declare -A URL SHA/,/^$/p' "$PORTABLE/build_deps.sh")"
tarball() { local name="${URL[$1]##*/}"; [ "$1" = nvcodec ] && name=nv-codec-headers-13.0.19.1.tar.gz
            [ "$1" = inih ] && name=inih-r58.tar.gz; echo "$ECLIPSE_LINUX_WORK/src/$name"; }
from_tarball() {   # step, member (below the top directory), destination file
    local t; t="$(tarball "$1")"
    echo "${SHA[$1]}  $t" | sha256sum -c --quiet - || die "dependency tarball $t does not match its pin"
    local top; top="$(tar -tf "$t" | sed -n 1p | cut -d/ -f1)"   # sed reads to the end: no SIGPIPE under pipefail
    tar -xOf "$t" "$top/$2" > "$PKG/licenses/dependencies/$3" || die "no $2 in $t"
}
from_tarball sdl2 LICENSE.txt SDL2-Zlib.txt
from_tarball sdl2 src/hidapi/LICENSE.txt SDL2-HIDAPI-licence-options.txt
from_tarball sdl2 src/hidapi/LICENSE-bsd.txt SDL2-HIDAPI-BSD.txt
from_tarball sdl2 src/video/yuv2rgb/LICENSE SDL2-yuv2rgb-BSD.txt
from_tarball sdl2image LICENSE.txt SDL2_image-Zlib.txt
from_tarball ffmpeg COPYING.LGPLv2.1 FFmpeg-LGPL-2.1.txt
from_tarball ffmpeg LICENSE.md FFmpeg-LICENSE.md
from_tarball mbedtls LICENSE mbedTLS-Apache-2.0.txt
from_tarball curl COPYING curl-COPYING.txt
from_tarball opus COPYING Opus-BSD-3-Clause.txt
from_tarball inih LICENSE.txt inih-BSD-3-Clause.txt
from_tarball nvcodec include/ffnvcodec/dynlink_loader.h nv-codec-headers-dynlink_loader.h
python3 - "$PKG/licenses/dependencies/nv-codec-headers-dynlink_loader.h" << 'EOF'
import re, sys
p = sys.argv[1]; text = open(p).read()
m = re.match(r'\s*/\*.*?\*/', text, re.S)          # the MIT licence is the header's leading comment
if not m or 'Permission is hereby granted' not in m.group(0): sys.exit('nv-codec-headers licence comment not found')
open(p, 'w').write(m.group(0) + '\n')
EOF
mv "$PKG/licenses/dependencies/nv-codec-headers-dynlink_loader.h" "$PKG/licenses/dependencies/nv-codec-headers-MIT.txt"
install -m644 "$SYSROOT/usr/share/doc/libva-drm2/copyright" "$PKG/licenses/dependencies/libva-copyright.txt"
install -m644 "$SYSROOT/usr/share/doc/libdrm2/copyright" "$PKG/licenses/dependencies/libdrm-copyright.txt"

dep_version() { local n="${URL[$1]##*/}"; n="${n%.tar.*}"; echo "$n"; }
{
    echo 'Eclipse Linux portable package - third-party notices'
    echo
    echo 'Full licence texts are under licenses/. Eclipse itself is distributed under GPL-3.0; see LICENSE.txt.'
    echo
    echo 'SOURCE COMPONENTS COMPILED INTO THE CLIENT'
    while IFS='|' read -r name url path file; do
        echo "- $name"; echo "  Source: $url"; echo "  Licence: licenses/source/$file"
    done <<< "$SOURCE_LICENSES"
    echo
    echo 'LIBRARIES BUILT FROM PINNED SOURCE (exact tarball URL and SHA-256 in SOURCE-MANIFEST.txt)'
    echo "- $(dep_version sdl2) (Zlib): lib/libSDL2-2.0.so.0 -- licenses/dependencies/SDL2-Zlib.txt"
    echo "- $(dep_version sdl2image) (Zlib): lib/libSDL2_image-2.0.so.0 -- licenses/dependencies/SDL2_image-Zlib.txt"
    echo "- $(dep_version ffmpeg) (LGPL-2.1-or-later; built without GPL or non-free parts): lib/libavcodec, libavutil,"
    echo "  libswscale -- licenses/dependencies/FFmpeg-LGPL-2.1.txt, FFmpeg-LICENSE.md"
    echo "- $(dep_version mbedtls) (Apache-2.0): lib/libmbedtls, libmbedx509, libmbedcrypto -- licenses/dependencies/mbedTLS-Apache-2.0.txt"
    echo "- $(dep_version curl) (curl licence; with patches/curl-8.22.0-mbedtls-pem-key-blob.patch): lib/libcurl.so.4 --"
    echo "  licenses/dependencies/curl-COPYING.txt"
    echo "- $(dep_version opus) (BSD-3-Clause), linked into bin/moonlight-tv -- licenses/dependencies/Opus-BSD-3-Clause.txt"
    echo "- inih r58 (BSD-3-Clause), linked into bin/moonlight-tv -- licenses/dependencies/inih-BSD-3-Clause.txt"
    echo "- nv-codec-headers n13.0.19.1 (MIT), compiled into FFmpeg -- licenses/dependencies/nv-codec-headers-MIT.txt"
    echo
    echo 'UBUNTU 22.04 LIBRARIES SHIPPED AS FALLBACKS (used only when the system lacks them)'
    echo "- libva / libva-drm (MIT): lib/fallback/libva, lib/fallback/libva-drm -- licenses/dependencies/libva-copyright.txt"
    echo "- libdrm (MIT): lib/fallback/libdrm -- licenses/dependencies/libdrm-copyright.txt"
    echo
    echo 'EXTERNAL DATA AND EMBEDDED ASSETS'
    echo '- LVGL embedded fonts (Montserrat, Font Awesome, Unscii): licenses/external/LVGL-Embedded-Fonts-NOTICE.md'
    echo '- Moonlight Common C Reed-Solomon: licenses/source/Moonlight-Common-C-Reed-Solomon-BSD.txt'
    echo '- libmicrodns compatibility code and authors: licenses/source/libmicrodns-compat-ISC-style.txt, libmicrodns-AUTHORS.txt'
    echo '- SDL2 embedded HIDAPI (BSD option) and yuv2rgb: licenses/dependencies/SDL2-HIDAPI-BSD.txt, SDL2-yuv2rgb-BSD.txt'
    echo "- SDL_GameControllerDB at revision $CONTROLLER_DB_REV: bin/gamecontrollerdb.txt"
    echo '  Source: https://github.com/gabomdq/SDL_GameControllerDB -- licenses/external/SDL_GameControllerDB-LICENSE.txt'
    echo "- Google Material Icons (embedded font; licence pinned at revision $MATERIAL_REV)"
    echo '  Source: https://github.com/google/material-design-icons -- licenses/external/Material-Icons-Apache-2.0.txt'
    echo "- Mozilla CA certificate list as of $CA_DATE: bin/curl-ca-bundle.crt"
    echo "  Source: https://curl.se/ca/cacert-$CA_DATE.pem -- Mozilla Public License 2.0, licenses/external/MPL-2.0.txt"
} > "$PKG/THIRD-PARTY-NOTICES.txt"

# ---------------------------------------------------------------------------------------------------------
step "audits"
ELFS=()
while IFS= read -r -d '' f; do is_elf "$f" && ELFS+=("$f"); done < <(find "$PKG" -type f -print0)
[ ${#ELFS[@]} -gt 10 ] || die "only ${#ELFS[@]} ELF files packaged"

# 1. RUNPATH: $ORIGIN-relative everywhere, never an RPATH or an absolute directory.
for f in "${ELFS[@]}"; do
    rel="${f#$PKG/}"
    readelf -d "$f" | grep '(RPATH)' > /dev/null && die "$rel has a DT_RPATH"
    rp="$(readelf -d "$f" | sed -n 's/.*(RUNPATH).*\[\(.*\)\]/\1/p')"
    case "$rel" in
        bin/moonlight-tv) [ "$rp" = '$ORIGIN/../lib' ] || die "$rel RUNPATH is '$rp'" ;;
        lib/fallback/*) [ -z "$rp" ] || die "$rel RUNPATH is '$rp'" ;;
        *) [ "$rp" = '$ORIGIN' ] || die "$rel RUNPATH is '$rp'" ;;
    esac
done

# 2. The glibc floor: no binary may need a symbol version newer than the jammy sysroot's glibc.
MAX_GLIBC="$(for f in "${ELFS[@]}"; do readelf -V -W "$f" | grep -o 'Name: GLIBC_[0-9.]*' | cut -d_ -f2; done | sort -uV | tail -1)"
[ "$(printf '%s\n%s\n' "$MAX_GLIBC" "$GLIBC_FLOOR" | sort -V | tail -1)" = "$GLIBC_FLOOR" ] ||
    die "a binary needs GLIBC_$MAX_GLIBC, above the $GLIBC_FLOOR floor"
for f in "${ELFS[@]}"; do readelf -V -W "$f" | grep 'Name: GLIBCXX_\|Name: CXXABI_' > /dev/null && die "${f#$PKG/} needs libstdc++"; done

# 3. No builder paths in anything this build produced (the Ubuntu fallbacks are Ubuntu's own builds).
for f in "${ELFS[@]}"; do
    case "$f" in "$PKG/lib/fallback/"*) continue ;; esac
    for p in "$ROOT" "$BUILD" "$ECLIPSE_LINUX_WORK" "$SYSROOT" "$HOME"; do
        # grep WITHOUT -q: an early exit would SIGPIPE strings, and pipefail would then hide the match
        strings -a "$f" | grep -F "$p" > /dev/null && die "builder path '$p' is embedded in ${f#$PKG/}"
    done
done

# 4. The ffmpeg video module is declared and every declared module is packaged.
python3 - "$PKG/lib/ss4s_modules.ini" "$PKG/lib" << 'EOF'
import configparser, os, sys
ini = configparser.ConfigParser(); ini.read(sys.argv[1])
if not (ini.has_section('ffmpeg') and ini.get('ffmpeg', 'video', fallback='') == 'true'):
    sys.exit('ss4s_modules.ini does not declare the ffmpeg video module')
for section in ini.sections():
    if not os.path.exists(os.path.join(sys.argv[2], f'ss4s-{section}.so')):
        sys.exit(f'module {section} is declared but ss4s-{section}.so is not packaged')
EOF

# 5. Every packaged binary loads under the jammy loader and C library, i.e. on the oldest supported system.
# (libpulse finds its private libpulsecommon through an ABSOLUTE runpath; on a real jammy system that is the
#  sysroot's pulseaudio directory, here it would be the build host's -- so name the sysroot's explicitly.)
JAMMY_PATH="$PKG/lib:$SYSROOT/lib/x86_64-linux-gnu:$SYSROOT/usr/lib/x86_64-linux-gnu:$SYSROOT/usr/lib/x86_64-linux-gnu/pulseaudio"
for f in "${ELFS[@]}"; do
    case "$f" in "$PKG/lib/fallback/"*) continue ;; esac
    out="$("$SYSROOT/lib64/ld-linux-x86-64.so.2" --library-path "$JAMMY_PATH" --list "$f" 2>&1)" ||
        die "${f#$PKG/} does not load on jammy: $out"
    case "$out" in *'not found'*) die "${f#$PKG/} on jammy: $(echo "$out" | grep 'not found')" ;; esac
done

for s in eclipse install-desktop-entry.sh install.sh uninstall.sh package-functions.sh; do sh -n "$PKG/$s" || die "$s has a syntax error"; done

# ---------------------------------------------------------------------------------------------------------
step "manifests"
{
    echo 'Eclipse Linux source manifest'
    echo "Root source: $SOURCE_STATE"
    echo "Root dirty entries before bootstrap: $DIRTY_ENTRIES"
    echo "Bootstrap script: scripts/bootstrap_client_deps.sh sha256=$(sha256sum "$ROOT/scripts/bootstrap_client_deps.sh" | cut -d' ' -f1)"
    echo
    echo 'Resolved client dependencies (diff is the Git blob hash of the applied worktree delta):'
    for d in core/moonlight-common-c core/moonlight-common-c/enet third_party/commons third_party/libmicrodns \
             third_party/lvgl third_party/ss4s; do
        [ -e "$ROOT/$d/.git" ] || die "client dependency $d is not initialised"
        echo "$d $(git -C "$ROOT/$d" rev-parse HEAD) diff=$(git -C "$ROOT/$d" diff --binary --no-ext-diff HEAD | git hash-object --stdin) status_entries=$(git -C "$ROOT/$d" status --porcelain=v1 --untracked-files=all | wc -l)"
    done
    echo
    echo 'Repository-owned patches:'
    for p in "$ROOT"/patches/client-deps/* "$PORTABLE"/patches/*; do
        echo "${p#$ROOT/} sha256=$(sha256sum "$p" | cut -d' ' -f1)"
    done
    echo
    echo 'Dependencies built from source (scripts/linux/portable/build_deps.sh):'
    for s in mbedtls curl opus inih nvcodec sdl2 sdl2image ffmpeg; do echo "$s ${URL[$s]} sha256=${SHA[$s]}"; done
    echo
    echo "Sysroot: Ubuntu 22.04 packages ($(ls "$ECLIPSE_LINUX_WORK/sysroot-debs" | wc -l) .deb files):"
    ls "$ECLIPSE_LINUX_WORK/sysroot-debs" | LC_ALL=C sort | sed 's/^/  /'
} > "$PKG/SOURCE-MANIFEST.txt"

# Read the configured top-level version, not a nested dependency's project().
PROJECT_VERSION=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$BUILD/CMakeCache.txt")
[[ "$PROJECT_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "Invalid configured Eclipse version: $PROJECT_VERSION"

BUILDINFO_CONTRACT=(
    "Version: $PROJECT_VERSION"
    'Code signing: NONE (unsigned community build)'
    "Target: x86_64 Linux with glibc $GLIBC_FLOOR or newer (linked against Ubuntu 22.04 libraries)"
    'Hardware video decode: NVDEC (NVIDIA) then VA-API (Intel/AMD) when the driver allows; software decode otherwise'
    'Decoder override: ECLIPSE_DIAGNOSTIC_VIDEO_DECODER=software|nvdec|vaapi'
    'Presentation path: decode, GPU-to-CPU readback and I420 conversion (not zero-copy)'
    'TLS: one shared mbedTLS 3.6.7 built with MBEDTLS_THREADING_C, used by curl 8.22.0'
    "System libraries (not bundled): glibc, $DESKTOP_LIBS, X11/Wayland/GL, PulseAudio/ALSA"
    'Bundled fallbacks (used only when the system lacks them): libva.so.2, libva-drm.so.2, libdrm.so.2 from Ubuntu 22.04'
)
{
    echo 'Eclipse Linux portable build'
    echo "Source: $SOURCE_STATE"
    echo "Configuration: $CONFIG"
    printf '%s\n' "${BUILDINFO_CONTRACT[@]}"
    echo "Highest glibc symbol version needed: GLIBC_$MAX_GLIBC"
    echo "Controller database revision: $CONTROLLER_DB_REV"
    echo "Controller database SHA-256: $CONTROLLER_DB_SHA"
    echo "Controller database licence SHA-256: $CONTROLLER_DB_LICENSE_SHA"
    echo "TLS roots: Mozilla CA list $CA_DATE (curl-ca-bundle.crt, SHA-256 $CA_SHA)"
    cmake --version | sed -n 1p
    gcc --version | sed -n 1p
    echo "Sysroot glibc: $(readlink -f "$SYSROOT/lib/x86_64-linux-gnu/libc.so.6" | xargs basename) ($(ls "$ECLIPSE_LINUX_WORK/sysroot-debs" | grep '^libc6_' | sed -n 1p))"
} > "$PKG/BUILDINFO.txt"
for line in "${BUILDINFO_CONTRACT[@]}"; do
    grep -qxF "$line" "$PKG/BUILDINFO.txt" || die "BUILDINFO.txt does not record: $line"
done

# Written outside the folder being listed, or the list would include its own temporary file.
(cd "$PKG" && find . -type f ! -name SHA256SUMS -printf '%P\n' | LC_ALL=C sort | tr '\n' '\0' | xargs -0 sha256sum) > "$BUILD/SHA256SUMS.tmp"
mv "$BUILD/SHA256SUMS.tmp" "$PKG/SHA256SUMS"

if [ "$MAKE_ARCHIVE" = 1 ]; then
    step "archive"
    EPOCH="$(git -C "$ROOT" log -1 --format=%ct)"
    tar -C "$ROOT/dist" --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$EPOCH" -cf - "$PKG_NAME" | gzip -n -9 > "$ARCHIVE"
    (cd "$ROOT/dist" && sha256sum "$PKG_NAME.tar.gz") > "$ARCHIVE.sha256"
    echo "Archive: $ARCHIVE ($(du -h "$ARCHIVE" | cut -f1))"
    cat "$ARCHIVE.sha256"
fi
echo "Portable package: $PKG"

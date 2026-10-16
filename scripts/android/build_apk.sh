#!/usr/bin/env bash
# build_apk.sh -- build the Android TV client and package it: dist/eclipse-android.apk (+ .sha256)
#
# The Android twin of scripts/linux/build_portable.sh: the same pinned controller database, CA bundle, licences
# and third-party notices, built once per ABI with CMake and the NDK, then packaged by deploy/android (Gradle).
# Prerequisites: an Android SDK (platform 35, build-tools) and NDK r27+, JDK 17+, cmake, ninja, pkg-config,
# gettext (msgfmt), and the dependencies from scripts/android/build_deps.sh (run automatically if missing).
#
#   --abi <abi>           arm64-v8a | armeabi-v7a | x86_64 (repeatable; default: all three)
#   --debug               a debuggable APK (assembleDebug) instead of the release build
#   --selftest            a TEST APK with the emulator self-test hooks (never a release)
#   --clean               start every ABI from an empty build directory
#   --require-clean-tree  refuse a modified checkout (release builds)
#
# Signing: ECLIPSE_ANDROID_KEYSTORE, ECLIPSE_ANDROID_KEYSTORE_PASSWORD, ECLIPSE_ANDROID_KEY_ALIAS and
# ECLIPSE_ANDROID_KEY_PASSWORD sign the release APK with the project's key. Without them it is signed with the
# local debug key and named eclipse-android-debug-signed.apk: it installs, but cannot update a release install.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
: "${ECLIPSE_ANDROID_DEPS:=$ROOT/build/android-deps}"
export ECLIPSE_ANDROID_DEPS
STAGE="$ROOT/build/android-stage"
export ECLIPSE_ANDROID_STAGE="$STAGE"
SYMBOLS="$ROOT/build/android-symbols"
DL="$ROOT/build/downloads-android"
API=28
ABIS=()
CLEAN=0; REQUIRE_CLEAN=0; VARIANT=release; SELFTEST=OFF
while [ $# -gt 0 ]; do
    case "$1" in
        --abi) ABIS+=("$2"); shift 2 ;;
        --debug) VARIANT=debug; shift ;;
        --selftest) SELFTEST=ON; shift ;;
        --clean) CLEAN=1; shift ;;
        --require-clean-tree) REQUIRE_CLEAN=1; shift ;;
        *) echo "usage: $0 [--abi <abi>]... [--debug] [--selftest] [--clean] [--require-clean-tree]" >&2; exit 2 ;;
    esac
done
[ ${#ABIS[@]} -gt 0 ] || ABIS=(arm64-v8a armeabi-v7a x86_64)

die() { echo "build_apk: $*" >&2; exit 1; }
step() { echo "== $*"; }

NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-${ANDROID_NDK_LATEST_HOME:-}}}"
[ -n "$NDK" ] && [ -f "$NDK/build/cmake/android.toolchain.cmake" ] || die "set ANDROID_NDK_HOME to an Android NDK"
LLVM="$(echo "$NDK"/toolchains/llvm/prebuilt/*/bin)"
[ -x "$LLVM/llvm-readelf" ] && [ -x "$LLVM/llvm-strip" ] || die "no llvm tools in $LLVM"
for t in cmake ninja pkg-config msgfmt curl sha256sum python3 git java; do
    command -v "$t" >/dev/null || die "'$t' is required"
done
needed() { "$LLVM/llvm-readelf" -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }

# Libraries every Android device provides to apps (the NDK's stable system libraries this program links).
SYSTEM_LIBS="libc.so libm.so libdl.so liblog.so libandroid.so libmediandk.so libEGL.so libGLESv1_CM.so \
libGLESv2.so libGLESv3.so libOpenSLES.so libz.so libjnigraphics.so"

# The same downloads as the desktop packages, from the same pins (scripts/linux/build_portable.sh).
eval "$(grep -E '^(CONTROLLER_DB_REV|CONTROLLER_DB_SHA|CONTROLLER_DB_LICENSE_SHA|MATERIAL_REV|MATERIAL_LICENSE_SHA|CA_DATE|CA_SHA)=' \
    "$ROOT/scripts/linux/build_portable.sh")"
[ -n "${CONTROLLER_DB_REV:-}" ] && [ -n "${CA_SHA:-}" ] || die "could not read the pins from scripts/linux/build_portable.sh"
verified_download() {   # url sha256 destination
    if [ -f "$3" ] && echo "$2  $3" | sha256sum -c --quiet - 2>/dev/null; then return 0; fi
    local tmp="$3.download.$$"
    curl -fsSL --retry 3 -o "$tmp" "$1" || { rm -f "$tmp"; die "download failed: $1"; }
    echo "$2  $tmp" | sha256sum -c --quiet - || { rm -f "$tmp"; die "hash mismatch for $1"; }
    mv -f "$tmp" "$3"
}

# ---------------------------------------------------------------------------------------------------------
step "source state"
COMMIT="$(git -C "$ROOT" rev-parse HEAD)"
DIRTY_ENTRIES="$(git -C "$ROOT" status --porcelain=v1 --untracked-files=all --ignore-submodules=none | wc -l)"
if [ "$REQUIRE_CLEAN" = 1 ] && [ "$DIRTY_ENTRIES" -gt 0 ]; then
    die "release packaging requires a clean checkout ($DIRTY_ENTRIES modified entries)"
fi
(cd "$ROOT" && ./scripts/bootstrap_client_deps.sh)

step "dependencies"
# Cheap when they are current: build_deps.sh skips every step whose stamp matches its pins.
DEPS_ARGS=()
for abi in "${ABIS[@]}"; do DEPS_ARGS+=(--abi "$abi"); done
"$ROOT/scripts/android/build_deps.sh" "${DEPS_ARGS[@]}"

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
rm -rf "$STAGE" "$SYMBOLS"
mkdir -p "$STAGE/jniLibs" "$STAGE/assets/eclipse" "$SYMBOLS"
MODULES_INI=""
CMAKE_BUILD_TYPE=RelWithDebInfo
[ "$VARIANT" = debug ] && CMAKE_BUILD_TYPE=Debug
for abi in "${ABIS[@]}"; do
    BUILD="$ROOT/build/android-$abi"
    step "build $abi ($CMAKE_BUILD_TYPE)"
    [ "$CLEAN" = 1 ] && rm -rf "$BUILD"
    # __FILE__ and debug records must not carry the builder's checkout path.
    PREFIX_MAPS="-ffile-prefix-map=$ROOT=/src -ffile-prefix-map=$BUILD=/build"
    cmake -S "$ROOT" -B "$BUILD" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI="$abi" \
        -DANDROID_PLATFORM="android-$API" -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
        -DECLIPSE_ANDROID_DEPS="$ECLIPSE_ANDROID_DEPS" -DCMAKE_INSTALL_PREFIX="$BUILD/install" \
        -DECLIPSE_ANDROID_SELFTEST="$SELFTEST" \
        -DCMAKE_C_FLAGS="$PREFIX_MAPS" -DCMAKE_CXX_FLAGS="$PREFIX_MAPS" >"$BUILD.configure.log" 2>&1 ||
        { tail -40 "$BUILD.configure.log"; die "configure failed for $abi"; }
    cmake --build "$BUILD" >"$BUILD.build.log" 2>&1 || { tail -60 "$BUILD.build.log"; die "build failed for $abi"; }

    OUT="$STAGE/jniLibs/$abi"
    mkdir -p "$OUT" "$SYMBOLS/$abi"
    built() { local f; f="$(find "$BUILD" -name "$1" -not -path '*/CMakeFiles/*' -print -quit)"; [ -n "$f" ] || die "the $abi build did not produce $1"; echo "$f"; }
    install -m644 "$(built libmain.so)" "$OUT/libmain.so"
    install -m755 "$(built libeclipse_peer.so)" "$OUT/libeclipse_peer.so"
    # Shared on every platform but Windows (src/app/backend/pcmanager/discovery/impl), as the Linux package ships it.
    install -m644 "$(built libmicrodns.so)" "$OUT/libmicrodns.so"
    if [ "$SELFTEST" = ON ]; then   # tests/android/test_remote_uinput.c, for tests/android/device_smoke.sh
        install -m755 "$(built libeclipse_test_remote.so)" "$OUT/libeclipse_test_remote.so"
    fi
    MODULES_DIR="$(dirname "$(built libss4s-mediacodec.so)")"
    for f in "$MODULES_DIR"/libss4s-*.so; do install -m644 "$f" "$OUT/$(basename "$f")"; done
    [ -f "$OUT/libss4s-sdl.so" ] || die "the $abi build has no SDL audio module"
    # mbedTLS must take its entropy from /dev/urandom: /dev/random blocks on older kernels (build_deps.sh).
    grep -aq '/dev/urandom' "$ECLIPSE_ANDROID_DEPS/$abi/lib/libmbedcrypto.so" &&
        ! grep -aq '/dev/random' "$ECLIPSE_ANDROID_DEPS/$abi/lib/libmbedcrypto.so" ||
        die "$abi: libmbedcrypto.so would read /dev/random (see MBEDTLS_PLATFORM_DEV_RANDOM in build_deps.sh)"
    if [ -z "$MODULES_INI" ]; then MODULES_INI="$MODULES_DIR/ss4s_modules.ini"; fi
    cmp -s "$MODULES_INI" "$MODULES_DIR/ss4s_modules.ini" || die "the module list differs between ABIs"

    # The NEEDED closure: every library a packaged binary needs is packaged or an Android system library.
    queue=("$OUT"/*.so)
    declare -A seen=()
    while [ ${#queue[@]} -gt 0 ]; do
        f="${queue[0]}"; queue=("${queue[@]:1}")
        for n in $(needed "$f"); do
            [ -n "${seen[$n]:-}" ] && continue
            seen[$n]=1
            [ -e "$OUT/$n" ] && continue
            if [ -e "$ECLIPSE_ANDROID_DEPS/$abi/lib/$n" ]; then
                install -m644 "$ECLIPSE_ANDROID_DEPS/$abi/lib/$n" "$OUT/$n"
                queue+=("$OUT/$n")
                continue
            fi
            case " $SYSTEM_LIBS " in *" $n "*) continue ;; esac
            die "$abi: $(basename "$f") needs $n: not packaged and not an Android system library"
        done
    done
    unset seen
    # Every name must be lib*.so: the package manager extracts nothing else into the native library directory.
    for f in "$OUT"/*; do case "$(basename "$f")" in lib*.so) ;; *) die "$abi: $(basename "$f") would not be installed" ;; esac; done
    # The peer finds its libraries beside it. It runs in the default linker namespace, where each object's own
    # RUNPATH is the only way to a packaged library: whatever needs one carries $ORIGIN (CMakeLists.txt).
    for f in "$OUT"/*.so; do
        for n in $(needed "$f"); do
            [ -e "$OUT/$n" ] || continue
            "$LLVM/llvm-readelf" -d "$f" | grep -q 'RUNPATH.*\[\$ORIGIN\]' ||
                die "$abi: $(basename "$f") needs $n beside it but its RUNPATH is not \$ORIGIN"
            break
        done
    done
    # Unstripped copies for symbolising crash records; the APK gets stripped ones.
    for f in "$OUT"/*.so; do
        cp "$f" "$SYMBOLS/$abi/"
        "$LLVM/llvm-strip" --strip-unneeded "$f"
    done
done

# ---------------------------------------------------------------------------------------------------------
step "assets"
A="$STAGE/assets/eclipse"
install -m644 "$MODULES_INI" "$A/ss4s_modules.ini"
grep -q '^\[mediacodec\]' "$A/ss4s_modules.ini" || die "the MediaCodec module is not in the module list"
grep -q '^\[sdl\]' "$A/ss4s_modules.ini" || die "the SDL audio module is not in the module list"
install -m644 "$DB_SRC" "$A/gamecontrollerdb.txt"
install -m644 "$ROOT/deploy/webos/assets/gamecontrollerdb_extra.txt" "$A/gamecontrollerdb_extra.txt"
install -m644 "$CA_SRC" "$A/curl-ca-bundle.crt"
MO_COUNT=0
for mo in "$ROOT/build/android-${ABIS[0]}"/mo/*/LC_MESSAGES/moonlight-tv.mo; do
    lang="$(basename "$(dirname "$(dirname "$mo")")")"
    install -Dm644 "$mo" "$A/locale/$lang/LC_MESSAGES/moonlight-tv.mo"
    MO_COUNT=$((MO_COUNT + 1))
done
[ "$MO_COUNT" -gt 0 ] || die "no translations were built"

step "licences and notices"
L="$A/licenses"
mkdir -p "$L/source" "$L/external" "$L/dependencies"
install -m644 "$ROOT/LICENSE.txt" "$ROOT/COPYRIGHT" "$ROOT/ACKNOWLEDGEMENTS.md" "$A/"
install -m644 "$ROOT"/docs/licenses/common/* "$L/external/"
install -m644 "$ROOT/docs/licenses/webos/Moonlight-Common-C-Reed-Solomon-BSD.txt" \
    "$ROOT/docs/licenses/webos/libmicrodns-compat-ISC-style.txt" "$L/source/"
install -m644 "$ROOT/third_party/libmicrodns/AUTHORS" "$L/source/libmicrodns-AUTHORS.txt"
install -m644 "$DB_LICENSE_SRC" "$L/external/SDL_GameControllerDB-LICENSE.txt"
install -m644 "$MATERIAL_LICENSE_SRC" "$L/external/Material-Icons-Apache-2.0.txt"
SOURCE_LICENSES="moonlight-common-c|core/moonlight-common-c/LICENSE.txt|moonlight-common-c-GPL-3.0.txt
ENet|core/moonlight-common-c/enet/LICENSE|ENet-MIT.txt
SS4S|third_party/ss4s/LICENSE|SS4S-LGPL-3.0.txt
commons-c|third_party/commons/LICENSE|commons-c-MIT.txt
LVGL|third_party/lvgl/LICENCE.txt|LVGL-MIT.txt
h264bitstream|third_party/h264bitstream/LICENSE|h264bitstream-LGPL-2.1.txt
libmicrodns|third_party/libmicrodns/COPYING|libmicrodns-LGPL-2.1.txt"
while IFS='|' read -r name path file; do
    [ -f "$ROOT/$path" ] || die "source licence for $name missing at $path"
    install -m644 "$ROOT/$path" "$L/source/$file"
done <<< "$SOURCE_LICENSES"
# The pinned dependency sources: licence files straight from the verified archives build_deps.sh used.
eval "$(sed -n '/^declare -A URL SHA/,/^STEPS=/p' "$ROOT/scripts/android/build_deps.sh" | grep -v '^STEPS=')"
from_archive() {   # step, member (below the top directory), destination file
    local name="${URL[$1]##*/}"; [ "$1" = inih ] && name=inih-r58.tar.gz
    local t="$ECLIPSE_ANDROID_DEPS/downloads/$name"
    echo "${SHA[$1]}  $t" | sha256sum -c --quiet - || die "dependency archive $t does not match its pin"
    local top; top="$(tar -tf "$t" | sed -n 1p | cut -d/ -f1)"
    tar -xOf "$t" "$top/$2" > "$L/dependencies/$3" || die "no $2 in $t"
}
from_archive sdl2 LICENSE.txt SDL2-Zlib.txt
from_archive sdl2 src/hidapi/LICENSE.txt SDL2-HIDAPI-licence-options.txt
from_archive sdl2 src/hidapi/LICENSE-bsd.txt SDL2-HIDAPI-BSD.txt
from_archive sdl2 src/video/yuv2rgb/LICENSE SDL2-yuv2rgb-BSD.txt
from_archive sdl2image LICENSE.txt SDL2_image-Zlib.txt
from_archive mbedtls LICENSE mbedTLS-Apache-2.0.txt
from_archive curl COPYING curl-COPYING.txt
from_archive opus COPYING Opus-BSD-3-Clause.txt
from_archive inih LICENSE.txt inih-BSD-3-Clause.txt
from_archive expat COPYING expat-MIT.txt
from_archive freetype LICENSE.TXT FreeType-licence-options.txt
from_archive freetype docs/FTL.TXT FreeType-FTL.txt
dep_version() { local n="${URL[$1]##*/}"; echo "${n%.tar.*}"; }
{
    echo 'Eclipse for Android TV - third-party notices'
    echo
    echo 'Full licence texts are in this folder. Eclipse itself is distributed under GPL-3.0; see LICENSE.txt.'
    echo
    echo 'SOURCE COMPONENTS COMPILED INTO THE CLIENT'
    while IFS='|' read -r name path file; do echo "- $name -- licenses/source/$file"; done <<< "$SOURCE_LICENSES"
    echo
    echo 'LIBRARIES BUILT FROM PINNED SOURCE (scripts/android/build_deps.sh: exact archive URL and SHA-256)'
    echo "- $(dep_version sdl2) (Zlib; with scripts/android/patches/sdl2-2.32.10-android-*.patch):"
    echo "  lib/<abi>/libSDL2.so, and its Java half in the app -- licenses/dependencies/SDL2-Zlib.txt"
    echo "- $(dep_version sdl2image) (Zlib), linked into libmain.so -- licenses/dependencies/SDL2_image-Zlib.txt"
    echo "- $(dep_version mbedtls) (Apache-2.0): libmbedtls, libmbedx509, libmbedcrypto -- licenses/dependencies/mbedTLS-Apache-2.0.txt"
    echo "- $(dep_version curl) (curl licence; with scripts/linux/portable/patches/curl-8.22.0-mbedtls-pem-key-blob.patch):"
    echo "  libcurl.so -- licenses/dependencies/curl-COPYING.txt"
    echo "- $(dep_version opus) (BSD-3-Clause), linked into libmain.so -- licenses/dependencies/Opus-BSD-3-Clause.txt"
    echo "- inih r58 (BSD-3-Clause), linked into libmain.so -- licenses/dependencies/inih-BSD-3-Clause.txt"
    echo "- $(dep_version expat) (MIT), linked into libmain.so -- licenses/dependencies/expat-MIT.txt"
    echo "- $(dep_version freetype) (FreeType licence, FTL option), linked into libmain.so -- licenses/dependencies/FreeType-FTL.txt"
    echo
    echo 'EXTERNAL DATA AND EMBEDDED ASSETS'
    echo '- LVGL embedded fonts (Montserrat, Font Awesome, Unscii): licenses/external/LVGL-Embedded-Fonts-NOTICE.md'
    echo '- Moonlight Common C Reed-Solomon: licenses/source/Moonlight-Common-C-Reed-Solomon-BSD.txt'
    echo '- libmicrodns compatibility code and authors: licenses/source/libmicrodns-compat-ISC-style.txt, libmicrodns-AUTHORS.txt'
    echo '- SDL2 embedded HIDAPI (BSD option) and yuv2rgb: licenses/dependencies/SDL2-HIDAPI-BSD.txt, SDL2-yuv2rgb-BSD.txt'
    echo "- SDL_GameControllerDB at revision $CONTROLLER_DB_REV: gamecontrollerdb.txt -- licenses/external/SDL_GameControllerDB-LICENSE.txt"
    echo "- Google Material Icons (embedded font; licence pinned at revision $MATERIAL_REV) -- licenses/external/Material-Icons-Apache-2.0.txt"
    echo "- Mozilla CA certificate list as of $CA_DATE: curl-ca-bundle.crt -- Mozilla Public License 2.0, licenses/external/MPL-2.0.txt"
} > "$A/THIRD-PARTY-NOTICES.txt"
{
    echo "Eclipse for Android TV $(sed -n 's/^project(moonlight VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
    echo "Source: https://github.com/PreceptorOfMagic/Eclipse commit $COMMIT"
    [ "$DIRTY_ENTRIES" -gt 0 ] && echo "Built from a MODIFIED checkout ($DIRTY_ENTRIES entries): not release-reproducible"
    echo "ABIs: ${ABIS[*]}; NDK $(basename "$NDK"); minimum Android API $API"
} > "$A/SOURCE-MANIFEST.txt"

# ---------------------------------------------------------------------------------------------------------
step "package ($VARIANT)"
mkdir -p "$ROOT/dist"
GRADLE_TASK=assembleRelease; APK_DIR=release
[ "$VARIANT" = debug ] && { GRADLE_TASK=assembleDebug; APK_DIR=debug; }
if [ "$SELFTEST" = ON ]; then
    # Debuggable, so tests/android/device_smoke.sh can copy its streams in with run-as, and under its own
    # application ID, so it installs beside a real installation without touching its hosts and settings.
    GRADLE_TASK=assembleDebug; APK_DIR=debug
    export ECLIPSE_ANDROID_APPLICATION_ID=io.github.preceptorofmagic.eclipse.selftest
fi
(cd "$ROOT/deploy/android" && ./gradlew --no-daemon -q "$GRADLE_TASK") || die "Gradle failed"
APK="$(find "$ROOT/deploy/android/app/build/outputs/apk/$APK_DIR" -name '*.apk' -print -quit)"
[ -n "$APK" ] || die "Gradle produced no APK"
if [ "$SELFTEST" = ON ]; then
    NAME=eclipse-android-selftest.apk
elif [ "$VARIANT" = debug ]; then
    NAME=eclipse-android-debug.apk
elif [ -n "${ECLIPSE_ANDROID_KEYSTORE:-}" ]; then
    NAME=eclipse-android.apk
else
    NAME=eclipse-android-debug-signed.apk
fi
install -m644 "$APK" "$ROOT/dist/$NAME"
(cd "$ROOT/dist" && sha256sum "$NAME" > "$NAME.sha256")
step "done: dist/$NAME"

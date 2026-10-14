#!/usr/bin/env bash
# build_deps.sh -- build the Android client's pinned dependencies from verified source, once per ABI:
#   $ECLIPSE_ANDROID_DEPS/<abi>/{include,lib,lib/pkgconfig}   and   $ECLIPSE_ANDROID_DEPS/sdl-java
#
# The Android twin of scripts/linux/portable/build_deps.sh. Versions and archive hashes are THE SAME PINS as the
# Linux and Windows packages (mbedTLS, curl + its PEM-key patch, Opus, SDL2, SDL2_image, inih) so every client
# runs the same library code; expat and FreeType are added because Android has no system copy an app may link.
#
#   --abi <abi>      arm64-v8a | armeabi-v7a | x86_64 (repeatable; default: all three)
#   --api <level>    minimum Android API level (default 28, matches deploy/android minSdk)
#   --clean          rebuild every step even when its stamp is current
#
# Environment: ANDROID_NDK_HOME (or ANDROID_NDK_ROOT / ANDROID_NDK_LATEST_HOME), ECLIPSE_ANDROID_DEPS
# (default: <repo>/build/android-deps). Host tools: cmake >= 3.22, ninja, pkg-config, curl, tar, xz, python3.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
: "${ECLIPSE_ANDROID_DEPS:=$ROOT/build/android-deps}"
OUT="$ECLIPSE_ANDROID_DEPS"
DL="$OUT/downloads"
API=28
ABIS=()
CLEAN=0
while [ $# -gt 0 ]; do
    case "$1" in
        --abi) ABIS+=("$2"); shift 2 ;;
        --api) API="$2"; shift 2 ;;
        --clean) CLEAN=1; shift ;;
        *) echo "usage: $0 [--abi <abi>]... [--api <level>] [--clean]" >&2; exit 2 ;;
    esac
done
[ ${#ABIS[@]} -gt 0 ] || ABIS=(arm64-v8a armeabi-v7a x86_64)

die() { echo "build_deps(android): $*" >&2; exit 1; }

NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-${ANDROID_NDK_LATEST_HOME:-}}}"
[ -n "$NDK" ] && [ -f "$NDK/build/cmake/android.toolchain.cmake" ] ||
    die "set ANDROID_NDK_HOME to an Android NDK (r27 or newer)"
TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
for t in cmake ninja pkg-config curl tar python3; do command -v "$t" >/dev/null || die "'$t' is required"; done
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# --- pins --------------------------------------------------------------------------------------------------
# Keep these equal to scripts/linux/portable/build_deps.sh (the Windows package uses the same versions).
declare -A URL SHA
URL[mbedtls]=https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
SHA[mbedtls]=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
URL[curl]=https://curl.se/download/curl-8.22.0.tar.xz
SHA[curl]=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
URL[opus]=https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz
SHA[opus]=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
URL[sdl2]=https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz
SHA[sdl2]=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
URL[sdl2image]=https://github.com/libsdl-org/SDL_image/releases/download/release-2.8.12/SDL2_image-2.8.12.tar.gz
SHA[sdl2image]=393f5efb50536ec13ca4f4affb69cc9966d3c3f969e6c5e701faddf9f9785381
URL[inih]=https://github.com/benhoyt/inih/archive/refs/tags/r58.tar.gz
SHA[inih]=e79216260d5dffe809bda840be48ab0eec7737b2bb9f02d2275c1b46344ea7b7
# Android only: the desktop packages use the system's copies of these two.
URL[expat]=https://github.com/libexpat/libexpat/releases/download/R_2_9_0/expat-2.9.0.tar.xz
SHA[expat]=1e6371862cc31999b368c3b89b49994f0677e1bab5f1b2b85ae3741f5d803051
URL[freetype]=https://downloads.sourceforge.net/project/freetype/freetype2/2.14.3/freetype-2.14.3.tar.xz
SHA[freetype]=36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f
STEPS=(mbedtls curl opus inih expat freetype sdl2 sdl2image)
CURL_PATCH="$ROOT/scripts/linux/portable/patches/curl-8.22.0-mbedtls-pem-key-blob.patch"
SDL_PATCHES=("$ROOT/scripts/android/patches/sdl2-2.32.10-android-audio-hotplug.patch"
             "$ROOT/scripts/android/patches/sdl2-2.32.10-android-tv-remote.patch")

# A step's stamp records everything that decides its output; a changed pin, patch, NDK or API rebuilds it.
STAMP_INPUTS="api=$API ndk=$(basename "$NDK") script=$(sha256sum "${BASH_SOURCE[0]}" | cut -c1-16)"
STAMP_INPUTS="$STAMP_INPUTS patches=$(cat "$CURL_PATCH" "${SDL_PATCHES[@]}" | sha256sum | cut -c1-16)"

mkdir -p "$DL"
archive_name() {
    case "$1" in inih) echo inih-r58.tar.gz ;; *) echo "${URL[$1]##*/}" ;; esac
}
fetch() {   # $1 = step -> verified archive path
    local name; name="$(archive_name "$1")"
    if [ ! -f "$DL/$name" ]; then
        curl -fsSL --retry 3 -o "$DL/$name.part" "${URL[$1]}" || die "download failed: ${URL[$1]}"
        mv "$DL/$name.part" "$DL/$name"
    fi
    echo "${SHA[$1]}  $DL/$name" | sha256sum -c --quiet - >&2 || { rm -f "$DL/$name"; die "CHECKSUM MISMATCH $name"; }
    echo "$DL/$name"
}
unpack() {   # $1 = step, $2 = abi -> fresh source dir for this ABI's build
    local archive dir; archive="$(fetch "$1")"
    dir="$OUT/work/$2/$1"
    rm -rf "$dir"; mkdir -p "$dir"
    tar -xf "$archive" -C "$dir" --strip-components=1
    echo "$dir"
}

cmake_dep() {   # $1 = abi, $2 = source dir, rest = cmake options
    local abi=$1 src=$2; shift 2
    local prefix="$OUT/$abi"
    cmake -S "$src" -B "$src/_build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$API" \
        -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_FIND_ROOT_PATH="$prefix" -DCMAKE_PREFIX_PATH="$prefix" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        "-DCMAKE_C_FLAGS=-ffile-prefix-map=$OUT=/deps" \
        "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384" \
        "$@" >"$src/_configure.log" 2>&1 || { tail -40 "$src/_configure.log"; die "configure failed: $src"; }
    cmake --build "$src/_build" -j "$JOBS" >"$src/_build.log" 2>&1 || { tail -60 "$src/_build.log"; die "build failed: $src"; }
    cmake --install "$src/_build" >"$src/_install.log" 2>&1 || { tail -20 "$src/_install.log"; die "install failed: $src"; }
}

# --- steps -------------------------------------------------------------------------------------------------
step_mbedtls() {   # ONE SHARED, THREAD-SAFE copy for curl, libgamestream and moonlight-common-c (see the Linux
                   # script: static copies in several .so files interposed inside one TLS handshake, 2026-09-29)
    local abi=$1 d; d="$(unpack mbedtls "$abi")"
    python3 "$d/scripts/config.py" -f "$d/include/mbedtls/mbedtls_config.h" set MBEDTLS_THREADING_C
    python3 "$d/scripts/config.py" -f "$d/include/mbedtls/mbedtls_config.h" set MBEDTLS_THREADING_PTHREAD
    cmake_dep "$abi" "$d" -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DMBEDTLS_FATAL_WARNINGS=OFF \
        -DUSE_SHARED_MBEDTLS_LIBRARY=ON -DUSE_STATIC_MBEDTLS_LIBRARY=OFF
    # Android installs only lib*.so files: an unversioned SONAME is what the loader looks for in the APK.
    for l in mbedtls mbedx509 mbedcrypto; do
        [ -f "$OUT/$abi/lib/lib$l.so" ] || die "mbedTLS did not install lib$l.so"
    done
}
step_curl() {   # HTTP/HTTPS only, TLS by the same mbedTLS; CA roots: Android's own store (/system/etc/security)
    local abi=$1 d; local prefix="$OUT/$abi"; d="$(unpack curl "$abi")"
    patch -d "$d" -p1 --forward --quiet < "$CURL_PATCH"
    cmake_dep "$abi" "$d" -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
        -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF -DHTTP_ONLY=ON \
        -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF \
        -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DCURL_ZLIB=OFF -DCURL_DISABLE_LDAP=ON \
        -DENABLE_ARES=OFF -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=/system/etc/security/cacerts -DCURL_CA_FALLBACK=OFF \
        -DENABLE_UNIX_SOCKETS=ON -DCURL_USE_GSSAPI=OFF -DUSE_LIBRTMP=OFF -DCURL_USE_PKGCONFIG=OFF \
        -DMBEDTLS_INCLUDE_DIR="$prefix/include" -DMBEDTLS_LIBRARY="$prefix/lib/libmbedtls.so" \
        -DMBEDX509_LIBRARY="$prefix/lib/libmbedx509.so" -DMBEDCRYPTO_LIBRARY="$prefix/lib/libmbedcrypto.so"
    # Module mode only: CURLConfig.cmake would pull curl's static-dependency discovery into the app's configure.
    rm -rf "$prefix/lib/cmake/CURL"
}
step_opus() {   # static; its libm need moves into Libs, since consumers link Libs only (Libs.private is ignored)
    local abi=$1 d; d="$(unpack opus "$abi")"
    cmake_dep "$abi" "$d" -DBUILD_SHARED_LIBS=OFF -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF \
        -DOPUS_BUILD_PROGRAMS=OFF
    sed -i 's|^Libs: \(.*-lopus\)$|Libs: \1 -lm|' "$OUT/$abi/lib/pkgconfig/opus.pc"
    grep -q '^Libs: .*-lopus -lm$' "$OUT/$abi/lib/pkgconfig/opus.pc" || die "opus.pc Libs not patched"
}
step_inih() {   # the pin cmake/ExternalINIH.cmake uses, static
    local abi=$1 d; local prefix="$OUT/$abi"; d="$(unpack inih "$abi")"
    cat > "$d/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.13)
project(inih LANGUAGES C)
add_library(inih STATIC ini.c)
install(FILES ini.h TYPE INCLUDE)
install(TARGETS inih ARCHIVE DESTINATION lib)
EOF
    cmake_dep "$abi" "$d"
    mkdir -p "$prefix/lib/pkgconfig"
    printf 'prefix=%s\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\n\nName: inih\nDescription: simple .INI file parser\nVersion: 58\nLibs: -L${libdir} -linih\nCflags: -I${includedir}\n' \
        "$prefix" > "$prefix/lib/pkgconfig/inih.pc"
}
step_expat() {   # static; libgamestream parses the host's XML replies
    local abi=$1 d; d="$(unpack expat "$abi")"
    cmake_dep "$abi" "$d" -DEXPAT_SHARED_LIBS=OFF -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF \
        -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_DOCS=OFF -DEXPAT_BUILD_FUZZERS=OFF -DEXPAT_BUILD_PKGCONFIG=ON
    rm -rf "$OUT/$abi/lib/cmake/expat-"*
}
step_freetype() {   # static, no optional codecs: the UI loads TrueType/OpenType files from /system/fonts
    local abi=$1 d; d="$(unpack freetype "$abi")"
    cmake_dep "$abi" "$d" -DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
    rm -rf "$OUT/$abi/lib/cmake/freetype"
}
step_sdl2() {   # shared: SDLActivity loads libSDL2.so before libmain.so; HIDAPI for USB/BLE controllers
    local abi=$1 d; d="$(unpack sdl2 "$abi")"
    local p
    for p in "${SDL_PATCHES[@]}"; do patch -d "$d" -p1 --forward --quiet < "$p"; done
    cmake_dep "$abi" "$d" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_TESTS=OFF \
        -DSDL_HIDAPI=ON -DSDL_HIDAPI_JOYSTICK=ON -DSDL2_DISABLE_SDL2MAIN=ON -DSDL2_DISABLE_UNINSTALL=ON
    # The Java half of SDL must be the SAME release as libSDL2.so (its native method table is version-bound),
    # with the same patch.
    local java_version; java_version="${SHA[sdl2]} $(cat "${SDL_PATCHES[@]}" | sha256sum | cut -c1-16)"
    if [ ! -d "$OUT/sdl-java/org/libsdl/app" ] || [ "$(cat "$OUT/sdl-java/VERSION" 2>/dev/null)" != "$java_version" ]; then
        rm -rf "$OUT/sdl-java"; mkdir -p "$OUT/sdl-java"
        cp -R "$d/android-project/app/src/main/java/org" "$OUT/sdl-java/"
        echo "$java_version" > "$OUT/sdl-java/VERSION"
        install -m644 "$d/LICENSE.txt" "$OUT/sdl-java/LICENSE.txt"
    fi
}
step_sdl2image() {   # PNG/JPEG via the built-in stb decoder: no libpng/libjpeg to bundle; static into libmain
    local abi=$1 d; d="$(unpack sdl2image "$abi")"
    cmake_dep "$abi" "$d" -DBUILD_SHARED_LIBS=OFF -DSDL2IMAGE_VENDORED=OFF -DSDL2IMAGE_DEPS_SHARED=OFF \
        -DSDL2IMAGE_BACKEND_STB=ON -DSDL2IMAGE_PNG=ON -DSDL2IMAGE_JPG=ON -DSDL2IMAGE_AVIF=OFF -DSDL2IMAGE_JXL=OFF \
        -DSDL2IMAGE_TIF=OFF -DSDL2IMAGE_WEBP=OFF -DSDL2IMAGE_SVG=OFF -DSDL2IMAGE_SAMPLES=OFF -DSDL2IMAGE_TESTS=OFF \
        -DSDL2_DIR="$OUT/$abi/lib/cmake/SDL2"
}

for abi in "${ABIS[@]}"; do
    case "$abi" in arm64-v8a|armeabi-v7a|x86_64|x86) ;; *) die "unknown ABI '$abi'" ;; esac
    mkdir -p "$OUT/$abi"
    for s in "${STEPS[@]}"; do
        stamp="$OUT/$abi/.stamp-$s"
        want="$s ${SHA[$s]} $STAMP_INPUTS"
        if [ "$CLEAN" = 0 ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ]; then
            echo "== $abi $s (current)"
            continue
        fi
        echo "== $abi $s $(date +%T)"
        "step_$s" "$abi"
        echo "$want" > "$stamp"
    done
    rm -rf "$OUT/work/$abi"
done
echo "Android dependencies are ready in $OUT"

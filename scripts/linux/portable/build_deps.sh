#!/usr/bin/env bash
# build_deps.sh [step...] -- build the portable client's pinned dependencies from verified source into the sysroot
# ($SYSROOT$DEPS_PREFIX). Versions match the Windows package (BUILDINFO.txt) so both desktops run the same code.
# Steps (default: all, in order): mbedtls curl opus inih nvcodec sdl2 sdl2image ffmpeg
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/env.sh"
SRC="$ECLIPSE_LINUX_WORK/src"; WORK="$ECLIPSE_LINUX_WORK/deps-build"; mkdir -p "$SRC" "$WORK" "$DEPS_DIR"
JOBS=$(nproc)
declare -A URL SHA
URL[mbedtls]=https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
SHA[mbedtls]=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
URL[curl]=https://curl.se/download/curl-8.22.0.tar.xz
SHA[curl]=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
URL[opus]=https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz
SHA[opus]=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
URL[nvcodec]=https://github.com/FFmpeg/nv-codec-headers/archive/refs/tags/n13.0.19.1.tar.gz
SHA[nvcodec]=c293161bd92dac53f2f8fab08abcd4f96773c3644a57b23c2260b4be2311ab2f
URL[sdl2]=https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz
SHA[sdl2]=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
URL[sdl2image]=https://github.com/libsdl-org/SDL_image/releases/download/release-2.8.12/SDL2_image-2.8.12.tar.gz
SHA[sdl2image]=393f5efb50536ec13ca4f4affb69cc9966d3c3f969e6c5e701faddf9f9785381
URL[inih]=https://github.com/benhoyt/inih/archive/refs/tags/r58.tar.gz
SHA[inih]=e79216260d5dffe809bda840be48ab0eec7737b2bb9f02d2275c1b46344ea7b7
URL[ffmpeg]=https://ffmpeg.org/releases/ffmpeg-9.0.1.tar.xz
SHA[ffmpeg]=cf38e0e28c7e5605942c4a77755349b0145804a397af37eb1fb4c77cb237f635

fetch() {   # $1 = step -> prints the unpacked source dir
    local name="${URL[$1]##*/}"; [ "$1" = nvcodec ] && name=nv-codec-headers-13.0.19.1.tar.gz; [ "$1" = inih ] && name=inih-r58.tar.gz
    [ -f "$SRC/$name" ] || curl -fsSL -o "$SRC/$name" "${URL[$1]}"
    echo "${SHA[$1]}  $SRC/$name" | sha256sum -c --quiet - || { echo "CHECKSUM MISMATCH $name" >&2; exit 1; }
    local dir="$WORK/$1"; rm -rf "$dir"; mkdir -p "$dir"; tar -xf "$SRC/$name" -C "$dir" --strip-components=1
    echo "$dir"
}
cmake_dep() {   # $1 = source dir, rest = cmake options
    local src=$1; shift
    cmake -S "$src" -B "$src/_build" -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN_FILE" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@" >"$src/_configure.log" 2>&1 || { tail -30 "$src/_configure.log"; exit 1; }
    cmake --build "$src/_build" -j "$JOBS" >"$src/_build.log" 2>&1 || { tail -40 "$src/_build.log"; exit 1; }
    DESTDIR="$SYSROOT" cmake --install "$src/_build" >"$src/_install.log" 2>&1 || { tail -20 "$src/_install.log"; exit 1; }
}

step_mbedtls() {
    # ONE SHARED, THREAD-SAFE copy for curl, libgamestream and moonlight-common-c -- as on Windows (one MSYS2
    # DLL). Linked statically, each of the three .so files exported its own copy of the same symbols; ELF
    # interposition then mixed copies inside a single TLS handshake, and the default config is not thread-safe
    # while the app polls several hosts over HTTPS at once. ASan: SEGV in mbedtls_sha512_finish (curl's
    # handshake running libgamestream's + moonlight-common-c's code); release: aborts in glibc's heap checks
    # on quit, before settings_save (2026-09-29).
    local d; d="$(fetch mbedtls)"
    python3 "$d/scripts/config.py" -f "$d/include/mbedtls/mbedtls_config.h" set MBEDTLS_THREADING_C
    python3 "$d/scripts/config.py" -f "$d/include/mbedtls/mbedtls_config.h" set MBEDTLS_THREADING_PTHREAD
    rm -f "$DEPS_DIR"/lib/libmbed{tls,x509,crypto}.a   # the old static archives: nothing may link them again
    cmake_dep "$d" -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF \
        -DUSE_SHARED_MBEDTLS_LIBRARY=ON -DUSE_STATIC_MBEDTLS_LIBRARY=OFF
}
step_curl() {   # HTTP/HTTPS only, TLS by the same mbedTLS; the app supplies its own CA bundle (curl-ca-bundle.crt)
    local d; d="$(fetch curl)"
    # libgamestream hands curl its client key as an in-memory PEM blob; curl 8.22's mbedTLS backend passes that
    # blob's length WITHOUT the terminating NUL mbedtls_pk_parse_key() requires for PEM -> every key rejected.
    patch -d "$d" -p1 --forward --quiet < "$HERE/patches/curl-8.22.0-mbedtls-pem-key-blob.patch"
    cmake_dep "$d" -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
        -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF -DHTTP_ONLY=ON \
        -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF \
        -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DCURL_ZLIB=OFF -DCURL_DISABLE_LDAP=ON \
        -DENABLE_ARES=OFF -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none -DCURL_CA_FALLBACK=OFF -DENABLE_UNIX_SOCKETS=ON \
        -DCURL_USE_GSSAPI=OFF -DUSE_LIBRTMP=OFF -DMBEDTLS_INCLUDE_DIR="$DEPS_DIR/include" \
        -DHAVE_MEMSET_EXPLICIT=0 `# GCC 15 builtin makes the link test pass; glibc 2.35 has no memset_explicit` \
        -DMBEDTLS_LIBRARY="$DEPS_DIR/lib/libmbedtls.so" -DMBEDX509_LIBRARY="$DEPS_DIR/lib/libmbedx509.so" \
        -DMBEDCRYPTO_LIBRARY="$DEPS_DIR/lib/libmbedcrypto.so"
}

step_opus() {   # static; its libm need moves into Libs, since consumers link Libs only (Libs.private is ignored)
    cmake_dep "$(fetch opus)" -DBUILD_SHARED_LIBS=OFF -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF \
        -DOPUS_BUILD_PROGRAMS=OFF
    sed -i 's|^Libs: \(.*-lopus\)$|Libs: \1 -lm|' "$DEPS_DIR/lib/pkgconfig/opus.pc"
    grep -q '^Libs: .*-lopus -lm$' "$DEPS_DIR/lib/pkgconfig/opus.pc" || { echo "opus.pc Libs not patched" >&2; exit 1; }
}
step_nvcodec() {   # header-only; FFmpeg's NVDEC dlopens libcuda/libnvcuvid at run time (no link dependency)
    local d; d="$(fetch nvcodec)"
    make -C "$d" PREFIX="$DEPS_PREFIX" DESTDIR="$SYSROOT" install >"$d/_install.log" 2>&1 || { tail -20 "$d/_install.log"; exit 1; }
}
step_sdl2() {   # every desktop backend is loaded at RUN time (*_SHARED): X11 or Wayland, PipeWire/Pulse/ALSA
    cmake_dep "$(fetch sdl2)" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_TESTS=OFF -DSDL_RPATH=OFF \
        -DSDL_X11=ON -DSDL_X11_SHARED=ON -DSDL_WAYLAND=ON -DSDL_WAYLAND_SHARED=ON -DSDL_WAYLAND_LIBDECOR=ON \
        -DSDL_WAYLAND_LIBDECOR_SHARED=ON -DSDL_WAYLAND_QT_TOUCH=OFF -DSDL_KMSDRM=ON -DSDL_KMSDRM_SHARED=ON \
        -DSDL_OPENGL=ON -DSDL_OPENGLES=ON -DSDL_VULKAN=ON -DSDL_PULSEAUDIO=ON -DSDL_PULSEAUDIO_SHARED=ON \
        -DSDL_PIPEWIRE=ON -DSDL_PIPEWIRE_SHARED=ON -DSDL_ALSA=ON -DSDL_ALSA_SHARED=ON -DSDL_JACK=OFF -DSDL_SNDIO=OFF \
        -DSDL_ESD=OFF -DSDL_ARTS=OFF -DSDL_NAS=OFF -DSDL_OSS=OFF -DSDL_LIBUDEV=ON -DSDL_DBUS=ON -DSDL_IBUS=OFF \
        -DSDL_HIDAPI=ON -DSDL_HIDAPI_LIBUSB=OFF -DSDL_LIBSAMPLERATE=OFF -DSDL_CCACHE=OFF
}
step_sdl2image() {   # PNG/JPEG via the built-in stb decoder: no libpng/libjpeg to bundle or depend on
    cmake_dep "$(fetch sdl2image)" -DBUILD_SHARED_LIBS=ON -DSDL2IMAGE_VENDORED=OFF -DSDL2IMAGE_DEPS_SHARED=OFF \
        -DSDL2IMAGE_BACKEND_STB=ON -DSDL2IMAGE_PNG=ON -DSDL2IMAGE_JPG=ON -DSDL2IMAGE_AVIF=OFF -DSDL2IMAGE_JXL=OFF \
        -DSDL2IMAGE_TIF=OFF -DSDL2IMAGE_WEBP=OFF -DSDL2IMAGE_SAMPLES=OFF -DSDL2IMAGE_TESTS=OFF
}

step_ffmpeg() {   # decode-only, same codecs Windows advertises (H.264 + HEVC); VAAPI (Intel/AMD, links libva) and
                  # NVDEC (NVIDIA, dlopens libcuda at run time); nothing auto-detected so no stray dependencies
    local d; d="$(fetch ffmpeg)"
    ( cd "$d" && ./configure --prefix="$DEPS_PREFIX" --sysroot="$SYSROOT" --target-os=linux --arch=x86_64 \
        --cc=gcc --x86asmexe=nasm --pkg-config=pkg-config \
        --extra-cflags="$SYSROOT_B -ffile-prefix-map=$ECLIPSE_LINUX_WORK=/build" \
        --extra-ldflags="$SYSROOT_LDFLAGS $SYSROOT_B" \
        --enable-shared --disable-static --enable-pic --disable-programs --disable-doc --disable-debug \
        --disable-autodetect --disable-everything --disable-network \
        --disable-avformat --disable-avdevice --disable-avfilter --disable-swresample \
        --enable-avcodec --enable-avutil --enable-swscale \
        --enable-decoder=h264,hevc --enable-parser=h264,hevc \
        --enable-vaapi --enable-libdrm --enable-hwaccel=h264_vaapi,hevc_vaapi \
        --enable-ffnvcodec --enable-nvdec --enable-hwaccel=h264_nvdec,hevc_nvdec \
        >"$d/_configure.log" 2>&1 ) || { tail -30 "$d/_configure.log"; exit 1; }
    # avutil_configuration() & co. return the configure line verbatim, which names this builder's paths
    # (the packaging audit rejects them): record it with the same /build mapping the sources get.
    sed -i "s|$ECLIPSE_LINUX_WORK|/build|g" "$d/config.h"
    grep -q "$ECLIPSE_LINUX_WORK" "$d/config.h" && { echo "config.h still names $ECLIPSE_LINUX_WORK"; exit 1; }
    make -C "$d" -j "$JOBS" >"$d/_build.log" 2>&1 || { tail -40 "$d/_build.log"; exit 1; }
    make -C "$d" install DESTDIR="$SYSROOT" >"$d/_install.log" 2>&1 || { tail -20 "$d/_install.log"; exit 1; }
    grep -E "^(Enabled hwaccels|Enabled decoders|External libraries providing hardware acceleration)" -A2 "$d/_configure.log" || true
}

step_inih() {   # the pin cmake/ExternalINIH.cmake uses; built HERE so the app never falls back to an ExternalProject
                # that would compile outside the sysroot toolchain
    local d; d="$(fetch inih)"
    $CC $CFLAGS -c "$d/ini.c" -o "$d/ini.o" && ar rcs "$DEPS_DIR/lib/libinih.a" "$d/ini.o"
    install -D -m644 "$d/ini.h" "$DEPS_DIR/include/ini.h"
    mkdir -p "$DEPS_DIR/lib/pkgconfig"
    printf 'prefix=%s\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\n\nName: inih\nDescription: simple .INI file parser\nVersion: 58\nLibs: -L${libdir} -linih\nCflags: -I${includedir}\n' \
        "$DEPS_PREFIX" > "$DEPS_DIR/lib/pkgconfig/inih.pc"
}

main() {
    local steps=("$@"); [ ${#steps[@]} -gt 0 ] || steps=(mbedtls curl opus inih nvcodec sdl2 sdl2image ffmpeg)
    for s in "${steps[@]}"; do echo "== $s $(date +%T)"; "step_$s"; echo "   done $(date +%T)"; done
}
main "$@"

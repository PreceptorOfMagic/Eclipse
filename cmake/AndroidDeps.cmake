# Android (NDK) dependency discovery.
#
# The libraries are prebuilt per ABI by scripts/android/build_deps.sh into
#     ${ECLIPSE_ANDROID_DEPS}/<ANDROID_ABI>/{include,lib,lib/pkgconfig}
# Nothing here downloads or builds: the NDK toolchain cannot forward ANDROID_ABI/ANDROID_PLATFORM into the
# ExternalProject fallbacks the desktop builds use (cmake/ExternalINIH.cmake, commons' ExternalOPUS/MbedTLS),
# so every dependency must already be in the prefix for the ordinary find/pkg-config paths to succeed.

if (NOT ECLIPSE_ANDROID_DEPS)
    set(ECLIPSE_ANDROID_DEPS "${CMAKE_SOURCE_DIR}/build/android-deps")
endif ()
set(ECLIPSE_ANDROID_DEPS "${ECLIPSE_ANDROID_DEPS}" CACHE PATH "Root of scripts/android/build_deps.sh output")
set(ECLIPSE_ANDROID_PREFIX "${ECLIPSE_ANDROID_DEPS}/${ANDROID_ABI}")

foreach (_eclipse_android_required lib/libSDL2.so lib/libmbedcrypto.so lib/libcurl.so lib/libopus.a lib/libinih.a
        lib/libexpat.a lib/libfreetype.a lib/libSDL2_image.a)
    if (NOT EXISTS "${ECLIPSE_ANDROID_PREFIX}/${_eclipse_android_required}")
        message(FATAL_ERROR "Android dependency ${_eclipse_android_required} is missing for ${ANDROID_ABI} in "
                "${ECLIPSE_ANDROID_PREFIX}. Run scripts/android/build_deps.sh --abi ${ANDROID_ABI} first.")
    endif ()
endforeach ()
unset(_eclipse_android_required)

# find_path/find_library: the NDK toolchain confines searches to its sysroot (ONLY mode); add the prefix as a root.
list(APPEND CMAKE_FIND_ROOT_PATH "${ECLIPSE_ANDROID_PREFIX}")
list(APPEND CMAKE_PREFIX_PATH "${ECLIPSE_ANDROID_PREFIX}")

# pkg_check_modules: only the prefix's .pc files, never the build host's.
set(ENV{PKG_CONFIG_LIBDIR} "${ECLIPSE_ANDROID_PREFIX}/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")

# pkg-config names libraries (-lSDL2); the existing link lines pass those names through unchanged.
link_directories("${ECLIPSE_ANDROID_PREFIX}/lib")

# Android 15+ devices may run with 16 KB pages; every shared object and the co-op peer executable must be
# aligned for it (NDK r28 does this by default, r27 does not).
add_link_options("LINKER:-z,max-page-size=16384")

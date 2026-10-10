# env.sh -- source me. Build environment for the portable Linux client: every compile and link goes against the
# Ubuntu 22.04 (glibc 2.35) sysroot so the package runs on every current distro, and every dependency we build lands
# INSIDE the sysroot under $DEPS_PREFIX so pkg-config's sysroot prefixing stays uniform. See README.md here.
: "${ECLIPSE_LINUX_WORK:=$HOME/eclipse-linux}"
export ECLIPSE_LINUX_WORK
export SYSROOT="$ECLIPSE_LINUX_WORK/sysroot-jammy"
export DEPS_PREFIX=/opt/eclipse-deps                 # as seen from inside the sysroot
export DEPS_DIR="$SYSROOT$DEPS_PREFIX"               # where it really is
export HOSTTOOLS="$ECLIPSE_LINUX_WORK/hosttools/root"
# Host tools: the builder's own when installed (Debian/Ubuntu: apt install pkgconf nasm patchelf gettext
# libwayland-bin), otherwise a rootless copy unpacked under $HOSTTOOLS, which needs its own libraries.
mkdir -p "$ECLIPSE_LINUX_WORK/bin"
SYSPATH="$(printf '%s' "$PATH" | tr ':' '\n' | grep -vxF "$ECLIPSE_LINUX_WORK/bin" | paste -sd:)"
host_tool() {   # $1 = tool, $2 = how the rootless copy is wired (link|wrap); nothing when the system has it
    rm -f "$ECLIPSE_LINUX_WORK/bin/$1"
    PATH="$SYSPATH" command -v "$1" > /dev/null 2>&1 && return 0
    local real="$HOSTTOOLS/usr/bin/$1"; [ "$1" = pkg-config ] && real="$HOSTTOOLS/usr/bin/pkgconf"
    [ -x "$real" ] || { echo "env.sh: '$1' is missing (install it, or unpack it under $HOSTTOOLS)" >&2; return 0; }
    if [ "$2" = link ]; then ln -s "$real" "$ECLIPSE_LINUX_WORK/bin/$1"; else
        printf '#!/bin/sh\nLD_LIBRARY_PATH="%s" exec "%s" "$@"\n' "$HOSTTOOLS/usr/lib/x86_64-linux-gnu" "$real" > "$ECLIPSE_LINUX_WORK/bin/$1"
        chmod +x "$ECLIPSE_LINUX_WORK/bin/$1"
    fi
}
host_tool pkg-config wrap
for t in nasm wayland-scanner patchelf; do host_tool "$t" link; done
for t in msgfmt xgettext msgmerge; do host_tool "$t" wrap; done
export PATH="$ECLIPSE_LINUX_WORK/bin:$PATH"
export PKG_CONFIG="$(command -v pkg-config)"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$DEPS_DIR/lib/pkgconfig:$DEPS_DIR/share/pkgconfig:$SYSROOT/usr/lib/x86_64-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig:$SYSROOT/usr/lib/pkgconfig"
unset PKG_CONFIG_PATH
# A NATIVE gcc with --sysroot still searches its OWN install-relative lib dir first
# (/usr/lib/gcc/x86_64-linux-gnu/15/../../../x86_64-linux-gnu = the HOST glibc 2.43): headers came from the sysroot
# but crt1.o and libm/libc were the host's, binding e.g. log10f@GLIBC_2.43. -B/-L put the sysroot first.
SYSROOT_LIBDIRS="$SYSROOT/lib/x86_64-linux-gnu $SYSROOT/usr/lib/x86_64-linux-gnu"
SYSROOT_B="-B$SYSROOT/usr/lib/x86_64-linux-gnu/ -B$SYSROOT/lib/x86_64-linux-gnu/"
export CC="gcc --sysroot=$SYSROOT $SYSROOT_B" CXX="g++ --sysroot=$SYSROOT $SYSROOT_B"
# C17, not GCC 15's default C23: C23-era code paths call functions glibc 2.35 does not have (curl uses
# memset_explicit, glibc >= 2.37), and C23 headers of newer glibcs redirect strtol & co to __isoc23_* symbols.
export CFLAGS="-std=gnu17 -O2 -fPIC -ffile-prefix-map=$ECLIPSE_LINUX_WORK=/build"
export CXXFLAGS="-O2 -fPIC -ffile-prefix-map=$ECLIPSE_LINUX_WORK=/build"
# $DEPS_DIR/lib too: some targets link pkg-config names bare (-lSDL2) without its library dir, which only
# works when the library is in a default path -- ours are not.
export SYSROOT_LDFLAGS="-L$DEPS_DIR/lib -L$SYSROOT/lib/x86_64-linux-gnu -L$SYSROOT/usr/lib/x86_64-linux-gnu -Wl,-rpath-link,$SYSROOT/lib/x86_64-linux-gnu:$SYSROOT/usr/lib/x86_64-linux-gnu:$DEPS_DIR/lib"
export LDFLAGS="$SYSROOT_LDFLAGS -Wl,-O1 -Wl,--as-needed"
export CMAKE_TOOLCHAIN_FILE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/toolchain-jammy.cmake"

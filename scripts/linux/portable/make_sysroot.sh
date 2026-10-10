#!/usr/bin/env bash
# make_sysroot.sh -- (re)build the Ubuntu 22.04 "jammy" sysroot every portable Linux binary is linked against:
# glibc 2.35, so the package runs on Ubuntu 22.04, Debian 12, Fedora 36, SteamOS 3.5 and anything newer.
# Rootless and reproducible: exactly the .deb files in sysroot-jammy.lock (SHA-256 checked), fetched by name
# from Ubuntu's archive through Launchpad (which keeps every published build, so a pin never disappears),
# unpacked with dpkg-deb, absolute symlinks made relative so the tree works from any directory.
# Run once per builder, before build_deps.sh. The dependency builds (build_deps.sh) install on top of it.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/env.sh"
LOCK="$HERE/sysroot-jammy.lock"
DEBS="$ECLIPSE_LINUX_WORK/sysroot-debs"
FROM="${ECLIPSE_SYSROOT_MIRROR:-https://launchpad.net/ubuntu/+archive/primary/+files}"
TARGET="${1:-$SYSROOT}"   # optional: build somewhere else (e.g. to compare against the current sysroot)

command -v dpkg-deb > /dev/null || { echo "dpkg-deb is required (package dpkg)" >&2; exit 1; }
mkdir -p "$DEBS"

n=0
while read -r sha name; do
    case "$sha" in ''|'#'*) continue ;; esac
    f="$DEBS/$name"
    if ! { [ -f "$f" ] && echo "$sha  $f" | sha256sum -c --quiet - 2> /dev/null; }; then
        curl -fsSL -o "$f.part" "$FROM/$name" || { echo "download failed: $name" >&2; exit 1; }
        echo "$sha  $f.part" | sha256sum -c --quiet - || { rm -f "$f.part"; echo "HASH MISMATCH: $name" >&2; exit 1; }
        mv -f "$f.part" "$f"
    fi
    n=$((n + 1))
done < "$LOCK"
echo "verified $n packages"

# A fresh tree, keeping the dependencies build_deps.sh installed (they are rebuilt from source anyway).
if [ -d "$TARGET" ]; then
    find "$TARGET" -mindepth 1 -maxdepth 1 ! -name opt -exec rm -rf {} +
fi
mkdir -p "$TARGET"
while read -r sha name; do
    case "$sha" in ''|'#'*) continue ;; esac
    dpkg-deb -x "$DEBS/$name" "$TARGET"
done < "$LOCK"

# Packages ship absolute symlinks (/lib/x86_64-linux-gnu/libfoo.so.1); inside a sysroot they must be relative.
python3 - "$TARGET" << 'EOF'
import os, sys
root = os.path.realpath(sys.argv[1]); fixed = 0
for dirpath, dirnames, filenames in os.walk(root):
    for name in dirnames + filenames:
        p = os.path.join(dirpath, name)
        if os.path.islink(p):
            t = os.readlink(p)
            if t.startswith('/'):
                os.unlink(p)
                os.symlink(os.path.relpath(root + t, dirpath), p)
                fixed += 1
print(f'relative symlinks: {fixed}')
EOF
[ -x "$TARGET/lib64/ld-linux-x86-64.so.2" ] || { echo "sysroot has no working loader" >&2; exit 1; }
echo "sysroot ready: $TARGET ($(grep -o 'libc6_[^_]*' "$LOCK"))"

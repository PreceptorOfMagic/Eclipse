#!/bin/sh
# Install for this user without root, services, autostart, or system configuration changes.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)
. "$HERE/package-functions.sh"
[ "$#" = 0 ] || package_fail 'Usage: ./install.sh'
package_paths
package_check "$HERE"
DEST="$DATA/eclipse/app"
[ ! -L "$DATA/eclipse" ] || package_fail "Refusing symbolic-link application parent: $DATA/eclipse"
mkdir -p "$DATA/eclipse"
[ ! -L "$DEST" ] || package_fail "Refusing to replace a symbolic link: $DEST"
if [ "$HERE" = "$DEST" ]; then
    "$HERE/install-desktop-entry.sh"
    exit 0
fi
# Verify before altering an existing install; includes all bundled libraries and install scripts.
(cd "$HERE" && sha256sum -c --quiet SHA256SUMS) || package_fail 'Package checksum verification failed.'
if [ -e "$DEST" ]; then
    package_check "$DEST"
    package_stop "$DEST"
fi
stage=$(mktemp -d "$DATA/eclipse/.install.XXXXXX")
backup=
committed=0
cleanup() {
    rm -rf -- "$stage"
    if [ -n "$backup" ] && [ -e "$backup" ]; then
        if [ "$committed" = 0 ]; then
            [ ! -e "$DEST" ] || rm -rf -- "$DEST"
            mv -- "$backup" "$DEST"
        else
            rm -rf -- "$backup"
        fi
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM
cp -a "$HERE/." "$stage/"
(cd "$stage" && sha256sum -c --quiet SHA256SUMS) || package_fail 'Installed files failed verification.'
# Preserve the previous version until the new application and menu entry are installed.
if [ -e "$DEST" ]; then
    backup=$(mktemp -d "$DATA/eclipse/.previous.XXXXXX")
    rmdir -- "$backup"
    mv -- "$DEST" "$backup"
fi
mv -- "$stage" "$DEST"
"$DEST/install-desktop-entry.sh"
committed=1
printf 'Installed Eclipse. Open it from your applications menu.\nUninstall: "%s/uninstall.sh"\n' "$DEST"

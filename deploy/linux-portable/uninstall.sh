#!/bin/sh
# Remove this package, its menu entry and (by default) this user's Eclipse data.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)
. "$HERE/package-functions.sh"
package_paths
package_check "$HERE"
KEEP=0
case "${1:-}" in
    '') [ "$#" = 0 ] || package_fail 'Unexpected arguments.' ;;
    --keep-settings) [ "$#" = 1 ] || package_fail 'Unexpected arguments.'; KEEP=1 ;;
    *) package_fail 'Usage: ./uninstall.sh [--keep-settings]' ;;
esac
# Guard automatic self-removal even if the script was copied into a user/system directory.
for base in "$profile_home" "$DATA" "$STATE" "$CACHE" / /usr /usr/local /opt; do
    [ "$HERE" != "$base" ] || package_fail "Refusing to remove top-level directory $HERE"
done
# The leaf may be removed as a link, but an intermediate symlink would redirect a purge.
if [ "$KEEP" = 0 ] && [ -L "$DATA/com.limelight" ]; then
    package_fail "Refusing symbolic-link settings parent: $DATA/com.limelight"
fi
package_stop "$HERE"
"$HERE/install-desktop-entry.sh" --remove
if [ "$KEEP" = 0 ]; then
    rm -rf -- "$DATA/com.limelight/moonlight-tv" "$STATE/eclipse" "$CACHE/moonlight-tv"
    rmdir -- "$DATA/com.limelight" 2>/dev/null || true
fi
rm -rf -- "$HERE"
rmdir -- "$DATA/eclipse" 2>/dev/null || true
printf 'Eclipse was uninstalled.\n'
[ "$KEEP" = 0 ] || printf 'Your settings, pairing keys and logs were kept.\n'

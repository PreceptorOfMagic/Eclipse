#!/bin/sh
# Add Eclipse to your applications menu -- for your user only, no administrator rights needed.
#   ./install-desktop-entry.sh           add (or refresh) the menu entry for THIS folder
#   ./install-desktop-entry.sh --remove  take it out again
# The entry starts ./eclipse from wherever this folder is now: run it again after moving the folder.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)
. "$HERE/package-functions.sh"
package_paths
APPS="$DATA/applications"
ICONS="$DATA/icons/hicolor"
SIZES="16 24 32 48 64 128 256"

refresh() {
    command -v update-desktop-database > /dev/null 2>&1 && update-desktop-database -q "$APPS" 2> /dev/null || true
}

case "${1:-}" in ''|--remove) ;; *) package_fail 'Usage: ./install-desktop-entry.sh [--remove]' ;; esac
[ "$#" -le 1 ] || package_fail 'Unexpected arguments.'
# Refuse line breaks in the desktop entry, even when the portable folder is outside XDG directories.
case "$HERE" in *"
"*|*"$(printf '\r')"*) package_fail 'The package path cannot contain line breaks.' ;; esac
owner=$(printf '%s' "$HERE" | sed -e 's/\\/\\\\/g')

if [ "${1:-}" = --remove ]; then
    # Another portable copy may own the menu entry. Do not remove its launcher or icons.
    if [ -f "$APPS/eclipse.desktop" ] && ! grep -qxF "X-Eclipse-Package=$owner" "$APPS/eclipse.desktop"; then
        echo 'The menu entry belongs to another Eclipse folder; it was kept.'
        exit 0
    fi
    rm -f "$APPS/eclipse.desktop"
    for s in $SIZES; do rm -f "$ICONS/${s}x${s}/apps/eclipse.png"; done
    refresh
    echo 'Eclipse was removed from the applications menu.'
    exit 0
fi

[ -x "$HERE/eclipse" ] || { echo "Run this from the unpacked Eclipse folder (./eclipse not found)." >&2; exit 1; }

for s in $SIZES; do
    mkdir -p "$ICONS/${s}x${s}/apps"
    cp -f "$HERE/share/icons/hicolor/${s}x${s}/apps/eclipse.png" "$ICONS/${s}x${s}/apps/eclipse.png"
done

# Desktop Entry spec: inside a quoted Exec argument, \ " ` $ take a backslash; then the general string
# escape doubles every backslash; and a literal % must be written %%.
escape_exec() { sed -e 's/[\\"`$]/\\&/g' -e 's/\\/\\\\/g' -e 's/%/%%/g'; }
exec_path=$(printf '%s' "$HERE/eclipse" | escape_exec)
shell_arg=$(printf '%s' 'exec "$1"' | escape_exec)

mkdir -p "$APPS"
cat > "$APPS/eclipse.desktop" << EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=Eclipse
GenericName=Game Streaming Client
Comment=Play games streamed from your PCs
# A fixed executable lets GLib validate entries whose package path contains a literal %.
# The package path is an argument, never shell source; exec replaces the temporary shell.
Exec=/bin/sh -c "$shell_arg" eclipse "$exec_path"
Icon=eclipse
Terminal=false
Categories=Game;Network;RemoteAccess;
Keywords=moonlight;sunshine;apollo;stream;gamestream;
StartupWMClass=eclipse
X-Eclipse-Package=$owner
EOF
refresh
echo "Eclipse is now in your applications menu (entry: $APPS/eclipse.desktop)."

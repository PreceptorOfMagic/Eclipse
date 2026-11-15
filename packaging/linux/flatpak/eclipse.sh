#!/bin/sh
# Eclipse inside the Flatpak sandbox. The runtime supplies every system library, so of the tarball launcher
# (deploy/linux-portable/eclipse) only what the client itself needs remains.

# Settings and pairings: the same folder as the tarball build (~/.local/share/com.limelight/moonlight-tv,
# granted in finish-args). SDL_GetPrefPath() finds it through XDG_DATA_HOME, which Flatpak points at the
# sandbox's own data folder.
export XDG_DATA_HOME="${HOST_XDG_DATA_HOME:-$HOME/.local/share}"
export TEXTDOMAINDIR="${TEXTDOMAINDIR:-/app/share/locale}"
# The window joins its menu entry (StartupWMClass) on X11 and Wayland.
export SDL_VIDEO_X11_WMCLASS="${SDL_VIDEO_X11_WMCLASS:-io.github.PreceptorOfMagic.Eclipse}"
export SDL_VIDEO_WAYLAND_WMCLASS="${SDL_VIDEO_WAYLAND_WMCLASS:-io.github.PreceptorOfMagic.Eclipse}"
# Started from the menu there is no terminal: keep the log, as the tarball does.
if [ ! -t 2 ]; then
    LOGDIR="${XDG_STATE_HOME:-$HOME/.local/state}/eclipse"
    if mkdir -p "$LOGDIR" 2> /dev/null; then
        [ -f "$LOGDIR/eclipse.log" ] && mv -f "$LOGDIR/eclipse.log" "$LOGDIR/eclipse.log.1"
        exec > "$LOGDIR/eclipse.log" 2>&1
    fi
fi
# KDE PLASMA, co-op with this PC as one of the two PCs: KWin lets an app use its virtual outputs, output layout and
# fake pointer only when the app's desktop file lists them (X-KDE-Wayland-Interfaces, in the desktop file shipped
# here). Flatpak 1.18.4 and later export desktop files through an allowlist of keys that drops this one, so then a
# copy of the exported file that keeps it goes into the user's applications folder, as Umbra for Linux does. Done at
# start, so KDE's service cache has caught up by the time a co-op session asks; nothing happens on other desktops.
kde_entry() {
    id="${FLATPAK_ID:-io.github.PreceptorOfMagic.Eclipse}"
    key='X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1,kde_output_management_v2,kde_output_device_registry_v2,kde_output_device_v2,org_kde_kwin_fake_input'
    dest="$XDG_DATA_HOME/applications/$id.desktop"
    src=
    for dir in "$XDG_DATA_HOME/flatpak/exports/share/applications" /var/lib/flatpak/exports/share/applications; do
        if [ -r "$dir/$id.desktop" ]; then src="$dir/$id.desktop"; break; fi
    done
    [ -n "$src" ] || return 0
    if grep -qx "$key" "$src"; then
        # Flatpak exports the key: no copy needed, and an old one of ours goes
        if grep -qx "$key" "$dest" 2> /dev/null; then rm -f "$dest"; fi
        return 0
    fi
    mkdir -p "$XDG_DATA_HOME/applications" || return 0
    tmp="$dest.tmp.$$"
    sed "/^X-KDE-Wayland-Interfaces=/d; /^\[Desktop Entry\]\$/a $key" "$src" > "$tmp" && grep -qx "$key" "$tmp" &&
        { cmp -s "$tmp" "$dest" || mv -f "$tmp" "$dest"; }
    rm -f "$tmp"
}
case "${XDG_CURRENT_DESKTOP:-}" in *KDE*) kde_entry ;; esac

exec /app/bin/moonlight-tv "$@"

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
exec /app/bin/moonlight-tv "$@"

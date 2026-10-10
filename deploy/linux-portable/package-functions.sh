# Shared by the per-user installer and uninstaller (POSIX shell).
package_fail() { printf 'Eclipse: %s\n' "$*" >&2; exit 1; }
package_paths() {
    [ -n "${HOME:-}" ] || package_fail 'HOME is not set.'
    DATA=${XDG_DATA_HOME:-$HOME/.local/share}
    STATE=${XDG_STATE_HOME:-$HOME/.local/state}
    CACHE=${XDG_CACHE_HOME:-$HOME/.cache}
    for base in "$HOME" "$DATA" "$STATE" "$CACHE"; do
        case "$base" in /*) ;; *) package_fail 'HOME and XDG directories must be absolute paths.' ;; esac
        [ "$base" != / ] || package_fail 'A user data directory cannot be the filesystem root.'
        case "$base" in *"
"*|*"$(printf '\r')"*) package_fail 'Directory names cannot contain line breaks.' ;; esac
    done
    # Resolve symlinked XDG roots so executable identity checks also work on relocated profiles.
    DATA=$(readlink -m -- "$DATA")
    STATE=$(readlink -m -- "$STATE")
    CACHE=$(readlink -m -- "$CACHE")
    profile_home=$(readlink -m -- "$HOME")
    for base in "$profile_home" "$DATA" "$STATE" "$CACHE"; do
        [ "$base" != / ] || package_fail 'A user data directory cannot resolve to the filesystem root.'
    done
}
package_check() {
    [ -f "$1/.eclipse-package" ] && [ "$(cat "$1/.eclipse-package")" = eclipse-linux-v1 ] &&
        [ -x "$1/bin/moonlight-tv" ] && [ -f "$1/SHA256SUMS" ] &&
        [ -x "$1/eclipse" ] && [ -x "$1/uninstall.sh" ] ||
        package_fail "Not a complete Eclipse package: $1"
}
# Match the executable's full path and uid, never a process name or command-line substring.
# Recheck before each signal so an unrelated application is never selected by its name.
package_pids() {
    for proc in /proc/[0-9]*; do
        exe=$(readlink "$proc/exe" 2>/dev/null) || continue
        case "$exe" in "$1/bin/moonlight-tv"|"$1/bin/moonlight-tv (deleted)")
            [ "$(stat -c %u "$proc" 2>/dev/null)" = "$(id -u)" ] || continue
            printf '%s\n' "${proc##*/}" ;;
        esac
    done
}
package_stop() {
    for pid in $(package_pids "$1"); do kill -TERM "$pid" 2>/dev/null || true; done
    attempt=0
    while [ -n "$(package_pids "$1")" ] && [ "$attempt" -lt 10 ]; do
        sleep 1
        attempt=$((attempt + 1))
    done
    for pid in $(package_pids "$1"); do kill -KILL "$pid" 2>/dev/null || true; done
    attempt=0
    while [ -n "$(package_pids "$1")" ] && [ "$attempt" -lt 3 ]; do
        sleep 1
        attempt=$((attempt + 1))
    done
    [ -z "$(package_pids "$1")" ] || package_fail 'Eclipse could not be stopped; no files were removed.'
}

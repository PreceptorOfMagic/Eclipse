#!/usr/bin/env bash
# Read-only profile discovery shared by TV operations. Never create a directory
# while resolving: doing so could hide an existing native-owned profile.
_tv_existing_conf() {
    _tv_selected=
    for _tv_candidate in "$1/conf" "$1/userdata/conf" "$2/conf"; do
        # Reject ambiguous links rather than following them into another profile.
        if [ -L "$_tv_candidate" ] || [ -L "${_tv_candidate%/*}" ]; then
            echo "TV profile: refusing symlink $_tv_candidate" >&2
            return 2
        fi
        # An inaccessible parent can hide an existing profile. Refuse to guess
        # that it is absent and redirect the operation into the external copy.
        if [ -d "${_tv_candidate%/*}" ] && [ ! -x "${_tv_candidate%/*}" ]; then
            echo "TV profile: cannot inspect parent ${_tv_candidate%/*}" >&2
            return 2
        fi
        if [ -d "$_tv_candidate" ]; then
            if [ -n "$_tv_selected" ]; then
                echo "TV profile: ambiguous profiles $_tv_selected and $_tv_candidate; native access must be checked" >&2
                return 2
            fi
            _tv_selected=$_tv_candidate
        fi
    done
    if [ -n "$_tv_selected" ]; then
        printf '%s\n' "$_tv_selected"
        return 0
    fi
    echo "TV profile: no existing profile for $1; launch the app first" >&2
    return 1
}

# resolve_tv_conf APP_ID SSH_COMMAND [SSH_ARGUMENTS...]
# SSH runs as prisoner, whose access differs from the native app UID. Do not use
# prisoner -w to choose another profile. Multiple existing locations are ambiguous
# without native-UID access: refuse rather than edit a possibly inactive profile.
# Report any read/write denial at the caller.
resolve_tv_conf() {
    local app_id="${1:-}"
    case "$app_id" in
        ''|.|..|*[!a-zA-Z0-9._-]*) echo 'TV profile: invalid app ID' >&2; return 2 ;;
    esac
    shift
    [ "$#" -gt 0 ] || { echo 'TV profile: missing SSH command' >&2; return 2; }
    {
        declare -f _tv_existing_conf
        printf '_tv_existing_conf "%s" "%s"\n' \
            "/media/developer/apps/usr/palm/applications/$app_id" \
            "/media/developer/temp/$app_id"
    } | "$@" 'sh -s'
}

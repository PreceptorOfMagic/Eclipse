#!/usr/bin/env bash
# Shared host resolution for Eclipse dev/ops scripts.
#
# WHY: every pinned 192.168.x.y in a script is a thing that breaks when DHCP
# shuffles, when a cable moves, or when a second host (PC2) enters the picture.
# These helpers resolve at RUN TIME, preferring the same broadcast discovery the
# Pi hub already uses, so nothing needs editing when the LAN changes.
#
# Resolution order is always: explicit env var -> live discovery -> cache -> fail
# with an actionable message. A successful lookup refreshes the cache, so a later
# run still works if discovery is briefly unavailable.
#
# Usage:  source "$(dirname "$0")/../lib/net_env.sh"   (adjust depth)
#         TV=$(resolve_tv)  HOST=$(resolve_stream_host)  PI=$(resolve_pi)

CTM_AGENT_PORT="${CTM_AGENT_PORT:-48054}"
TV_SSH_PORT="${TV_SSH_PORT:-9922}"
_AURORA_CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/aurora"

_cache_get() { [ -r "$_AURORA_CACHE/$1" ] && cat "$_AURORA_CACHE/$1"; }
_cache_put() { mkdir -p "$_AURORA_CACHE" && printf '%s\n' "$2" > "$_AURORA_CACHE/$1"; }

_port_open() {  # host port  -- returns 0 if reachable
    timeout 2 bash -c "</dev/tcp/$1/$2" 2>/dev/null
}

# --- streaming host (SKYNET today, PC2 tomorrow) ----------------------------
# Broadcasts CTM_DISCOVER_V1; the ctm-usbip agent on each host replies
# CTM_AGENT_V1 with "session=on|off". A host that is actively streaming wins, so
# this is correct with two interchangeable hosts, not just shuffle-proof.
ctm_discover() {   # prints "ip session=on|off" per responder
    python3 - "$CTM_AGENT_PORT" 2>/dev/null <<'PY'
import socket, sys, time
port = int(sys.argv[1])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
s.settimeout(0.35)
try:
    s.sendto(b"CTM_DISCOVER_V1", ("255.255.255.255", port))
except OSError:
    sys.exit(0)
seen = {}
deadline = time.time() + 0.6
while time.time() < deadline:
    try:
        d, a = s.recvfrom(256)
    except socket.timeout:
        break
    if d.startswith(b"CTM_AGENT_V1"):
        txt = d.decode(errors="replace")
        seen[a[0]] = "on" if "session=on" in txt else "off"
for ip, sess in seen.items():
    print(f"{ip} session={sess}")
PY
}

# WSL2 is NAT'd onto its own subnet, so a 255.255.255.255 broadcast from here never
# reaches the LAN and ctm_discover always comes back empty. But when we ARE inside
# WSL, the streaming host is the Windows machine hosting us - ask it directly.
# Follow the DEFAULT ROUTE to find the real LAN adapter. Picking "the first IPv4
# address" instead returns the WSL vEthernet adapter (172.21.x.x), which is not on
# the LAN at all.
_win_host_ip() {
    command -v powershell.exe >/dev/null 2>&1 || return 1
    powershell.exe -NoProfile -Command \
        "\$i=(Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue | Sort-Object RouteMetric | Select-Object -First 1).InterfaceIndex; if(\$i){(Get-NetIPAddress -AddressFamily IPv4 -InterfaceIndex \$i -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty IPAddress)}" \
        2>/dev/null | tr -d '\r\n '
}

resolve_stream_host() {
    if [ -n "${XLINK_HOST:-}" ]; then printf '%s\n' "$XLINK_HOST"; return 0; fi

    local found active any
    found="$(ctm_discover)"
    active="$(printf '%s\n' "$found" | awk '/session=on/  {print $1; exit}')"
    any="$(   printf '%s\n' "$found" | awk 'NF {print $1; exit}')"
    for ip in "$active" "$any"; do
        if [ -n "$ip" ]; then _cache_put stream_host "$ip"; printf '%s\n' "$ip"; return 0; fi
    done

    local win; win="$(_win_host_ip)"
    if [ -n "$win" ]; then _cache_put stream_host "$win"; printf '%s\n' "$win"; return 0; fi

    local cached; cached="$(_cache_get stream_host)"
    if [ -n "$cached" ]; then
        echo "net_env: discovery found no agent; using cached $cached" >&2
        printf '%s\n' "$cached"; return 0
    fi
    echo "net_env: no streaming host found. Is ctm-usbip running? Override with XLINK_HOST=<ip>" >&2
    return 1
}

# --- TV ---------------------------------------------------------------------
# ares device profile first (the user already keeps it current for deploys),
# then cache, then a bounded scan of the local /24 for the dev-SSH port. The
# scan means a DHCP move fixes itself instead of needing every script edited.
_tv_from_ares() {
    command -v ares-setup-device >/dev/null 2>&1 || return 1
    ares-setup-device -l 2>/dev/null |
        awk '/prisoner@/ { if (match($0, /[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+/))
                              { print substr($0, RSTART, RLENGTH); exit } }'
}

# The LAN /24 the devices live on. Inside WSL our own routes point at WSL's NAT
# subnet (172.x), NOT the LAN, so ask Windows first when interop is available.
_lan_prefix() {
    local ip
    ip="$(_win_host_ip)"
    if [ -z "$ip" ]; then
        ip="$(ip -4 route get 1.1.1.1 2>/dev/null |
              awk '{for(i=1;i<=NF;i++) if($i=="src"){print $(i+1); exit}}')"
    fi
    [ -n "$ip" ] || return 1
    printf '%s\n' "${ip%.*}"
}

_tv_scan() {
    local prefix; prefix="$(_lan_prefix)"; [ -n "$prefix" ] || return 1
    echo "net_env: scanning ${prefix}.0/24 for the TV on port ${TV_SSH_PORT}..." >&2
    local i
    for i in $(seq 1 254); do
        ( _port_open "${prefix}.${i}" "$TV_SSH_PORT" && echo "${prefix}.${i}" ) &
    done 2>/dev/null | head -1
    wait 2>/dev/null
}

resolve_tv() {
    local ip
    if [ -n "${TV_HOST:-}" ]; then printf '%s\n' "$TV_HOST"; return 0; fi

    ip="$(_tv_from_ares)"
    if [ -n "$ip" ] && _port_open "$ip" "$TV_SSH_PORT"; then
        _cache_put tv "$ip"; printf '%s\n' "$ip"; return 0
    fi

    ip="$(_cache_get tv)"
    if [ -n "$ip" ] && _port_open "$ip" "$TV_SSH_PORT"; then printf '%s\n' "$ip"; return 0; fi

    ip="$(_tv_scan)"
    if [ -n "$ip" ]; then
        echo "net_env: TV found at $ip - consider: ares-setup-device -m LivingRoomMoonlight -i \"host=$ip\"" >&2
        _cache_put tv "$ip"; printf '%s\n' "$ip"; return 0
    fi
    echo "net_env: TV not found. Is it on? Override with TV_HOST=<ip>" >&2
    return 1
}

# --- Pi hub -----------------------------------------------------------------
# Scan the /24 for SSH, then CONFIRM by asking each candidate its hostname over
# the existing key auth - so we can never latch onto some other machine's :22.
_pi_scan() {
    local prefix; prefix="$(_lan_prefix)"; [ -n "$prefix" ] || return 1
    echo "net_env: scanning ${prefix}.0/24 for the Pi..." >&2
    local candidates i
    candidates="$(for i in $(seq 1 254); do
        ( _port_open "${prefix}.${i}" 22 && echo "${prefix}.${i}" ) &
    done 2>/dev/null | sort -u; wait 2>/dev/null)"

    local want; want="$(printf '%s' "${PI_HOSTNAME:-networkusb}" | tr 'A-Z' 'a-z')"
    local ip name
    for ip in $candidates; do
        name="$(timeout 4 ssh -o BatchMode=yes -o ConnectTimeout=2 -o StrictHostKeyChecking=no \
                    "${PI_USER:-cameron}@${ip}" hostname 2>/dev/null | tr -d '\r' | tr 'A-Z' 'a-z')"
        if [ -n "$name" ] && [ "$name" = "$want" ]; then printf '%s\n' "$ip"; return 0; fi
    done
    return 1
}

resolve_pi() {
    local ip
    if [ -n "${PI_HOST:-}" ]; then printf '%s\n' "$PI_HOST"; return 0; fi
    for name in "${PI_HOSTNAME:-networkusb}" "${PI_HOSTNAME:-networkusb}.local"; do
        ip="$(getent hosts "$name" 2>/dev/null | awk '{print $1; exit}')"
        if [ -n "$ip" ] && _port_open "$ip" 22; then _cache_put pi "$ip"; printf '%s\n' "$ip"; return 0; fi
    done
    ip="$(_cache_get pi)"
    if [ -n "$ip" ] && _port_open "$ip" 22; then printf '%s\n' "$ip"; return 0; fi

    ip="$(_pi_scan)"
    if [ -n "$ip" ]; then _cache_put pi "$ip"; printf '%s\n' "$ip"; return 0; fi
    echo "net_env: Pi not found. Override with PI_HOST=<ip>" >&2
    return 1
}

resolve_pi_ssh() {   # user@ip for scp/ssh
    local ip; ip="$(resolve_pi)" || return 1
    printf '%s@%s\n' "${PI_USER:-cameron}" "$ip"
}

# --- the PEER streaming host (PC2 today) ------------------------------------
# Co-op needs BOTH hosts, and the second one cannot be found the same way as the first:
# `ctm_discover` only hears from machines running the ctm-usbip agent, which PC2 does not,
# and `_win_host_ip` by definition returns the machine hosting this WSL instance. What PC2
# does have is an Umbra/Apollo-compatible host, so the peer is "a LAN host with the host HTTP
# port open that is not us". That is stable across a re-cable, which a pinned address is not -- and a stale
# pin here is silent: scripts carry on and address a machine that is not there.
_peer_scan() {
    local prefix; prefix="$(_lan_prefix)"; [ -n "$prefix" ] || return 1
    local me; me="$(_win_host_ip 2>/dev/null)"
    echo "net_env: scanning ${prefix}.0/24 for the peer host (Umbra/Apollo-compatible on 47989)..." >&2
    local found i
    found="$(for i in $(seq 1 254); do
        ( _port_open "${prefix}.${i}" 47989 && echo "${prefix}.${i}" ) &
    done 2>/dev/null | sort -u; wait 2>/dev/null)"
    local ip
    for ip in $found; do
        [ "$ip" = "$me" ] && continue
        printf '%s\n' "$ip"; return 0
    done
    return 1
}

resolve_peer_host() {
    local ip
    if [ -n "${PC2_IP:-}" ]; then printf '%s\n' "$PC2_IP"; return 0; fi
    ip="$(_cache_get peer)"
    if [ -n "$ip" ] && _port_open "$ip" 47989; then printf '%s\n' "$ip"; return 0; fi
    ip="$(_peer_scan)"
    if [ -n "$ip" ]; then _cache_put peer "$ip"; printf '%s\n' "$ip"; return 0; fi
    # Fall back to the cached address even when it is not answering, so a caller that only
    # needs an ssh target still works while the host happens to be stopped -- but say so,
    # because "the peer is off" and "the peer moved" need different responses.
    ip="$(_cache_get peer)"
    if [ -n "$ip" ]; then
        echo "net_env: peer not answering on 47989; using last known $ip (Umbra host stopped?)" >&2
        printf '%s\n' "$ip"; return 0
    fi
    echo "net_env: peer host not found. Override with PC2_IP=<ip> (or PC2_HOST=user@ip)" >&2
    return 1
}

resolve_peer_ssh() {   # user@ip for scp/ssh
    local ip; ip="$(resolve_peer_host)" || return 1
    printf '%s@%s\n' "${PC2_USER:?set PC2_USER to the user name on the second PC}" "$ip"
}

# Profile lookup shares app precedence but performs no network I/O until called.
. "$(dirname "${BASH_SOURCE[0]}")/tv_profile.sh"

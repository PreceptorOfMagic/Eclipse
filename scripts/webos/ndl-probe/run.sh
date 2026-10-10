#!/usr/bin/env bash
# Build ndl_probe.c, install it as a throwaway app (com.aurora.ndlprobe), run ONE case on
# the TV and print its log. It never touches Eclipse's install or conf.
#
#   scripts/webos/ndl-probe/run.sh                          # Eclipse's shape (default case)
#   scripts/webos/ndl-probe/run.sh "h265 pcm sdl" 25        # <args> [settle-seconds]
#   scripts/webos/ndl-probe/run.sh --remove                 # uninstall the probe app
#
# Needs the webOS SDK (scripts/webos/build_for_lg.sh downloads it to /tmp) and ares-cli.
# Case flags are documented at the top of ndl_probe.c. docs/webos26-media-load.md says
# what the 11.2.0 results were and how to read the log.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../../.." && pwd)"
. "${ROOT}/scripts/lib/net_env.sh"
export PATH="$HOME/.local/bin:$PATH"
SDK="${WEBOS_SDK_DIR:-/tmp/arm-webos-linux-gnueabi_sdk-buildroot}"
APP_ID=com.aurora.ndlprobe
DEVICE="${ARES_DEVICE:-tv}"
TV="$(resolve_tv)" || { echo "ndl-probe: cannot resolve the TV"; exit 1; }
TVSSH=(ssh -p 9922 -i "$HOME/.ssh/aurora_tv" -o HostKeyAlgorithms=+ssh-rsa
       -o PubkeyAcceptedAlgorithms=+ssh-rsa -o StrictHostKeyChecking=no -o BatchMode=yes
       "prisoner@${TV}")

if [ "${1:-}" = "--remove" ]; then
    ares-install --remove "${APP_ID}" -d "${DEVICE}"
    exit 0
fi
ARGS="${1:-h265 pcm sdl,render,earlyinit,noinit}"
SETTLE="${2:-25}"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
mkdir -p "${WORK}/app/bin"
"${SDK}/bin/arm-webos-linux-gnueabi-gcc" -O1 -g -Wall -Wno-deprecated-declarations -Wno-unused-result \
    -rdynamic -I"${SDK}/arm-webos-linux-gnueabi/sysroot/usr/include/libndl-media" \
    -o "${WORK}/app/bin/ndlprobe" "${HERE}/ndl_probe.c" -lNDL_directmedia -lpthread -ldl
cp "${ROOT}/deploy/webos/icon.png" "${WORK}/app/icon.png"
cat > "${WORK}/app/appinfo.json" <<EOF
{"id": "${APP_ID}", "version": "0.0.1", "vendor": "Eclipse diagnostics", "type": "native",
 "main": "bin/ndlprobe", "title": "NDL probe", "icon": "icon.png"}
EOF
ares-package "${WORK}/app" -o "${WORK}" >/dev/null
ares-install "${WORK}/${APP_ID}_0.0.1_arm.ipk" -d "${DEVICE}" >/dev/null
# The probe truncates its own log on start; the args file is ours.
"${TVSSH[@]}" "echo '${ARGS}' > /tmp/ndlprobe.args" 2>/dev/null
ares-launch "${APP_ID}" -d "${DEVICE}" >/dev/null
echo "== ndl-probe: '${ARGS}' on ${TV}; settling ${SETTLE}s"
sleep "${SETTLE}"
"${TVSSH[@]}" 'cat /tmp/ndlprobe.log' 2>/dev/null
ares-launch --close "${APP_ID}" -d "${DEVICE}" >/dev/null 2>&1 || true

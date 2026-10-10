#!/usr/bin/env bash
# coop_smoke.sh — unattended co-op spike runs. No one has to be at the TV.
#
# Eclipse already accepts webOS launch params (app_webos.c: app_handle_launch),
# so a stream can be started by launching the app with a host uuid + app id
# instead of a human navigating the UI:
#   ares-launch com.aurora.gamestream -d tv -p '{"host_uuid":"...","host_app_id":N}'
#
# Subcommands:
#   stream [app]       start a stream unattended (app: desktop|bigpicture|vdisplay)
#   probe-video [app]  run the split-plane spike end to end and print the result
#   probe-audio [app]  run the 2nd-PCM-out spike end to end and print the result
#   ndl-2proc          two-PROCESS NDL pipeline test (no stream, no TV UI needed)
#   stop               close Eclipse on the TV and clear all probe sentinels
#
# Probe lines are matched by the run id they carry; /tmp/aurora-diag.log on the
# TV cannot be deleted by the prisoner account, so filtering by a reference
# epoch taken before the run is the only reliable way to attribute output.
set -euo pipefail

TV_DEVICE="${TV_DEVICE:-tv}"
TV_APP="${TV_APP:-com.aurora.gamestream}"
HOST_UUID="${HOST_UUID:-CBA83FB7-192B-2C0A-0F10-2487C7D92B85}"
SETTLE="${SETTLE:-25}"          # seconds to let the stream come up + probe fire
PROBE_BIN="ndl_pipeline_probe"

case "${2:-desktop}" in
    desktop)    APP_ID=881448767 ;;
    bigpicture) APP_ID=1093255277 ;;
    vdisplay)   APP_ID=382300562 ;;
    *)          APP_ID="${2}" ;;
esac

SSH_OPTS="-p 9922 -i $HOME/.ssh/aurora_tv -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o StrictHostKeyChecking=no"
SCP_OPTS="-P 9922 -i $HOME/.ssh/aurora_tv -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o StrictHostKeyChecking=no"

resolve_tv() {
    # ares knows the current IP; the TV moves on DHCP (was .21, now .25).
    ares-setup-device --list 2>/dev/null \
        | awk -v d="$TV_DEVICE" '$1==d {print $2}' \
        | sed 's/.*@//; s/:.*//' | head -1
}
TV_HOST="${TV_HOST:-$(resolve_tv)}"
[ -n "$TV_HOST" ] || { echo "!! could not resolve the TV address" >&2; exit 1; }
tvssh() { ssh $SSH_OPTS "prisoner@${TV_HOST}" "$@" 2>/dev/null; }

wait_for_tv() {
    echo "--- waiting for TV at ${TV_HOST}:9922 (it drops ICMP; probing the port) ---"
    for _ in $(seq 1 60); do
        if timeout 3 bash -c "cat < /dev/null > /dev/tcp/${TV_HOST}/9922" 2>/dev/null; then
            echo "    TV is up"; return 0
        fi
        sleep 5
    done
    echo "!! TV never came up — it is powered off. This is the one step that needs a human." >&2
    return 1
}

start_stream() {
    echo "--- launching Eclipse with launch params (host_app_id=${APP_ID}) ---"
    ares-launch --close "$TV_APP" -d "$TV_DEVICE" >/dev/null 2>&1 || true
    sleep 3
    ares-launch "$TV_APP" -d "$TV_DEVICE" \
        -p "{\"host_uuid\":\"${HOST_UUID}\",\"host_app_id\":${APP_ID}}" 2>&1 | tail -1
}

collect() {  # $1 = reference epoch
    echo "--- probe output for this run (run >= $1) ---"
    tvssh "grep -a 'COOP-PROBE' /tmp/aurora-diag.log 2>/dev/null | tail -40" \
        | awk -v ref="$1" '
            /run=/ { if (match($0, /run=[0-9]+/)) {
                        id = substr($0, RSTART+4, RLENGTH-4) + 0
                        if (id >= ref) print
                     }
                     next }
            { untagged = untagged $0 "\n" }
            END { if (untagged != "") printf "\n(untagged lines — cannot be attributed to this run:)\n%s", untagged }'
}

disarm() { tvssh "rm -f /tmp/coop-probe /tmp/coop-probe-audio" || true; }

run_probe() {  # $1 = sentinel to arm
    wait_for_tv
    disarm
    REF=$(tvssh 'date +%s')
    echo "--- reference epoch ${REF}; arming $1 ---"
    tvssh "touch $1"
    start_stream
    echo "--- letting the stream run ${SETTLE}s ---"
    sleep "$SETTLE"
    collect "$REF"
    disarm
    echo "--- disarmed ---"
}

case "${1:-}" in
    stream)      wait_for_tv; start_stream ;;
    probe-video) run_probe /tmp/coop-probe ;;
    probe-audio) run_probe /tmp/coop-probe-audio ;;
    ndl-2proc)
        wait_for_tv
        echo "--- two-PROCESS NDL pipeline test (no stream involved) ---"
        scp $SCP_OPTS "$(dirname "$0")/../../tests/coop/${PROBE_BIN}" \
            "prisoner@${TV_HOST}:/tmp/${PROBE_BIN}" >/dev/null 2>&1 \
            || { echo "!! build tests/coop/${PROBE_BIN} first (see its header)" >&2; exit 1; }
        tvssh "chmod +x /tmp/${PROBE_BIN}
            echo '=== BASELINE: one process alone ==='
            APPID=${TV_APP} /tmp/${PROBE_BIN} 0
            echo
            echo '=== A holds the pipeline, B tries (SAME appid) ==='
            APPID=${TV_APP} /tmp/${PROBE_BIN} 12 > /tmp/probe_a.log 2>&1 &
            sleep 4
            APPID=${TV_APP} /tmp/${PROBE_BIN} 0
            echo '--- holder A said: ---'; sleep 10; cat /tmp/probe_a.log
            echo
            echo '=== A holds, B tries (DIFFERENT appid) ==='
            APPID=${TV_APP} /tmp/${PROBE_BIN} 12 > /tmp/probe_a2.log 2>&1 &
            sleep 4
            APPID=com.aurora.gamestream.coop /tmp/${PROBE_BIN} 0
            echo '--- holder A said: ---'; sleep 10; cat /tmp/probe_a2.log"
        ;;
    stop)
        disarm
        ares-launch --close "$TV_APP" -d "$TV_DEVICE" 2>&1 | tail -1 || true
        echo "closed + disarmed"
        ;;
    *)
        sed -n '2,20p' "$0"
        exit 1 ;;
esac

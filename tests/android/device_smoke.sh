#!/usr/bin/env bash
# device_smoke.sh -- the Android port's smoke tests on a device over adb: the emulator job of
# .github/workflows/build-android.yml, or by hand against a SHIELD (adb connect <ip>:5555, ANDROID_SERIAL).
#
#   device_smoke.sh <eclipse-android-selftest.apk> <hevc_tilecols_cli> <work dir> [--uhd]
#
# The self-test APK (scripts/android/build_apk.sh --selftest) is debuggable, so the test streams are copied
# into the app's own files with run-as, and has its own application ID: it installs beside a real Eclipse
# without touching its paired hosts or settings (uninstall it afterwards with adb uninstall <its ID>). Every case launches the app as the launcher does and checks:
#   launcher   the UI comes up and survives d-pad input (screenshots launcher.png, launcher-nav.png)
#   co-op      the co-op peer executable runs from the installed package (exits 1 on its unreachable socket),
#              and, spawned by a real coordinator as a co-op stream spawns it, starts up without a Java VM and
#              reports back over the co-op control channel
#   video      composite.265 (a co-op composite), solo.264, left.265 (a narrow, letterboxed picture) and, with
#              --uhd, uhd.265 (4K Main10) play through SS4S and the MediaCodec module onto the video surface
#              under the UI: every access unit accepted, at least 90% of the pictures decoded, no codec error
# and that nothing crashed. Logs and screenshots are left in <work dir>; the exit status is the verdict.
set -euo pipefail
APK=$1 TILECOLS=$2 W=$3 UHD_ARG=()
[ "${4:-}" = --uhd ] && UHD_ARG=(--uhd)
PKG=io.github.preceptorofmagic.eclipse.selftest   # beside, never over, a real installation
ACTIVITY=$PKG/io.github.preceptorofmagic.eclipse.EclipseActivity
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$W"
FAILURES=0
fail() { echo "FAIL: $*"; FAILURES=$((FAILURES + 1)); }
pass() { echo "PASS: $*"; }

"$HERE/make_fixtures.sh" "$TILECOLS" "$W/fixtures" "${UHD_ARG[@]}"

adb wait-for-device
echo "device: $(adb shell getprop ro.product.model | tr -d '\r'), Android $(adb shell getprop ro.build.version.release | tr -d '\r') (API $(adb shell getprop ro.build.version.sdk | tr -d '\r')), $(adb shell getprop ro.product.cpu.abi | tr -d '\r')"
adb install -r -t -g "$APK" >/dev/null
DATA="$(adb shell run-as "$PKG" pwd | tr -d '\r')"
[ -n "$DATA" ] || { echo "run-as $PKG failed: is this the debuggable self-test APK?"; exit 1; }
adb shell run-as "$PKG" mkdir -p files/selftest
for f in "$W"/fixtures/*.265 "$W"/fixtures/*.264; do
    name="$(basename "$f")"
    adb push "$f" "/data/local/tmp/eclipse-$name" >/dev/null
    adb shell "cat /data/local/tmp/eclipse-$name | run-as $PKG sh -c 'cat > files/selftest/$name'"
    adb shell rm -f "/data/local/tmp/eclipse-$name"
done

crashed() {   # logcat file: a Java exception in the app, or a native crash of either process (a process that
              # ends normally, as the app does after a self-test, is not one)
    grep -qE "FATAL EXCEPTION|Fatal signal [0-9]+|>>> $PKG <<<" "$1"
}

# One launch of a self-test: waits for "SELFTEST done", screenshots the picture mid-way. Fails (status 1) if
# the app crashed or never finished: its results then mean nothing.
run_selftest() {   # case name, then am start extras
    local name=$1
    shift
    adb logcat -c
    adb shell am start -W -S -n "$ACTIVITY" "$@" >/dev/null
    local start=$SECONDS shot=0 log="$W/$name.logcat.txt"
    while [ $((SECONDS - start)) -lt 180 ]; do
        sleep 1
        adb logcat -d -v threadtime >"$log"
        if [ "$shot" = 0 ] && [ $((SECONDS - start)) -ge 5 ]; then
            adb exec-out screencap -p >"$W/$name.png" || true
            shot=1
        fi
        grep -q "SELFTEST done" "$log" && break
        crashed "$log" && break
    done
    sleep 2
    adb logcat -d -v threadtime >"$log"
    if crashed "$log"; then
        fail "$name: the app crashed (see $log)"
        return 1
    fi
    if ! grep -q "SELFTEST done" "$log"; then
        fail "$name: the self-test did not finish in time (see $log)"
        return 1
    fi
}

check_video() {   # case name, stream
    local name=$1 stream=$2 log="$W/$1.logcat.txt" line closed
    line="$(grep -o "SELFTEST video .*" "$log" | tail -1 || true)"
    if [ -z "$line" ]; then
        fail "$name: no video result"
        return
    fi
    echo "  $line"
    closed="$(grep -o "Closed: fed [0-9]*, rendered [0-9]*, older pictures skipped [0-9]*, input lost [0-9]*, errors [0-9]*" "$log" | tail -1 || true)"
    echo "  ${closed:-MediaCodec module statistics missing}"
    case "$line" in *result=PASS*) ;; *) fail "$name: $stream was not accepted"; return ;; esac
    if [ -z "$closed" ]; then
        fail "$name: the MediaCodec module did not report its statistics"
        return
    fi
    local fed rendered skipped errors
    read -r fed rendered skipped errors < <(awk -F'[^0-9]+' '{print $2, $3, $4, $6}' <<<"$closed")
    if [ "$errors" -ne 0 ] || [ $((rendered + skipped)) -lt $((fed * 9 / 10)) ] || [ "$rendered" -eq 0 ]; then
        fail "$name: $stream decoded $((rendered + skipped)) of $fed pictures with $errors codec errors"
    else
        pass "$name: $stream, $fed pictures fed, $rendered shown, $skipped superseded before display"
    fi
}

# --- the launcher ---------------------------------------------------------------------------------------
adb logcat -c
adb shell am start -W -S -n "$ACTIVITY" >/dev/null
sleep 10
adb exec-out screencap -p >"$W/launcher.png" || true
for key in DPAD_DOWN DPAD_RIGHT DPAD_RIGHT DPAD_LEFT DPAD_UP; do
    adb shell input keyevent "KEYCODE_$key"
    sleep 0.5
done
sleep 2
adb exec-out screencap -p >"$W/launcher-nav.png" || true
adb logcat -d -v threadtime >"$W/launcher.logcat.txt"
if crashed "$W/launcher.logcat.txt"; then
    fail "launcher: the app crashed (see $W/launcher.logcat.txt)"
elif [ -z "$(adb shell pidof "$PKG" | tr -d '\r')" ]; then
    fail "launcher: the app is not running after start-up and d-pad input"
else
    pass "launcher: up, and still running after d-pad input"
fi
grep -o "Decoder [a-z0-9]*: .*\|Display HDR10: .*" "$W/launcher.logcat.txt" | sort -u | sed 's/^/  /' || true
adb shell am force-stop "$PKG"

# --- co-op peer and the co-op composite -----------------------------------------------------------------
if run_selftest coop --es selftest_video "$DATA/files/selftest/composite.265" --es selftest_codec hevc \
    --es selftest_size 1280x720 --ez selftest_peer true; then
    peer="$(grep -o "SELFTEST peer .*" "$W/coop.logcat.txt" | tail -1 || true)"
    echo "  ${peer:-no peer result}"
    case "$peer" in *result=PASS*) pass "co-op: the peer executable runs from the package" ;; *) fail "co-op: peer" ;; esac
    handshake="$(grep -o "SELFTEST coop-handshake .*" "$W/coop.logcat.txt" | tail -1 || true)"
    echo "  ${handshake:-no co-op handshake result}"
    case "$handshake" in
        *result=PASS*) pass "co-op: the peer starts up without Java and answers the coordinator" ;;
        *) fail "co-op: start-up handshake (the peer's own lines are in $W/coop.logcat.txt)" ;;
    esac
    check_video coop composite.265
fi

# --- single streams -------------------------------------------------------------------------------------
run_selftest h264 --es selftest_video "$DATA/files/selftest/solo.264" --es selftest_codec h264 \
    --es selftest_size 1280x720 && check_video h264 solo.264
run_selftest narrow --es selftest_video "$DATA/files/selftest/left.265" --es selftest_codec hevc \
    --es selftest_size 640x720 && check_video narrow left.265
if [ ${#UHD_ARG[@]} -gt 0 ]; then
    run_selftest uhd --es selftest_video "$DATA/files/selftest/uhd.265" --es selftest_codec hevc \
        --es selftest_size 3840x2160 && check_video uhd uhd.265
fi

adb shell am force-stop "$PKG"
echo
if [ "$FAILURES" -gt 0 ]; then
    echo "device smoke tests: $FAILURES FAILED"
    exit 1
fi
echo "device smoke tests: all passed"

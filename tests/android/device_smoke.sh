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
#   keys       no keyboard-with-a-d-pad (a TV remote) is a joystick to SDL; on an emulator, a remote is plugged
#              in (tests/android/test_remote_uinput.c) and its Up and Back arrive as SDL's Up and AC Back keys --
#              not Escape, not joystick buttons -- which the remote's Back (tap/hold) and OK (hold: right click)
#              depend on
#   display    the TV's refresh rates, and the display mode a 60 FPS stream asks for taking effect
#   audio      the outputs Settings -> Audio offers, by name, and the first of them opened as a stream opens a
#              chosen output: by its current number, on SDL's AAudio driver
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

# Each case reads only the log written since it began. adb logcat -c alone is not enough: on the API 30
# emulator it left the previous case's lines in place, and its "SELFTEST done" ended the next case's wait
# after a second, before that case had run (2026-10-15). -T takes the device's clock, whole seconds; the
# cases are at least two seconds apart.
log_mark() {
    adb logcat -c || true
    LOG_SINCE="$(adb shell date +%s | tr -d '\r').000"
}
log_dump() {
    adb logcat -d -v threadtime -T "$LOG_SINCE"
}

crashed() {   # logcat file: a Java exception in the app, or a native crash of either process (a process that
              # ends normally, as the app does after a self-test, is not one)
    grep -qE "FATAL EXCEPTION|Fatal signal [0-9]+|>>> $PKG <<<" "$1"
}

# One launch of a self-test: waits for "SELFTEST done", screenshots the picture mid-way. Fails (status 1) if
# the app crashed or never finished: its results then mean nothing.
run_selftest() {   # case name, then am start extras
    local name=$1
    shift
    log_mark
    adb shell am start -W -S -n "$ACTIVITY" "$@" >/dev/null
    local start=$SECONDS shot=0 log="$W/$name.logcat.txt"
    while [ $((SECONDS - start)) -lt 180 ]; do
        sleep 1
        log_dump >"$log"
        if [ "$shot" = 0 ] && [ $((SECONDS - start)) -ge 5 ]; then
            adb exec-out screencap -p >"$W/$name.png" || true
            shot=1
        fi
        grep -q "SELFTEST done" "$log" && break
        crashed "$log" && break
    done
    sleep 2
    log_dump >"$log"
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
log_mark
adb shell am start -W -S -n "$ACTIVITY" >/dev/null
sleep 10
adb exec-out screencap -p >"$W/launcher.png" || true
for key in DPAD_DOWN DPAD_RIGHT DPAD_RIGHT DPAD_LEFT DPAD_UP; do
    adb shell input keyevent "KEYCODE_$key"
    sleep 0.5
done
sleep 2
adb exec-out screencap -p >"$W/launcher-nav.png" || true
log_dump >"$W/launcher.logcat.txt"
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

# --- remote keys ----------------------------------------------------------------------------------------
# On an emulator, a TV remote is plugged in for this case: tests/android/test_remote_uinput.c, from the
# self-test APK, run as adb's shell user (adb emu event send reached no input device on emulator 37.2).
EMULATOR="$(adb shell getprop ro.kernel.qemu | tr -d '\r')"
REMOTE=/data/local/tmp/eclipse-test-remote
start_test_remote() {
    local abi
    abi="$(adb shell getprop ro.product.cpu.abi | tr -d '\r')"
    python3 -c 'import sys, zipfile; sys.stdout.buffer.write(zipfile.ZipFile(sys.argv[1]).read(sys.argv[2]))' \
        "$APK" "lib/$abi/libeclipse_test_remote.so" >"$W/eclipse-test-remote" || return 1
    adb push "$W/eclipse-test-remote" "$REMOTE" >/dev/null && adb shell chmod 755 "$REMOTE" || return 1
    adb shell "$REMOTE" 90 >"$W/test-remote.txt" 2>&1 &
    remote_pid=$!
    local start=$SECONDS
    while [ $((SECONDS - start)) -lt 15 ]; do
        adb shell dumpsys input | grep -q "Eclipse test remote" && return 0
        sleep 1
    done
    return 1
}
stop_test_remote() {
    adb shell 'kill -TERM $(pidof eclipse-test-remote)' >/dev/null 2>&1 || true
    if [ -n "${remote_pid:-}" ]; then
        wait "$remote_pid" 2>/dev/null || true
    fi
    adb shell rm -f "$REMOTE" >/dev/null 2>&1 || true
}
send_emulator_keys() {   # once the self-test listens: Up and Back from the plugged-in remote
    local start=$SECONDS
    while [ $((SECONDS - start)) -lt 30 ]; do
        log_dump | grep -q "SELFTEST keys ready" && break
        sleep 1
    done
    sleep 1
    adb shell 'kill -USR1 $(pidof eclipse-test-remote)' >/dev/null   # (toybox pkill reads -USR1 as -U SR1)
}
if [ "$EMULATOR" = 1 ]; then
    if start_test_remote; then
        trap stop_test_remote EXIT
        send_emulator_keys &
        sender=$!
    else
        fail "keys: the test remote could not be plugged in (see $W/test-remote.txt)"
    fi
fi
if run_selftest keys --ez selftest_keys true; then
    grep -o "SELFTEST input-device .*" "$W/keys.logcat.txt" | sed 's/^/  /' || true
    devices="$(grep -o "SELFTEST input-devices .*" "$W/keys.logcat.txt" | tail -1 || true)"
    echo "  ${devices:-no input device result}"
    case "$devices" in
        *result=PASS*) pass "keys: no TV-remote-like device is a joystick to SDL" ;;
        *) fail "keys: a keyboard with a d-pad is a joystick to SDL (see $W/keys.logcat.txt)" ;;
    esac
    if [ "$EMULATOR" = 1 ] && ! grep -q 'SELFTEST input-device name="Eclipse test remote"' "$W/keys.logcat.txt"; then
        fail "keys: the self-test did not see the test remote among the input devices"
    fi
    if [ "$EMULATOR" = 1 ]; then
        grep -o "SELFTEST key .*" "$W/keys.logcat.txt" | sed 's/^/  /' || true
        if grep -q "SELFTEST key down scancode=.*(82)" "$W/keys.logcat.txt" &&
           grep -q "SELFTEST key down scancode=.*(270)" "$W/keys.logcat.txt" &&
           ! grep -q "SELFTEST key \(joystick\|controller\)-button" "$W/keys.logcat.txt"; then
            pass "keys: a d-pad device's Up and Back arrive as SDL's Up and AC Back keys"
        else
            fail "keys: Up/Back from a d-pad device did not arrive as Up and AC Back keys"
        fi
    else
        echo "  (key routing is checked on the emulator; on this device press Back and OK in a stream)"
    fi
fi
if [ -n "${sender:-}" ]; then
    wait "$sender" 2>/dev/null || true
fi
if [ "$EMULATOR" = 1 ]; then
    stop_test_remote
    trap - EXIT
fi

# --- display mode and audio outputs ---------------------------------------------------------------------
if run_selftest platform --ez selftest_display true --ez selftest_audio true; then
    display="$(grep -o "SELFTEST display .*" "$W/platform.logcat.txt" | tail -1 || true)"
    echo "  ${display:-no display result}"
    case "$display" in
        *result=PASS*) pass "display: the TV's refresh rates read, and the mode for a 60 FPS stream taken" ;;
        *) fail "display: refresh rates or mode change (see $W/platform.logcat.txt)" ;;
    esac
    grep -o "SELFTEST audio-outputs .*" "$W/platform.logcat.txt" | tail -1 | sed 's/^/  /' || true
    audio="$(grep -o "SELFTEST audio output=.*" "$W/platform.logcat.txt" | tail -1 || true)"
    echo "  ${audio:-no audio result}"
    case "$audio" in
        *result=PASS*) pass "audio: outputs listed by name, and one opened by its current number on AAudio" ;;
        *) fail "audio: output list or open (see $W/platform.logcat.txt)" ;;
    esac
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

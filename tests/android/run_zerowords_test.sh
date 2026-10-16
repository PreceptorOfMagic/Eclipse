#!/usr/bin/env bash
# Co-op composites with cabac_zero_words before the second slice, as the hosts' padding lands in them, run through
# coop_au_strip_zero_words() as the Android build feeds them. The SHIELD TV's decoder does not decode a slice that
# follows such padding (2026-10-16); removing it must leave every picture exactly as it was.
#   run_zerowords_test.sh <hevc_tilecols_cli> <hevc_zerowords_cli> <work dir>
# Exits 77 (skipped) without an FFmpeg that has libx265, or without python3.
set -euo pipefail
TILECOLS=$1 ZW=$2 W=$3
command -v ffmpeg >/dev/null || { echo "ffmpeg not found: skipped"; exit 77; }
command -v python3 >/dev/null || { echo "python3 not found: skipped"; exit 77; }
encoders="$(ffmpeg -hide_banner -encoders 2>/dev/null)"
grep -q libx265 <<<"$encoders" || { echo "no libx265: skipped"; exit 77; }
mkdir -p "$W"
X265="ctu=32:bframes=0:ref=1:slices=1:keyint=30:min-keyint=30:scenecut=0:repeat-headers=1:info=0:wpp=0:pmode=0"
X265="$X265:pme=0:frame-threads=1:no-open-gop=1:weightp=0:log-level=error"
ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=640x352:rate=60 -t 1 -pix_fmt yuv420p \
    -c:v libx265 -x265-params "$X265" -bsf:v hevc_mp4toannexb -f hevc "$W/a.265"
ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i mandelbrot=size=640x352:rate=60 -t 1 -pix_fmt yuv420p \
    -c:v libx265 -x265-params "$X265" -bsf:v hevc_mp4toannexb -f hevc "$W/b.265"

md5() { ffmpeg -nostdin -hide_banner -loglevel error -i "$1" -f framemd5 - | grep -v '^#' | awk -F, '{print $NF}' | md5sum; }
# Slices followed by another slice of the same picture that still end in 00 00 03.
padded_before_slice() {
    python3 - "$1" <<'EOF'
import re, sys
d = open(sys.argv[1], 'rb').read()
starts = [m.end() for m in re.finditer(b'\x00\x00\x01', d)]
nals = [d[s:(starts[i + 1] - 3 if i + 1 < len(starts) else len(d))].rstrip(b'\x00') for i, s in enumerate(starts)]
bad = 0
for i, n in enumerate(nals[:-1]):
    nxt = nals[i + 1]
    if ((n[0] >> 1) & 0x3f) < 32 and ((nxt[0] >> 1) & 0x3f) < 32 and not (nxt[2] & 0x80) and n.endswith(b'\x00\x00\x03'):
        bad += 1
print(bad)
EOF
}

status=0
for layout in rows cols; do
    flag=""; [ "$layout" = rows ] && flag="--rows"
    "$TILECOLS" $flag "$W/a.265" "$W/b.265" "$W/$layout.265" >/dev/null
    want="$(md5 "$W/$layout.265")"
    "$ZW" add 300 "$W/$layout.265" "$W/$layout-padded.265" >/dev/null
    [ "$(padded_before_slice "$W/$layout-padded.265")" -gt 0 ] || { echo "FAIL: $layout: the fixture has no padding"; status=1; }
    [ "$(md5 "$W/$layout-padded.265")" = "$want" ] || { echo "FAIL: $layout: the padded fixture decodes differently"; status=1; }
    if out="$("$ZW" strip "$W/$layout-padded.265" "$W/$layout-stripped.265")"; then echo "  $layout: $out"
    else echo "FAIL: $layout: the padding was not removed ($out)"; status=1; fi
    if ffmpeg -nostdin -hide_banner -loglevel error -err_detect explode -xerror -i "$W/$layout-stripped.265" -f null - &&
       [ "$(md5 "$W/$layout-stripped.265")" = "$want" ]; then
        echo "  $layout: stripped composite decodes cleanly to the same pictures"
    else
        echo "FAIL: $layout: the stripped composite decodes differently"; status=1
    fi
    left="$(padded_before_slice "$W/$layout-stripped.265")"
    if [ "$left" = 0 ]; then echo "  $layout: no slice is followed by padding any more"
    else echo "FAIL: $layout: $left slices still end in padding before another slice"; status=1; fi
    # A composite without padding is left alone: fed as it is.
    if "$ZW" strip "$W/$layout.265" "$W/$layout-same.265" >/dev/null; then
        echo "FAIL: $layout: a composite without padding was rewritten"; status=1
    elif cmp -s "$W/$layout.265" "$W/$layout-same.265"; then
        echo "  $layout: a composite without padding is fed unchanged"
    else
        echo "FAIL: $layout: a composite without padding changed"; status=1
    fi
done
exit $status

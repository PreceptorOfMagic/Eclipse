#!/usr/bin/env bash
# Horizontal co-op's composite (two tile ROWS, one column) re-emitted untiled by coop_au_untile(), as the Android
# build feeds it: the SHIELD TV's Tegra decoder renders a second tile row as green garbage, but decodes the same
# picture without tiles. With one tile column, tile scan is raster scan, so the untiled stream must decode to
# exactly the same pictures. Checked with FFmpeg, frame by frame; and a tile-COLUMN composite must be refused.
#   run_untile_test.sh <hevc_tilecols_cli> <hevc_untile_cli> <work dir>
# Exits 77 (skipped) without an FFmpeg that has libx265.
set -euo pipefail
TILECOLS=$1 UNTILE=$2 W=$3
command -v ffmpeg >/dev/null || { echo "ffmpeg not found: skipped"; exit 77; }
encoders="$(ffmpeg -nostdin -hide_banner -encoders 2>/dev/null)"
grep -q libx265 <<<"$encoders" || { echo "no libx265: skipped"; exit 77; }
mkdir -p "$W"
# Panes shaped like NVENC's (as make_fixtures.sh), 11 CTB rows each: tile rows split on a CTB row.
X265="ctu=32:bframes=0:ref=1:slices=1:keyint=30:min-keyint=30:scenecut=0:repeat-headers=1:info=0:wpp=0:pmode=0"
X265="$X265:pme=0:frame-threads=1:no-open-gop=1:weightp=0:log-level=error"
ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=640x352:rate=60 -t 1 -pix_fmt yuv420p \
    -c:v libx265 -x265-params "$X265" -bsf:v hevc_mp4toannexb -f hevc "$W/top.265"
ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i mandelbrot=size=640x352:rate=60 -t 1 -pix_fmt yuv420p \
    -c:v libx265 -x265-params "$X265" -bsf:v hevc_mp4toannexb -f hevc "$W/bottom.265"
"$TILECOLS" --rows "$W/top.265" "$W/bottom.265" "$W/rows.265" | tail -1
"$TILECOLS" "$W/top.265" "$W/bottom.265" "$W/cols.265" | tail -1

status=0
frames() { ffprobe -hide_banner -loglevel error -count_frames -show_entries stream=nb_read_frames -of csv=p=0 "$1"; }
tiles() { ffmpeg -nostdin -hide_banner -i "$1" -c copy -bsf:v trace_headers -f null - 2>&1 | grep -cE "tiles_enabled_flag +[01]+ = 1" || true; }

"$UNTILE" "$W/rows.265" "$W/untiled.265" || { echo "FAIL: the tile-rows composite was not untiled whole"; status=1; }
want="$(frames "$W/rows.265")"
[ "$want" -gt 0 ] || { echo "FAIL: the tile-rows composite has no pictures"; exit 1; }
if ffmpeg -nostdin -hide_banner -loglevel error -err_detect explode -xerror -i "$W/untiled.265" -f null - &&
   [ "$(frames "$W/untiled.265")" = "$want" ]; then
    echo "  untiled stream decodes cleanly, all $want pictures"
else
    echo "FAIL: the untiled stream does not decode cleanly"; status=1
fi
[ "$(tiles "$W/rows.265")" -gt 0 ] || { echo "FAIL: the composite has no tiles to remove"; status=1; }
if [ "$(tiles "$W/untiled.265")" = 0 ]; then echo "  untiled stream has tiles off"
else echo "FAIL: the untiled stream still has tiles on"; status=1; fi
ffmpeg -nostdin -hide_banner -loglevel error -i "$W/rows.265" -f framemd5 "$W/rows.md5"
ffmpeg -nostdin -hide_banner -loglevel error -i "$W/untiled.265" -f framemd5 "$W/untiled.md5"
if diff -q <(grep -v '^#' "$W/rows.md5") <(grep -v '^#' "$W/untiled.md5") >/dev/null; then
    echo "  every picture identical to the tiled composite's ($want)"
else
    echo "FAIL: the untiled pictures differ from the tiled composite's"; status=1
fi

# Two tile COLUMNS are not raster order: must be refused, never rewritten into a wrong picture.
if "$UNTILE" "$W/cols.265" "$W/cols-untiled.265" >/dev/null; then
    echo "FAIL: a tile-column composite was untiled"; status=1
elif cmp -s "$W/cols.265" "$W/cols-untiled.265"; then
    echo "  tile-column composite refused, passed through unchanged"
else
    echo "FAIL: a refused tile-column composite was changed"; status=1
fi
exit $status

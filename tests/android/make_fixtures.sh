#!/usr/bin/env bash
# The Android port's test streams, shaped like what a host sends, made with FFmpeg (libx264, libx265):
#   left.265, right.265   640x720 HEVC panes as NVENC sends them for co-op
#   composite.265         1280x720, the two panes spliced by the shipping co-op code (hevc_tilecols_cli)
#   solo.264              1280x720 H.264
#   uhd.265               3840x2160 HEVC Main10 (only with --uhd: for real hardware, too heavy for an emulator)
#   make_fixtures.sh <hevc_tilecols_cli> <out dir> [--uhd]
# Exits 77 without an FFmpeg that has libx264 and libx265.
set -euo pipefail
TILECOLS=$1 W=$2 UHD=0
[ "${3:-}" = --uhd ] && UHD=1
command -v ffmpeg >/dev/null || { echo "ffmpeg not found: skipped"; exit 77; }
# Held in a variable: under pipefail, grep -q closing the pipe early would fail ffmpeg with SIGPIPE.
encoders="$(ffmpeg -hide_banner -encoders 2>/dev/null)"
grep -q libx265 <<<"$encoders" || { echo "no libx265: skipped"; exit 77; }
grep -q libx264 <<<"$encoders" || { echo "no libx264: skipped"; exit 77; }
mkdir -p "$W"
# Panes shaped like NVENC's: one slice, CTB 32, no B-frames, one reference, no weighted prediction.
X265="ctu=32:bframes=0:ref=1:slices=1:keyint=60:min-keyint=60:scenecut=0:repeat-headers=1:info=0:wpp=0:pmode=0"
X265="$X265:pme=0:frame-threads=1:no-open-gop=1:weightp=0:log-level=error"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=640x720:rate=60 -t 2 -pix_fmt yuv420p \
    -c:v libx265 -x265-params "$X265" -bsf:v hevc_mp4toannexb -f hevc "$W/left.265"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i mandelbrot=size=640x720:rate=60 -t 2 -pix_fmt yuv420p \
    -c:v libx265 -x265-params "$X265" -bsf:v hevc_mp4toannexb -f hevc "$W/right.265"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=60 -t 2 -pix_fmt yuv420p \
    -c:v libx264 -g 60 -bf 0 -x264-params repeat-headers=1 -bsf:v h264_mp4toannexb -f h264 "$W/solo.264"
"$TILECOLS" "$W/left.265" "$W/right.265" "$W/composite.265" | tail -1
if [ "$UHD" = 1 ]; then
    ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=3840x2160:rate=60 -t 2 -pix_fmt yuv420p10le \
        -c:v libx265 -profile:v main10 -x265-params "$X265:preset=ultrafast" -bsf:v hevc_mp4toannexb -f hevc "$W/uhd.265"
fi

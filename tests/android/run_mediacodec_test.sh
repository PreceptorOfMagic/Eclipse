#!/usr/bin/env bash
# The MediaCodec module's feeding against real H.264 and H.265 streams -- including a co-op composite built by
# the shipping splice code (tests/coop/hevc_tilecols_cli.c) -- and the result decoded by FFmpeg in strict mode.
#   run_mediacodec_test.sh <test_mediacodec_feed> <hevc_tilecols_cli> <work dir>
# Exits 77 (skipped) without an FFmpeg that has libx264 and libx265.
set -euo pipefail
FEED=$1 TILECOLS=$2 W=$3
"$(dirname "$0")/make_fixtures.sh" "$TILECOLS" "$W"
status=0
for spec in "composite.265 hevc" "left.265 hevc" "solo.264 h264"; do
    read -r stream codec <<<"$spec"
    DUMP="$W/$stream.fed" "$FEED" "$W/$stream" "$codec" || status=1
    frames="$(ffprobe -hide_banner -loglevel error -count_frames -show_entries stream=nb_read_frames -of csv=p=0 "$W/$stream")"
    if ffmpeg -hide_banner -loglevel error -err_detect explode -xerror -i "$W/$stream.fed" -f null - &&
       [ "$(ffprobe -hide_banner -loglevel error -count_frames -show_entries stream=nb_read_frames -of csv=p=0 "$W/$stream.fed")" = "$frames" ]; then
        echo "  $stream: what the decoder receives decodes cleanly, all $frames pictures"
    else
        echo "  $stream: what the decoder receives does NOT decode cleanly"
        status=1
    fi
done
exit $status

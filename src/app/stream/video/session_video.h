#pragma once

#include <Limelight.h>
#include <stdbool.h>

extern struct VIDEO_STATS vdec_summary_stats;
extern struct VIDEO_INFO vdec_stream_info;
extern struct AUDIO_INFO audio_stream_info;

extern DECODER_RENDERER_CALLBACKS ss4s_dec_callbacks;

/** Call before LiStartConnection. Sets decoder capabilities from settings (RFI + slices for HEVC/AV1 when enabled). */
void session_video_prepare_stream(void);


/**
 * Has the co-op composite emitted at least one picture?
 *
 * USER_STREAM_OPEN means the STREAM opened, not that a picture is on screen. In co-op the
 * horizontal path suppresses its own pictures until it can splice, so closing the loading
 * dialog on stream-open leaves the panel blank with nothing to say it is still working.
 * Always true-ish for an ordinary stream (which pairs nothing and needs no gate), so callers
 * must only consult it when app_launch_coop_requested().
 */
bool coop_composite_started(void);

/** Per-stream totals into the kept log (support logs); called when a stream stops. */
void vdec_stats_log_totals(void);

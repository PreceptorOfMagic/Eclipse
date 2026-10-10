#pragma once

#include <stddef.h>
#include <stdint.h>

/* A/V hand-off probe (diagnostic, off unless /tmp/avsync-probe exists). Logs, on one CLOCK_MONOTONIC base, the moment
 * a picture much larger than its neighbours (a full-screen flash) and the moment an audio onset from silence (a click)
 * are handed to the media player, so a host emitting both together shows how far apart streaming delivers them. */
void avsync_probe_video(size_t len);
void avsync_probe_audio(const int16_t *pcm, int frames, int channels, int rate);

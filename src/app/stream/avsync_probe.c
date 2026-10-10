#include "avsync_probe.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

/* Off unless /tmp/avsync-probe exists; re-checked about once a second per stream so it can be armed mid-session.
 * Video and audio run on different threads: each keeps its own state and its own file. */
#define AVSYNC_SENTINEL "/tmp/avsync-probe"
#define AVSYNC_VIDEO_LOG "/tmp/avsync-video.log"
#define AVSYNC_AUDIO_LOG "/tmp/avsync-audio.log"

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000u + (uint64_t) ts.tv_nsec / 1000u;
}

static bool armed(uint64_t *next_check_us, bool *state, uint64_t now) {
    if (now >= *next_check_us) {
        struct stat st;
        *state = stat(AVSYNC_SENTINEL, &st) == 0;
        *next_check_us = now + 1000000u;
    }
    return *state;
}

static void append(const char *path, const char *line) {
    FILE *f = fopen(path, "a");
    if (f != NULL) {
        fputs(line, f);
        fclose(f);
    }
}

/* VIDEO: a frame larger than 3x the running average of the frames before it (and > 60 KB) is a picture-wide change. */
static uint64_t v_next_check, v_frames;
static bool v_on;
static double v_avg;

void avsync_probe_video(size_t len) {
    const uint64_t t = now_us();
    if (!armed(&v_next_check, &v_on, t)) {
        v_frames = 0;
        return;
    }
    const double size = (double) len;
    if (v_frames >= 30 && size > 3.0 * v_avg && size > 60000.0) {
        char line[128];
        snprintf(line, sizeof(line), "video %llu size %zu avg %.0f\n", (unsigned long long) t, len, v_avg);
        append(AVSYNC_VIDEO_LOG, line);
    } else {
        v_avg = v_frames == 0 ? size : v_avg + (size - v_avg) * 0.05;
    }
    v_frames++;
}

/* AUDIO: a sample above 1/8 of full scale at least 50 ms after the previous one is an onset; its time is the hand-off
 * time of its block plus its offset in the block (the block's first sample goes out at the hand-off). Intermediate
 * levels do not reset the gap, so a click's rising edge cannot hide it. */
static uint64_t a_next_check, a_since_loud = UINT64_MAX / 2;
static bool a_on;

void avsync_probe_audio(const int16_t *pcm, int frames, int channels, int rate) {
    const uint64_t t = now_us();
    if (pcm == NULL || frames <= 0 || channels <= 0 || rate <= 0 || !armed(&a_next_check, &a_on, t)) {
        a_since_loud = UINT64_MAX / 2;
        return;
    }
    for (int i = 0; i < frames; ++i) {
        int peak = 0;
        for (int c = 0; c < channels; ++c) {
            const int v = abs(pcm[i * channels + c]);
            if (v > peak) peak = v;
        }
        if (peak > 32768 / 8) {
            if (a_since_loud >= (uint64_t) rate / 20) {
                char line[128];
                snprintf(line, sizeof(line), "audio %llu offset %d gap_ms %llu\n",
                         (unsigned long long) (t + (uint64_t) i * 1000000u / (uint64_t) rate), i,
                         (unsigned long long) (a_since_loud > (uint64_t) rate * 100u ? 99999u : a_since_loud * 1000u / (uint64_t) rate));
                append(AVSYNC_AUDIO_LOG, line);
            }
            a_since_loud = 0;
        } else {
            a_since_loud++;
        }
    }
}
